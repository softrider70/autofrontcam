/*
 * display.c - ILI9341 (240x320) fuer CYD, eigener Minimal-Treiber
 *
 * Hinweis: ESP-IDF 6.x bringt keinen ILI9341-Treiber mehr mit (nur ST7789/SSD1306),
 * daher wird das Display direkt ueber esp_lcd_panel_io_spi angesteuert:
 *   - Kommandos/Parameter:  esp_lcd_panel_io_tx_param()
 *   - Pixeldaten:           esp_lcd_panel_io_tx_color()
 * Farbdaten sind RGB565 im BIG-ENDIAN-Format (Byte 0 = High-Byte), wie es
 * der ILI9341 erwartet. Die JPEG-Dekodierung liefert mit swap_color_bytes=1
 * ebenfalls Big-Endian -> konsistent.
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "config.h"
#include "display.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io = NULL;
static int s_rotation = DISPLAY_ROTATION;

/* RGB565 -> Big-Endian-Speicherformat fuer das Display */
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ------------------------------------------------------------------ */
/* ILI9341-Kommandos                                                  */
/* ------------------------------------------------------------------ */
static void lcd_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

/* Adressfenster setzen + RAMWR aktivieren */
static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t d[4];
    d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)x0;
    d[2] = (uint8_t)(x1 >> 8); d[3] = (uint8_t)x1;
    esp_lcd_panel_io_tx_param(s_io, 0x2A, d, 4);   /* CASET */
    d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)y0;
    d[2] = (uint8_t)(y1 >> 8); d[3] = (uint8_t)y1;
    esp_lcd_panel_io_tx_param(s_io, 0x2B, d, 4);   /* PASET */
    esp_lcd_panel_io_tx_param(s_io, 0x2C, NULL, 0); /* RAMWR */
}

static void ili9341_init_panel(void)
{
    /* Hardware-Reset */
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x01);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Universelle Init-Sequenz: funktioniert mit ILI9341 UND ST7789 (CYD-Varianten).
     * Bewusst KEINE chipspezifischen Gamma-/Power-Register, damit keine Variante
     * durcheinandergebracht wird. */
    lcd_cmd_data(0x3A, (const uint8_t[]){ 0x55 }, 1);   /* Pixel Format 16bpp */
    lcd_cmd_data(0x36, (const uint8_t[]){ 0x08 }, 1);   /* MADCTL: nur BGR (Drehung in Software) */

#if CYD_INVERT_COLOR
    lcd_cmd(0x21);                 /* INVON: invertierte Farben (ST7789-Variante) */
#else
    lcd_cmd(0x20);                 /* INVOFF: normale Farben */
#endif

    lcd_cmd(0x29);                 /* Display ON */
}

/* ------------------------------------------------------------------ */
/* Oeffentliche API                                                    */
/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TFT_CS) | (1ULL << TFT_DC) | (1ULL << TFT_RST) | (1ULL << TFT_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO-Konfiguration fehlgeschlagen");

    gpio_set_level(TFT_CS, 1);
    gpio_set_level(TFT_BL, !TFT_BL_ON);

    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SCK,
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 2 + 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI-Bus init fehlgeschlagen");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_CS,
        .dc_gpio_num = TFT_DC,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,   /* 20 MHz: stabiler als 40 MHz auf CYD-Leiterbahnen */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST, &io_cfg, &s_io),
                        TAG, "Panel-IO init fehlgeschlagen");

    ili9341_init_panel();
    display_fill(0x0000);
    ESP_LOGI(TAG, "ILI9341 initialisiert (%dx%d)", TFT_WIDTH, TFT_HEIGHT);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(TFT_BL, on ? TFT_BL_ON : !TFT_BL_ON);
}

void display_fill(uint16_t color)
{
    if (!s_io) return;
    uint16_t *row = heap_caps_malloc(TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) return;
    uint16_t c = be16(color);
    for (int x = 0; x < TFT_WIDTH; x++) row[x] = c;

    lcd_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    for (int y = 0; y < TFT_HEIGHT; y++) {
        esp_lcd_panel_io_tx_color(s_io, -1, row, TFT_WIDTH * 2);
    }
    heap_caps_free(row);
}

void display_blit_decoded(const uint16_t *src, int src_w, int src_h)
{
    if (!s_io || !src) return;
    if (src_w <= 0 || src_h <= 0) return;

    /* Dimensionen des rotierten Bildes */
    int rw = (s_rotation == 0) ? src_w : src_h;
    int rh = (s_rotation == 0) ? src_h : src_w;
    if (rw <= 0 || rh <= 0) return;

    uint16_t *row = heap_caps_malloc(TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) return;

    for (int dy = 0; dy < TFT_HEIGHT; dy++) {
        int ry = (dy * rh) / TFT_HEIGHT;
        if (ry < 0) ry = 0;
        if (ry >= rh) ry = rh - 1;
        for (int dx = 0; dx < TFT_WIDTH; dx++) {
            int rx = (dx * rw) / TFT_WIDTH;
            if (rx < 0) rx = 0;
            if (rx >= rw) rx = rw - 1;
            int sx, sy;
            if (s_rotation == 1) {            /* CW */
                sx = src_w - 1 - ry;
                sy = rx;
            } else if (s_rotation == 2) {     /* CCW */
                sx = ry;
                sy = src_h - 1 - rx;
            } else {                          /* 0 = ohne Rotation */
                sx = rx;
                sy = ry;
            }
            row[dx] = src[sy * src_w + sx];
        }
        lcd_set_window(0, dy, TFT_WIDTH - 1, dy);
        esp_lcd_panel_io_tx_color(s_io, -1, row, TFT_WIDTH * 2);
    }
    heap_caps_free(row);
}

void display_set_rotation(int rotation)
{
    if (rotation < 0 || rotation > 2) rotation = 1;
    s_rotation = rotation;
}

int display_get_rotation(void)
{
    return s_rotation;
}

/* ------------------------------------------------------------------ */
/* 5x7-Font (Public Domain)                                            */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
    {0x00,0x06,0x09,0x09,0x06}, /* DEL */
};

void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg)
{
    if (!s_io || !text) return;

    int len = (int)strlen(text);
    int total_w = len * 6;   /* 5px Glyphe + 1px Abstand */
    if (total_w <= 0) return;

    uint16_t *row = heap_caps_malloc((size_t)total_w * 2, MALLOC_CAP_DMA);
    if (!row) return;

    uint16_t col_be = be16(color);
    uint16_t bg_be  = be16(bg);

    for (int ry = 0; ry < 7; ry++) {
        for (int i = 0; i < len; i++) {
            /* Font ist spaltenorientiert: 5 Bytes = 5 Spalten, Bit ry = Zeile ry.
             * Nicht-ASCII-Zeichen defensiv auf '?' abbilden. */
            unsigned char ch = (unsigned char)text[i];
            if (ch < 32 || ch > 126) ch = '?';
            for (int c = 0; c < 5; c++) {
                uint8_t col = font5x7[ch - 32][c];
                row[i * 6 + c] = ((col >> ry) & 1) ? col_be : bg_be;
            }
            row[i * 6 + 5] = bg_be;
        }
        int yy = y + ry;
        if (yy < 0 || yy >= TFT_HEIGHT) continue;
        int xx0 = (x < 0) ? 0 : x;
        int ww = total_w;
        if (xx0 + ww > TFT_WIDTH) ww = TFT_WIDTH - xx0;
        if (ww <= 0) continue;
        lcd_set_window(xx0, yy, xx0 + ww - 1, yy);
        esp_lcd_panel_io_tx_color(s_io, -1, row + (xx0 - x), (size_t)ww * 2);
    }
    heap_caps_free(row);
}

void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_io) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    uint16_t *row = heap_caps_malloc((size_t)w * 2, MALLOC_CAP_DMA);
    if (!row) return;
    uint16_t c = be16(color);
    for (int i = 0; i < w; i++) row[i] = c;

    for (int ry = 0; ry < h; ry++) {
        lcd_set_window(x, y + ry, x + w - 1, y + ry);
        esp_lcd_panel_io_tx_color(s_io, -1, row, (size_t)w * 2);
    }
    heap_caps_free(row);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_draw_filled_rect(x, y, w, 1, color);
    display_draw_filled_rect(x, y + h - 1, w, 1, color);
    display_draw_filled_rect(x, y, 1, h, color);
    display_draw_filled_rect(x + w - 1, y, 1, h, color);
}

void display_test_pattern(void)
{
    ESP_LOGI(TAG, "Selbsttest: Rot");
    display_fill(0xF800);
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "Selbsttest: Gruen");
    display_fill(0x07E0);
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "Selbsttest: Blau");
    display_fill(0x001F);
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "Selbsttest: Schwarz");
    display_fill(0x0000);
}
