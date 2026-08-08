/*
 * touch.c - FT6236 kapazitiver Touch (CYD) ueber I2C
 *
 * Der CYD (siehe Referenzprojekt cyd-display-car1) hat einen FT6236
 * I2C-Touch (SDA=GPIO6, SCL=GPIO5) - KEINEN XPT2046-SPI-Touch.
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "touch.h"

static const char *TAG = "touch";

#define FT6236_REG_TD_STATUS  0x02   /* Anzahl aktiver Touches + Touch 1 Daten */

static bool s_touch_ok = false;   /* erst true, wenn I2C + FT6236 erfolgreich initialisiert */

static esp_err_t ft6236_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t touch_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(TOUCH_I2C_PORT, &conf), TAG, "I2C-Konfiguration fehlgeschlagen");
    ESP_RETURN_ON_ERROR(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0), TAG, "I2C-Treiber fehlgeschlagen");

    uint8_t td = 0;
    esp_err_t ret = ft6236_read_reg(FT6236_REG_TD_STATUS, &td, 1);
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

    uint8_t data[6];
    if (ft6236_read_reg(FT6236_REG_TD_STATUS, data, 6) != ESP_OK) {
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
