/*
 * main.c - s3lcd (LCDWIKI ES3C40P) - Bring-up
 *
 * Boot-Ablauf (Testmodus S3LCD_TEST_MODE=1):
 *   NVS -> Display (ST7796S) -> Backlight -> Farb-Selbsttest ->
 *   Touch (FT6336U) -> Rohwerte loggen.
 *
 * Sobald Display + Touch auf Hardware laufen, wird hier der
 * Display-Client (WiFi + Kamera-Stream) analog tft/cyd ergaenzt.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "config.h"
#include "version.h"
#include "display.h"
#include "touch.h"

static const char *TAG = "s3lcd_main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== %s (%s), Version %s ===", BOARD_NAME, BOARD_CHIP, APP_VERSION_STRING);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(display_init());
    display_backlight(true);
    display_test_pattern();

    esp_err_t tret = touch_init();
    if (tret != ESP_OK) {
        ESP_LOGW(TAG, "Touch nicht erreichbar - fahre ohne Touch fort");
    }

    ESP_LOGI(TAG, "Bring-up laeuft. Farbtest am Display, Touch-Rohwerte im Log.");
    touch_point_t pt;
    for (;;) {
        if (touch_read(&pt) == ESP_OK && pt.touched) {
            ESP_LOGI(TAG, "Touch: raw_x=%d raw_y=%d", pt.raw_x, pt.raw_y);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
