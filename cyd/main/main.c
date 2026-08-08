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
    /* Panel-Geometrie ist verifiziert (320x240 Landscape, MADCTL 0x40) - kein
     * Selbsttest beim Boot noetig. Der Geometrie-Test bleibt im Menue (DIAG)
     * erreichbar, um die Orientierung jederzeit zu pruefen. */
#if 0
    display_test_pattern();
    display_draw_text(24, 150, "autofrontcam CYD", 0xFFFF, 0x0000);
#endif

    /* Touch (XPT2046 auf SPI3): Test-Initialisierung. Falls nicht vorhanden,
     * laeuft der CYD trotzdem weiter (SPI ist separat, kein Crash). */
#if 1
    esp_err_t tret = touch_init();
    if (tret != ESP_OK) {
        ESP_LOGW(TAG, "Touch nicht erreichbar - fahre ohne Touch fort");
    }
#endif

    stream_start();
    ui_start();
}
