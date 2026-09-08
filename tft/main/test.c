/*
 * test.c - tft Standalone-Testmodul (ohne Sendermodul/Kamera/WiFi)
 *
 * Solange der ESP32-CAM (Sendermodul) nicht verfuegbar ist, zeigt dieses
 * Modul einen eigenstaendigen Display-/Touch-Test auf dem 2.8"-Panel:
 *   1. display_test_pattern(): Geometrie-Test (4 farbige Quadranten,
 *      Rahmen, Kreuz, Eckmarker 1..4) - verifiziert Orientierung/Farben.
 *   2. Farbbalken (RGBW) + Schriftprobe inkl. Versionsstring.
 *   3. Touch-Live-Test (falls TFT_HAVE_TOUCH): Antippen zeichnet einen
 *      Marker und loggt die Koordinaten.
 * Aktiv ueber TFT_TEST_MODE=1 in include/config.h.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "version.h"
#include "display.h"
#include "touch.h"
#include "test.h"

static const char *TAG = "test";

/* Farbbalken (RGBW) + Schriftprobe */
static void test_farbbalken(void)
{
    display_fill(0x0000);
    const int bw = TFT_WIDTH / 4, bh = 34, y0 = 6;
    static const struct { const char *lbl; uint16_t color; } bars[4] = {
        { "R", 0xF800 }, { "G", 0x07E0 }, { "B", 0x001F }, { "W", 0xFFFF },
    };
    for (int i = 0; i < 4; i++) {
        int x0 = i * bw;
        display_draw_filled_rect(x0, y0, bw, bh, bars[i].color);
        uint16_t lc = (bars[i].color == 0xFFFF) ? 0x0000 : 0xFFFF;
        display_draw_text(x0 + 6, y0 + bh / 2 - 4, bars[i].lbl, lc, bars[i].color);
    }
    display_draw_text(2, 52, "autofrontcam tft", 0xFFFF, 0x0000);
    display_draw_text(2, 64, "TEST-MODUL (ohne CAM)", 0xFFFF, 0x0000);
    display_draw_text(2, 76, APP_VERSION_STRING, 0x07FF, 0x0000);
    display_draw_text(2, 88, "Orientierung: 240x320 Portrait", 0xFFFF, 0x0000);
    ESP_LOGI(TAG, "Farbbalken + Text (Version %s) angezeigt", APP_VERSION_STRING);
}

static void test_task(void *arg)
{
    ESP_LOGI(TAG, "TFT-Testmodul gestartet (ohne Kamera/WiFi)");

    /* 1) Geometrie-/Orientierungs-Test */
    display_test_pattern();
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* 2) Farbbalken + Schrift */
    test_farbbalken();
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* 3) Touch-Live-Test (Dauerschleife) */
    display_fill(0x0000);
    display_draw_text(2, 4, "Touch-Test - Antippen!", 0xFFFF, 0x0000);
    char coord[24] = "kein Kontakt";
    int last_x = -1, last_y = -1;
    for (;;) {
        int x = 0, y = 0;
        if (touch_get_point(&x, &y)) {
            snprintf(coord, sizeof(coord), "x=%3d y=%3d", x, y);
            /* Gruener Marker an der Antipp-Position */
            display_draw_filled_rect(x - 6, y - 6, 13, 13, 0x07E0);
            ESP_LOGI(TAG, "Touch: x=%d y=%d", x, y);
            last_x = x;
            last_y = y;
        }
        /* Statuszeilen neu zeichnen (ueberschreiben alten Text) */
        display_draw_text(2, 20, coord, 0x07FF, 0x0000);
        if (last_x >= 0) {
            char p[24];
            snprintf(p, sizeof(p), "letzter: %d/%d", last_x, last_y);
            display_draw_text(2, 32, p, 0xFFFF, 0x0000);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void test_start(void)
{
    xTaskCreate(test_task, "test", 4096, NULL, 5, NULL);
}
