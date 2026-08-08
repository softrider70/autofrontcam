/*
 * touch.c - XPT2046 resistiver Touch (CYD) auf dem geteilten SPI-Bus
 *
 * Der Touch nutzt denselben SPI2_HOST-Bus wie das Display, aber einen
 * eigenen CS (TOUCH_CS=14). Ablauf: Kommando (24-bit Transaktion: Cmd + 2
 * Datenbytes), 12-bit-Wert aus den letzten 2 Bytes extrahieren.
 */

#include <stdlib.h>
#include "driver/spi_master.h"
#include "esp_log.h"
#include "config.h"
#include "touch.h"

static const char *TAG = "touch";
static spi_device_handle_t s_touch_spi;

esp_err_t touch_init(void)
{
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 1000000,
        .spics_io_num = TOUCH_CS,
        .queue_size = 4,
    };
    esp_err_t err = spi_bus_add_device(TFT_SPI_HOST, &devcfg, &s_touch_spi);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "XPT2046 Touch initialisiert");
    } else {
        ESP_LOGE(TAG, "Touch-Init fehlgeschlagen: %s", esp_err_to_name(err));
    }
    return err;
}

static uint16_t xpt_read(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };
    spi_transaction_t t = {
        .tx_buffer = tx,
        .rx_buffer = rx,
        .length = 24,
        .rxlength = 24,
    };
    if (spi_device_polling_transmit(s_touch_spi, &t) != ESP_OK) return 0;
    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
}

static int map_val(uint16_t v, uint16_t lo, uint16_t hi, int out_max)
{
    if (hi <= lo) return 0;
    long p = ((long)v - lo) * out_max / (hi - lo);
    if (p < 0) p = 0;
    if (p > out_max) p = out_max;
    return (int)p;
}

bool touch_get_point(int *x, int *y)
{
    if (!x || !y || !s_touch_spi) return false;

    /* Druckerkennung ueber Z1/Z2:
     * Ohne Beruehrung lesen Z1/Z2 nahe 0 oder nahe 4095 (offene Leitung).
     * Hinweis: Schwellwerte ggf. am echten Display nachkalibrieren. */
    uint16_t z1 = xpt_read(0xB0);   /* Kanal 3 = Z1 */
    uint16_t z2 = xpt_read(0xC0);   /* Kanal 4 = Z2 */
    if (z1 > 3600 || z2 > 3600 || z1 == 0 || z2 == 0) {
        return false;
    }

    uint16_t raw_x = xpt_read(0xD0);   /* Kanal 5 = X */
    uint16_t raw_y = xpt_read(0x90);   /* Kanal 1 = Y */

    *x = map_val(raw_x, TOUCH_MIN_X, TOUCH_MAX_X, TFT_WIDTH - 1);
    *y = map_val(raw_y, TOUCH_MIN_Y, TOUCH_MAX_Y, TFT_HEIGHT - 1);
    return true;
}
