/*
 * touch.c - FT6236 kapazitiver Touch (CYD) ueber I2C
 *
 * Der CYD (siehe Referenzprojekt cyd-display-car1) hat einen FT6236
 * I2C-Touch (SDA=GPIO6, SCL=GPIO5) - KEINEN XPT2046-SPI-Touch.
 *
 * Nutzt das NEUE i2c_master.h-API (i2c_new_master_bus/transmit_receive):
 * der alte driver/i2c.h (i2c_driver_install/i2c_master_cmd_begin) panict
 * auf diesem Board (Panic auf APP CPU -> INT-WDT-Reset).
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "touch.h"

static const char *TAG = "touch";

#define FT6236_REG_TD_STATUS  0x02   /* Anzahl aktiver Touches + Touch 1 Daten */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_touch_ok = false;   /* erst true, wenn I2C + FT6236 erfolgreich initialisiert */

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C-Bus fehlgeschlagen");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "I2C-Geraet fehlgeschlagen");

    /* FT6236 pruefen: Touch-Punkt-Register lesen (0x02) */
    uint8_t reg = FT6236_REG_TD_STATUS;
    uint8_t val = 0;
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT6236 antwortet nicht auf I2C - Touch deaktiviert");
        return ret;
    }
    s_touch_ok = true;
    ESP_LOGI(TAG, "FT6236 Touch initialisiert");
    return ESP_OK;
}

bool touch_get_point(int *x, int *y)
{
    if (!s_touch_ok || !x || !y) {
        return false;   /* Touch nicht initialisiert -> kein Touch */
    }

    uint8_t reg = FT6236_REG_TD_STATUS;
    uint8_t data[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, data, 6, pdMS_TO_TICKS(50)) != ESP_OK) {
        return false;
    }
    if (!(data[0] & 0x0F)) {
        return false;   /* kein Touch aktiv */
    }

    uint16_t raw_x = ((data[1] & 0x0F) << 8) | data[2];
    uint16_t raw_y = ((data[3] & 0x0F) << 8) | data[4];

    /* Umrechnung fuer das Porträt-Display (wie im Referenzprojekt verifiziert).
     * Falls die Achsen spiegelverkehrt sind, ggf. anpassen. */
    int dx = TFT_HEIGHT - (int)raw_y;   /* 320 - raw_y */
    int dy = (int)raw_x;
    if (dx < 0) dx = 0;
    if (dx > TFT_WIDTH - 1) dx = TFT_WIDTH - 1;
    if (dy < 0) dy = 0;
    if (dy > TFT_HEIGHT - 1) dy = TFT_HEIGHT - 1;
    *x = dx;
    *y = dy;
    return true;
}
