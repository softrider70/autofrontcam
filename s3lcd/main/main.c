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
#include "esp_timer.h"
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

    display_fill(0x0000);
    ESP_LOGI(TAG, "Touch-Kalibrierung: weisser Marker folgt dem Finger.");
    ESP_LOGI(TAG, "Fuehr den Finger zu allen 4 Ecken + Mitte und pruef, ob der Marker korrekt folgt.");

    touch_screen_t p;
    const int CS = 18;              /* Cursor-Groesse */
    int prev_x = -1, prev_y = -1;
    for (;;) {
        if (touch_read_screen(&p) == ESP_OK && p.touched) {
            int cx = p.x - CS / 2;
            int cy = p.y - CS / 2;
            if (prev_x >= 0) {
                display_fill_rect(prev_x, prev_y, CS, CS, 0x0000);  /* alten Cursor loeschen */
            }
            display_fill_rect(cx, cy, CS, CS, 0xFFFF);
            prev_x = cx;
            prev_y = cy;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
