/*
 * display.c - ST7796S Display-Treiber (ES3C40P, 320x480 SPI, hier quer 480x320)
 *
 * Roher SPI (spi_master, synchron, DC manuell). Pinbelegung aus config.h
 * (VERIFIZIERTES LCDWIKI-Manual). Init-Sequenz ST7796S (Bring-up) - auf der
 * echten Hardware zu verifizieren (Farben/Orientierung).
 */

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "display.h"

static const char *TAG = "s3lcd_disp";

#define CHUNK_MAX   12000   /* max. Bytes pro SPI-Transaktion (DMA) */

static spi_device_handle_t s_spi = NULL;
static SemaphoreHandle_t s_lcd_mutex = NULL;   /* schuetzt SPI (UI- vs Stream-Task) */
static bool s_swap = false;          /* Pixel-Bytes getauscht (lo,hi) */
static uint8_t s_madctl_base = 0x60; /* Orientierung ohne BGR-Bit (0x08) */
static int s_rotation = 0;           /* 0=ohne, 1=CW, 2=CCW (Bildausrichtung) */

/* Framebuffer in PSRAM (480x320x2 = 300 KB): Video + Overlay werden hier
 * hineingezeichnet und dann 1x pro Frame per display_commit() ans Panel
 * gesendet -> kein Flackern (Overlay ist Teil des Frames). */
static uint16_t *s_fb = NULL;

/* Ausschnitt des Framebuffers ans Panel senden (Teil-Update). */
static void fb_send_rect(int x0, int y0, int x1, int y1);

/* RGB565 -> Big-Endian-Speicherformat fuer das Display (roher SPI-Send) */
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ---------- Low-Level (Mutex-geschuetzt, atomar cmd+data) ---------- */

static void lcd_write_cmd_nolock(uint8_t cmd)
{
    gpio_set_level(TFT_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_write_data_nolock(const uint8_t *data, size_t len)
{
    gpio_set_level(TFT_DC, 1);
    while (len > 0) {
        size_t chunk = len > CHUNK_MAX ? CHUNK_MAX : len;
        spi_transaction_t t = { .length = chunk * 8, .tx_buffer = data };
        spi_device_polling_transmit(s_spi, &t);
        data += chunk;
        len -= chunk;
    }
}

static void lcd_write_cmd(uint8_t cmd)
{
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    lcd_write_cmd_nolock(cmd);
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
}

static void lcd_write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    lcd_write_cmd_nolock(cmd);
    if (len) lcd_write_data_nolock(data, len);
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
}

/* Setzt das Schreib-Fenster (CASET/RASET) und waehlt RAMWR - ohne Mutex. */
static void set_window_nolock(int x0, int y0, int x1, int y1)
{
    uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                     (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t ra[] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                     (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    lcd_write_cmd_nolock(0x2A);             /* CASET */
    lcd_write_data_nolock(ca, sizeof(ca));
    lcd_write_cmd_nolock(0x2B);             /* RASET */
    lcd_write_data_nolock(ra, sizeof(ra));
    lcd_write_cmd_nolock(0x2C);             /* RAMWR */
}

/* Setzt das Schreib-Fenster (CASET/RASET) und waehlt RAMWR (Mutex). */
void display_set_window(int x0, int y0, int x1, int y1)
{
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    set_window_nolock(x0, y0, x1, y1);
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
}

/* ---------- Init ---------- */

esp_err_t display_init(void)
{
    s_lcd_mutex = xSemaphoreCreateMutex();

    /* Framebuffer in PSRAM allokieren (8 MB vorhanden) */
    s_fb = heap_caps_malloc((size_t)TFT_WIDTH * TFT_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "Framebuffer (%d KB PSRAM) fehlgeschlagen",
                 (TFT_WIDTH * TFT_HEIGHT * 2) / 1024);
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .sclk_io_num = TFT_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_MAX,
    };
    esp_err_t ret = spi_bus_initialize(TFT_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Bus Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = TFT_SPI_CLK_HZ,
        .mode = 0,                      /* CPOL=0, CPHA=0 */
        .spics_io_num = TFT_CS,
        .queue_size = 4,
    };
    ret = spi_bus_add_device(TFT_SPI_HOST, &dev, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Device Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TFT_DC) | (1ULL << TFT_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(TFT_DC, 0);
    gpio_set_level(TFT_BL, TFT_BL_ON);

    /* TFT_RST = -1 (CHIP_PU, kein eigener Pin): Software-Reset nutzen. */
    lcd_write_cmd(0x01);                /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_write_cmd(0x11);                /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* ST7796S Basis-Init (Bring-up; auf Hardware verifizieren) */
    uint8_t madctl = ST7796S_MADCTL;
    s_madctl_base = ST7796S_MADCTL & 0xF7;  /* BGR-Bit separat steuerbar */
    uint8_t colmod = 0x55;              /* 16 bpp RGB565 */
    lcd_write_cmd_data(0x36, &madctl, 1);
    lcd_write_cmd_data(0x3A, &colmod, 1);

#if ST7796S_INVERT
    lcd_write_cmd(0x21);                /* INVON - per Farbsweep ermittelt */
#endif
    lcd_write_cmd(0x29);                /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(50));

    display_fill(0x0000);
    display_commit();
    ESP_LOGI(TAG, "ST7796S initialisiert (%dx%d, MADCTL 0x%02X, %d Hz)",
             TFT_WIDTH, TFT_HEIGHT, madctl, (int)TFT_SPI_CLK_HZ);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(TFT_BL, on ? TFT_BL_ON : !TFT_BL_ON);
}

/* Framebuffer-Zeichnen zwischen UI- und Stream-Task serialisieren
 * (Zeichenfunktionen + display_commit muessen unter display_lock laufen). */
void display_lock(void)
{
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
}

void display_unlock(void)
{
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
}

void display_set_byte_swap(bool swap)
{
    s_swap = swap;
}

void display_set_color_mode(bool bgr, bool invert)
{
    uint8_t madctl = (uint8_t)(s_madctl_base | (bgr ? 0x08 : 0x00));
    lcd_write_cmd_data(0x36, &madctl, 1);
    lcd_write_cmd(invert ? 0x21 : 0x20);   /* INVON / INVOFF */
    ESP_LOGI(TAG, "Farbmodus: bgr=%d invert=%d (MADCTL 0x%02X)", bgr ? 1 : 0, invert ? 1 : 0, madctl);
}

void display_set_madctl(uint8_t madctl)
{
    lcd_write_cmd_data(0x36, &madctl, 1);
    ESP_LOGI(TAG, "MADCTL 0x%02X", madctl);
}

/* ---------- Zeichnen ---------- */

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    if (!s_fb) return;

    /* In den Framebuffer schreiben (Panel-Update erst bei display_commit) */
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = s_fb + (size_t)yy * TFT_WIDTH + x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void display_fill(uint16_t color)
{
    display_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void display_blit(const uint16_t *pixels, int x, int y, int w, int h)
{
    if (!pixels || !s_fb) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) {
        ESP_LOGW(TAG, "blit ausserhalb (%d,%d %dx%d)", x, y, w, h);
        return;
    }

    /* In den Framebuffer kopieren (little-endian uint16 im RAM) */
    for (int yy = 0; yy < h; yy++) {
        memcpy(s_fb + (size_t)(y + yy) * TFT_WIDTH + x,
               pixels + (size_t)yy * w, (size_t)w * 2);
    }
}

/* Rotiert ein RGB565-Bild (sw x sh) um seinen Mittelpunkt um 'deg' Grad
 * (0..359) und schreibt es an (dx,dy) in den Framebuffer. Bereiche ausserhalb
 * der Quelle (Ecken) werden schwarz. 8.8-Festkomma (kein FPU -> schnell). */
void display_blit_rotated(const uint16_t *src, int sw, int sh,
                          int dx, int dy, int deg)
{
    if (!s_fb || !src || sw <= 0 || sh <= 0) return;
    if (dx < 0 || dy < 0 || dx + sw > TFT_WIDTH || dy + sh > TFT_HEIGHT) {
        ESP_LOGW(TAG, "blit_rotated ausserhalb (%d,%d %dx%d)", dx, dy, sw, sh);
        return;
    }
    deg = deg % 360;
    if (deg < 0) deg += 360;
    if (deg == 0) {   /* haeufigster Fall: direkte Kopie */
        display_blit(src, dx, dy, sw, sh);
        return;
    }

    const int FP = 65536;   /* 16.16 - cos/sin; Pixel-Offsets bleiben in px */
    float rad = deg * 3.14159265f / 180.0f;
    int cs = (int)(cosf(rad) * FP);
    int sn = (int)(sinf(rad) * FP);
    int cx0 = sw / 2, cy0 = sh / 2;

    for (int yy = 0; yy < sh; yy++) {
        int fy = yy - cy0;
        uint16_t *dst = s_fb + (size_t)(dy + yy) * TFT_WIDTH + dx;
        for (int xx = 0; xx < sw; xx++) {
            int fx = xx - cx0;
            int sx = ((cs * fx + sn * fy) >> 16) + cx0;
            int sy = ((-sn * fx + cs * fy) >> 16) + cy0;
            if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                dst[xx] = src[(size_t)sy * sw + sx];
            else
                dst[xx] = 0x0000;   /* schwarz ausserhalb */
        }
    }
}

/* =====================================================================
 * Zeichenfunktionen (Text/Linien/Rechtecke) - fuer OSD + Kalibrierlinien.
 * Farben RGB565 (native); Bytes werden high-first (big-endian) gesendet
 * (row-Puffer mit be16-Werten roh uebertragen, vgl. display_blit).
 * ===================================================================== */

void display_set_rotation(int rotation)
{
    if (rotation < 0 || rotation > 2) rotation = 1;
    s_rotation = rotation;
}

int display_get_rotation(void)
{
    return s_rotation;
}

void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color)
{
    display_fill_rect(x, y, w, h, color);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_draw_filled_rect(x, y, w, 1, color);
    display_draw_filled_rect(x, y + h - 1, w, 1, color);
    display_draw_filled_rect(x, y, 1, h, color);
    display_draw_filled_rect(x + w - 1, y, 1, h, color);
}

/* 5x7-Font (Public Domain) */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}, {0x00,0x06,0x09,0x09,0x06},
};

void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg)
{
    if (!text || !s_fb) return;
    int len = (int)strlen(text);
    if (len <= 0) return;

    for (int ry = 0; ry < 7; ry++) {
        for (int i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)text[i];
            if (ch < 32 || ch > 126) ch = '?';
            for (int c = 0; c < 5; c++) {
                uint8_t col = font5x7[ch - 32][c];
                int xx = x + i * 6 + c;
                int yy = y + ry;
                if (xx >= 0 && xx < TFT_WIDTH && yy >= 0 && yy < TFT_HEIGHT) {
                    s_fb[(size_t)yy * TFT_WIDTH + xx] =
                        ((col >> ry) & 1) ? color : bg;
                }
            }
            int xx = x + i * 6 + 5;
            int yy = y + ry;
            if (xx >= 0 && xx < TFT_WIDTH && yy >= 0 && yy < TFT_HEIGHT) {
                s_fb[(size_t)yy * TFT_WIDTH + xx] = bg;
            }
        }
    }
}

/* Linie mit Breite (Scanline-Fill): pro Bildzeile EIN Fenster + EIN Transfer.
 * Fuer die Kalibrierungslinien. */
void display_draw_line(int x0, int y0, int x1, int y1, int width, uint16_t color)
{
    if (!s_fb) return;
    if (width < 1) width = 1;

    float dxv = (float)(x1 - x0), dyv = (float)(y1 - y0);
    float len = sqrtf(dxv * dxv + dyv * dyv);
    if (len < 0.5f) return;
    float nx = -dyv / len;   /* senkrecht zur Linienrichtung */
    float ny =  dxv / len;
    float half = (float)width / 2.0f;

    float px[4], py[4];
    px[0] = x0 + nx * half;  py[0] = y0 + ny * half;
    px[1] = x1 + nx * half;  py[1] = y1 + ny * half;
    px[2] = x1 - nx * half;  py[2] = y1 - ny * half;
    px[3] = x0 - nx * half;  py[3] = y0 - ny * half;

    int ymin = (int)floorf(fminf(fminf(py[0], py[1]), fminf(py[2], py[3])));
    int ymax = (int)ceilf(fmaxf(fmaxf(py[0], py[1]), fmaxf(py[2], py[3])));
    if (ymin < 0) ymin = 0;
    if (ymax > TFT_HEIGHT - 1) ymax = TFT_HEIGHT - 1;
    if (ymin > ymax) return;

    /* Scanline-Fill direkt in den Framebuffer */
    for (int y = ymin; y <= ymax; y++) {
        float fy = (float)y;
        float xmin = 1e9f, xmax = -1e9f;
        for (int e = 0; e < 4; e++) {
            int f = (e + 1) & 3;
            float ax = px[e], ay = py[e], bx = px[f], by = py[f];
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                float t = (fy - ay) / (by - ay);
                float x = ax + t * (bx - ax);
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmax < xmin) continue;
        int xi0 = (int)ceilf(xmin - 0.5f);
        int xi1 = (int)floorf(xmax + 0.5f);
        if (xi0 < 0) xi0 = 0;
        if (xi1 > TFT_WIDTH - 1) xi1 = TFT_WIDTH - 1;
        if (xi1 < xi0) continue;
        uint16_t *row = s_fb + (size_t)y * TFT_WIDTH;
        for (int xx = xi0; xx <= xi1; xx++) row[xx] = color;
    }
}

void display_test_pattern(void)
{
    int w = TFT_WIDTH, h = TFT_HEIGHT;

    /* 4 Quadranten: Rot | Gruen / Blau | Schwarz */
    display_fill_rect(0,    0,    w / 2, h / 2, 0xF800);  /* Rot    */
    display_fill_rect(w / 2, 0,    w / 2, h / 2, 0x07E0);  /* Gruen  */
    display_fill_rect(0,    h / 2, w / 2, h / 2, 0x001F);  /* Blau   */
    display_fill_rect(w / 2, h / 2, w / 2, h / 2, 0x0000); /* Schwarz */

    /* Weisser Rahmen (1px) zur Geometrie-Kontrolle */
    display_fill_rect(0, 0, w, 1, 0xFFFF);
    display_fill_rect(0, h - 1, w, 1, 0xFFFF);
    display_fill_rect(0, 0, 1, h, 0xFFFF);
    display_fill_rect(w - 1, 0, 1, h, 0xFFFF);

    /* Orientierungs-Marker: 20px weisses Quadrat oben links (Ecke 0,0) */
    display_fill_rect(4, 4, 20, 20, 0xFFFF);
    display_commit();
    ESP_LOGI(TAG, "Testpattern ausgegeben (%dx%d)", w, h);
}

/* =====================================================================
 * Framebuffer -> Panel (einziger SPI-Burst pro Frame)
 * ===================================================================== */

/* Teilbereich (x0,y0)-(x1,y1) des Framebuffers ans Panel senden. */
static void fb_send_rect(int x0, int y0, int x1, int y1)
{
    if (!s_spi || !s_fb) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= TFT_WIDTH)  x1 = TFT_WIDTH - 1;
    if (y1 >= TFT_HEIGHT) y1 = TFT_HEIGHT - 1;
    if (x1 < x0 || y1 < y0) return;
    int w = x1 - x0 + 1;

    /* 8 Zeilen pro Transfer puffern (Bytes high-first) */
    const int rows = 8;
    uint8_t *buf = heap_caps_malloc((size_t)w * rows * 2, MALLOC_CAP_DMA);
    if (!buf) return;

    /* ACHTUNG: kein Mutex hier - der Aufrufer haelt display_lock() */
    set_window_nolock(x0, y0, x1, y1);
    int y = y0;
    while (y <= y1) {
        int n = (y1 - y + 1 < rows) ? (y1 - y + 1) : rows;
        uint8_t *p = buf;
        for (int r = 0; r < n; r++, y++) {
            const uint16_t *src = s_fb + (size_t)y * TFT_WIDTH + x0;
            for (int xx = 0; xx < w; xx++) {
                *p++ = (uint8_t)(src[xx] >> 8);   /* high byte zuerst */
                *p++ = (uint8_t)(src[xx] & 0xFF);
            }
        }
        lcd_write_data_nolock(buf, (size_t)w * n * 2);
    }
    heap_caps_free(buf);
}

/* Kompletten Framebuffer ans Panel senden (1x pro Frame aufrufen). */
void display_commit(void)
{
    fb_send_rect(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
}
