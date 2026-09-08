/*
 * main.c - tft (ESP32 + 2.8" SPI-Display) - Einstieg
 *
 * Boot-Ablauf: NVS -> Display -> Touch (optional) -> dann je nach
 * TFT_TEST_MODE: Standalone-Test (test.c) ODER Stream+UI (mit Kamera).
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
#include "test.h"

static const char *TAG = "tft_main";

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

    /* Weniger Log-Spam von HTTP-Komponenten: Wenn die CAM zeitweise nicht
     * erreichbar ist, wuerden sonst "Failed to open a new connection"-Fehler
     * das Log und den Terminal-Puffer ueberfluten (und die Touch-Diagnose
     * verstecken). Komplett unterdruecken (ESP_LOG_NONE). */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);

    ESP_ERROR_CHECK(display_init());
    display_backlight(true);

    /* Touch (XPT2046) initialisieren, wenn im config.h aktiviert (TFT_HAVE_TOUCH). */
#if TFT_HAVE_TOUCH
    esp_err_t tret = touch_init();
    if (tret != ESP_OK) {
        ESP_LOGW(TAG, "Touch nicht erreichbar - fahre ohne Touch fort");
    }
#endif

#if TFT_TEST_MODE
    /* Standalone-Test (ohne Kamera/WiFi): Geometrie-/Farb-/Touch-Test
     * (test.c) - solange das Sendermodul (ESP32-CAM) nicht verfuegbar ist. */
    test_start();
#else
    stream_start();
    ui_start();
#endif
}
