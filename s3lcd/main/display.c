/*
 * display.c - ST7796S Display-Treiber (ES3C40P, 320x480 SPI, hier quer 480x320)
 *
 * Roher SPI (spi_master, synchron, DC manuell). Pinbelegung aus config.h
 * (VERIFIZIERTES LCDWIKI-Manual). Init-Sequenz ST7796S (Bring-up) - auf der
 * echten Hardware zu verifizieren (Farben/Orientierung).
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "display.h"

static const char *TAG = "s3lcd_disp";

#define CHUNK_MAX   12000   /* max. Bytes pro SPI-Transaktion (DMA) */

static spi_device_handle_t s_spi = NULL;
static bool s_swap = false;          /* Pixel-Bytes getauscht (lo,hi) */
static uint8_t s_madctl_base = 0x60; /* Orientierung ohne BGR-Bit (0x08) */

/* ---------- Low-Level ---------- */

static void lcd_write_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_write_data(const uint8_t *data, size_t len)
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

static void lcd_write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_write_cmd(cmd);
    if (len) {
        lcd_write_data(data, len);
    }
}

/* Setzt das Schreib-Fenster (CASET/RASET) und waehlt RAMWR */
void display_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                     (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t ra[] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                     (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    lcd_write_cmd_data(0x2A, ca, sizeof(ca));   /* CASET */
    lcd_write_cmd_data(0x2B, ra, sizeof(ra));   /* RASET */
    lcd_write_cmd(0x2C);                        /* RAMWR */
}

/* ---------- Init ---------- */

esp_err_t display_init(void)
{
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
    ESP_LOGI(TAG, "ST7796S initialisiert (%dx%d, MADCTL 0x%02X, %d Hz)",
             TFT_WIDTH, TFT_HEIGHT, madctl, (int)TFT_SPI_CLK_HZ);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(TFT_BL, on ? TFT_BL_ON : !TFT_BL_ON);
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

    display_set_window(x, y, x + w - 1, y + h - 1);

    /* Eine Zeile als DMA-Puffer vorbereiten (ST7796S erwartet Bytes
     * big-endian: high byte zuerst) und zeilenweise senden. */
    size_t row_bytes = (size_t)w * 2;
    uint8_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "DMA-Puffer (%u B) fehlgeschlagen", (unsigned)row_bytes);
        return;
    }
    for (size_t i = 0; i + 1 < row_bytes; i += 2) {
        if (s_swap) {
            row[i]     = (uint8_t)(color & 0xFF);
            row[i + 1] = (uint8_t)(color >> 8);
        } else {
            row[i]     = (uint8_t)(color >> 8);
            row[i + 1] = (uint8_t)(color & 0xFF);
        }
    }
    for (int yy = 0; yy < h; yy++) {
        lcd_write_data(row, row_bytes);
    }
    heap_caps_free(row);
}

void display_fill(uint16_t color)
{
    display_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void display_blit(const uint16_t *pixels, int x, int y, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) {
        ESP_LOGW(TAG, "blit ausserhalb (%d,%d %dx%d)", x, y, w, h);
        return;
    }

    display_set_window(x, y, x + w - 1, y + h - 1);

    size_t row_bytes = (size_t)w * 2;
    uint8_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "DMA-Puffer (%u B) fehlgeschlagen", (unsigned)row_bytes);
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        const uint16_t *src = pixels + (size_t)yy * w;
        for (int xx = 0; xx < w; xx++) {
            row[xx * 2]     = (uint8_t)(src[xx] >> 8);   /* high byte zuerst */
            row[xx * 2 + 1] = (uint8_t)(src[xx] & 0xFF);
        }
        lcd_write_data(row, row_bytes);
    }
    heap_caps_free(row);
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
    ESP_LOGI(TAG, "Testpattern ausgegeben (%dx%d)", w, h);
}
