/*
 * touch.c - FT6336U kapazitiver Touch (ES3C40P, I2C Adr 0x38)
 *
 * Nutzt die neue i2c_master-API (IDF 5.2+). Register (FT6x36-Familie):
 *   0x02 TD_STATUS, 0x03/0x04 Touch1 X (11 Bit), 0x05/0x06 Touch1 Y.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"
#include "touch.h"

static const char *TAG = "s3lcd_touch";

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = NULL;

    if (TOUCH_RST >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << TOUCH_RST) | (1ULL << TOUCH_INT),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(TOUCH_RST, 0);
        gpio_set_level(TOUCH_INT, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C-Bus Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_CLK_HZ,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C-Device (0x%02X) fehlgeschlagen: %s",
                 TOUCH_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* Probe: Device Mode Register (0x00) lesen -> Adresse/IC bestaetigen */
    uint8_t reg = 0x00;
    uint8_t mode = 0xFF;
    ret = i2c_master_transmit_receive(s_dev, &reg, 1, &mode, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT6336U antwortet nicht (0x%02X): %s",
                 TOUCH_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "FT6336U gefunden (Adr 0x%02X, Device-Mode 0x%02X)",
             TOUCH_I2C_ADDR, mode);
    return ESP_OK;
}

esp_err_t touch_read(touch_point_t *pt)
{
    if (!pt || !s_dev) return ESP_ERR_INVALID_STATE;

    pt->touched = false;
    uint8_t reg = 0x02;             /* TD_STATUS */
    uint8_t buf[5];                 /* status, xh, xl, yh, yl */
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 50);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t n = buf[0];
    if (n == 0 || n == 0xFF) {
        return ESP_OK;              /* kein Touch */
    }
    pt->raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    pt->raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    pt->touched = true;
    return ESP_OK;
}
