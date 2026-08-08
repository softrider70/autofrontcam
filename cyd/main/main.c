/*
 * main.c - CYD (Cheap Yellow Display) - Einstieg
 *
 * Boot-Ablauf: NVS -> Display -> Touch -> Stream-Task (WiFi+JPEG) -> UI-Task
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs_config.h"
#include "config.h"
#include "version.h"
#include "display.h"
#include "touch.h"
#include "stream.h"
#include "ui.h"

static const char *TAG = "cyd_main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== %s (%s), Chip %s ===", BOARD_NAME, APP_VERSION_STRING, BOARD_CHIP);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    nvs_config_init();

    ESP_ERROR_CHECK(display_init());
    display_backlight(true);
    /* Diagnose-Selbsttest: Rot -> Gruen -> Blau -> Schwarz
     * (zeigt, ob Pixeldaten ankommen und ob Farben invertiert sind) */
    display_test_pattern();
    display_draw_text(24, 150, "autofrontcam CYD", 0xFFFF, 0x0000);

    /* Touch ist optional: Wenn der FT6236 nicht antwortet, trotzdem weiterstarten */
    esp_err_t tret = touch_init();
    if (tret != ESP_OK) {
        ESP_LOGW(TAG, "Touch nicht erreichbar - fahre ohne Touch fort");
    }

    stream_start();
    ui_start();
}
