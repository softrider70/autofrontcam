/*
 * touch.c - XPT2046 resistiver Touch (CYD) ueber SPI (separater Bus SPI3)
 *
 * Belegung aus dem Referenzprojekt cyd-display-car1/src/touch_test.c:
 * MOSI=32, MISO=39, CLK=25, CS=33, IRQ=36. Der CYD hat sehr wahrscheinlich
 * einen XPT2046 (resistiv), NICHT FT6236: GPIO 6 als I2C-SDA ist auf dem
 * ESP32-WROOM ein Flash-SPI-Pin und hardwaremaessig nicht nutzbar.
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "touch.h"

static const char *TAG = "touch";

/* XPT2046 Kanal-Kommandos (S=1, A2A1A0, MODE=0, SER=0, PD=00) */
#define XPT_CMD_Z1   0xB0   /* Druck */
#define XPT_CMD_Z2   0xC0   /* Druck */
#define XPT_CMD_X    0x90   /* X-Position */
#define XPT_CMD_Y    0xD0   /* Y-Position */
#define XPT_CMD_PWD  0x80   /* Power-Down */

static spi_device_handle_t s_touch_spi = NULL;
static bool s_touch_ok = false;   /* true, sobald SPI-Treiber initialisiert */
static bool s_calib_mode = false; /* Kalibrier-Modus (Rohwerte loggen) */

void touch_set_calib_mode(bool on)
{
    s_calib_mode = on;
    ESP_LOGI(TAG, "Kalibrier-Modus %s", on ? "EIN (Rohwerte werden geloggt)" : "AUS");
}

bool touch_calib_mode(void)
{
    return s_calib_mode;
}

static uint16_t touch_read_channel(uint8_t cmd)
{
    if (!s_touch_spi) return 0;
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };
    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_transmit(s_touch_spi, &t) != ESP_OK) {
        return 0;
    }
    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
}

esp_err_t touch_init(void)
{
    /* IRQ-Pin (Eingang) */
    gpio_config_t irq = {
        .pin_bit_mask = (1ULL << TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "Touch-IRQ-GPIO fehlgeschlagen");

    /* Separater SPI-Bus (kein DMA noetig, 3-Byte-Transaktionen) */
    spi_bus_config_t bus = {
        .mosi_io_num = TOUCH_MOSI,
        .miso_io_num = TOUCH_MISO,
        .sclk_io_num = TOUCH_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TOUCH_SPI_HOST, &bus, SPI_DMA_DISABLED), TAG, "Touch-SPI-Bus fehlgeschlagen");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2500000,   /* 2.5 MHz wie im Referenzprojekt */
        .mode = 0,
        .spics_io_num = TOUCH_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TOUCH_SPI_HOST, &dev, &s_touch_spi), TAG, "Touch-SPI-Geraet fehlgeschlagen");

    /* Diagnose: Z1 lesen (Druck-Kanal). Bei angeschlossenem XPT2046 liefert
     * das Board Werte; andernfalls 0 -> Touch vorhanden aber evtl. andere Pins. */
    uint16_t z1 = touch_read_channel(XPT_CMD_Z1);
    touch_read_channel(XPT_CMD_PWD);
    s_touch_ok = true;
    ESP_LOGI(TAG, "XPT2046 SPI initialisiert (Z1-Diagnose: %u)", (unsigned)z1);
    return ESP_OK;
}

bool touch_get_point(int *x, int *y)
{
    if (!s_touch_ok || !x || !y) {
        return false;
    }

    /* Druck pruefen (Z1 vs Z2) */
    uint16_t z1 = touch_read_channel(XPT_CMD_Z1);
    uint16_t z2 = touch_read_channel(XPT_CMD_Z2);
    int pressure = (z1 > 0) ? ((int)z1 - (int)z2 + 4095) : 0;
    if (pressure < 0) pressure = 0;

    /* Kein Druck: nichts loggen (Diag-Logs waren nur fuer die Erkennungs-
     * Diagnose; Kalibrier-Rohwerte kommen ueber den KALIB-Zweig). */
    if (pressure < 50) {
        touch_read_channel(XPT_CMD_PWD);
        return false;   /* kein Druck -> kein Touch */
    }

    uint16_t x_raw = touch_read_channel(XPT_CMD_X);
    uint16_t y_raw = touch_read_channel(XPT_CMD_Y);
    touch_read_channel(XPT_CMD_PWD);

    /* KALIBRIER-MODUS (aus dem Touch-Menue aktiviert): Rohwerte loggen.
     * Koordinaten werden TROTZDEM geliefert, damit das Menue bedienbar bleibt
     * (z.B. um den Kalibrier-Modus wieder zu beenden). */
    if (s_calib_mode) {
        static TickType_t last_cal = 0;
        if ((xTaskGetTickCount() - last_cal) >= pdMS_TO_TICKS(500)) {
            ESP_LOGI(TAG, "KALIB: x_raw=%u y_raw=%u", (unsigned)x_raw, (unsigned)y_raw);
            last_cal = xTaskGetTickCount();
        }
    }

    /* 12-bit Rohwerte -> Display-Koordinaten (Basis-Schätzung, wird nach der
     * Kalibrierung durch die echte Matrix ersetzt). */
    int dx = (int)(((uint32_t)x_raw * TFT_WIDTH) / 4096);
    int dy = (int)(((uint32_t)y_raw * TFT_HEIGHT) / 4096);
    if (dx < 0) dx = 0;
    if (dx > TFT_WIDTH - 1) dx = TFT_WIDTH - 1;
    if (dy < 0) dy = 0;
    if (dy > TFT_HEIGHT - 1) dy = TFT_HEIGHT - 1;
    *x = dx;
    *y = dy;
    return true;
}
