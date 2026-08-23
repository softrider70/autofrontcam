/*
 * main.c - tft (ESP32 + 2.8" SPI-Display) - Einstieg
 *
 * Boot-Ablauf: NVS -> Display -> (Selbsttest) -> Touch (optional) ->
 * Stream-Task (WiFi+JPEG) -> UI-Task
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

    /* Beim tft-Bring-up ist die Panel-Geometrie/Orientierung NOCH NICHT
     * verifiziert -> Selbsttest beim Boot AKTIV: Rot -> Gruen -> Blau ->
     * Schwarz, danach Text. So sind Verkabelung, Init und Farbordnung sofort
     * beurteilbar. Nach erfolgreicher Verifikation auf #if 0 stellen. */
#if 1
    display_test_pattern();
    display_draw_text(24, 150, "autofrontcam tft", 0xFFFF, 0x0000);
    vTaskDelay(pdMS_TO_TICKS(2500));
#endif

    /* Touch (XPT2046) nur initialisieren, wenn im config.h aktiviert
     * (TFT_HAVE_TOUCH). Default AUS -> Display-Bring-up bleibt der Fokus. */
#if TFT_HAVE_TOUCH
    esp_err_t tret = touch_init();
    if (tret != ESP_OK) {
        ESP_LOGW(TAG, "Touch nicht erreichbar - fahre ohne Touch fort");
    }
#endif

    stream_start();
    ui_start();
}
