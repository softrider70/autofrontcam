/*
 * ui.c - OSD + Touch-Menue (Helligkeit/Rotation/Kalibrierung/Diagnose)
 *
 * Design: Das Kamerabild ist vollflaechig, oben nur eine schmale OSD-Leiste
 * (Version/fps/Status). KEINE dauerhaften Buttons mehr - ein Tippen auf das
 * Display oeffnet das Touch-Menue mit Buttons (BRI+/BRI-/ROT) und den
 * Funktionen Kalibrieren (XPT2046-Rohwerte loggen) und Diagnose (Panel-
 * Geometrie-Test fuer das "1/4 fehlend"-Problem). Die Buttons senden die
 * Bildparameter per HTTP-POST an die Config-API des ESP32-CAM (/api/config).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_config.h"
#include "config.h"
#include "version.h"
#include "display.h"
#include "touch.h"
#include "stream.h"
#include "ui.h"

static char s_status[40] = "Starte...";
static int s_brightness = 0;
static bool s_menu_open = false;
static bool s_diag_mode = false;   /* Panel-Geometrie-Test aktiv */

/* ------------------------------------------------------------------ */
/* Touch-Menue-Layout (unterer Bildschirmbereich, 320x240)             */
/* ------------------------------------------------------------------ */
#define MENU_Y      (TFT_HEIGHT - 80)   /* 160 bei 240 hoch */
#define MENU_BTN_H  30
#define MENU_BTN_W  ((TFT_WIDTH - 16) / 3)   /* ~101 bei 320 breit */
#define MENU_B1_X   4
#define MENU_B2_X   (MENU_B1_X + MENU_BTN_W + 4)
#define MENU_B3_X   (MENU_B2_X + MENU_BTN_W + 4)

static void ui_draw_button(int x, int y, int w, int h, const char *label,
                           uint16_t bg, uint16_t fg)
{
    display_draw_filled_rect(x, y, w, h, bg);
    display_draw_rect(x, y, w, h, 0xFFFF);
    int lw = (int)strlen(label) * 6;
    int lx = x + (w - lw) / 2;
    if (lx < x + 2) lx = x + 2;
    display_draw_text(lx, y + (h - 7) / 2, label, fg, bg);
}

static void ui_draw_menu(void)
{
    display_draw_text(2, MENU_Y - 14, "Menue", 0xFFFF, 0x0000);
    /* Reihe 0: BRI+ BRI- ROT */
    ui_draw_button(MENU_B1_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "BRI+", 0x001F, 0xFFFF);
    ui_draw_button(MENU_B2_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "BRI-", 0x001F, 0xFFFF);
    ui_draw_button(MENU_B3_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "ROT", 0x07E0, 0x0000);
    /* Reihe 1: KALIB DIAG ZU */
    ui_draw_button(MENU_B1_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "KALIB", 0x7BEF, 0x0000);
    ui_draw_button(MENU_B2_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "DIAG", 0x7BEF, 0x0000);
    ui_draw_button(MENU_B3_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "ZU", 0xF800, 0xFFFF);
}

void ui_set_status(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, args);
    va_end(args);
}

void ui_draw_overlay(void)
{
    char line[40];

    /* Diagnose-Test aktiv: nichts ueber den Geometrie-Test zeichnen */
    if (s_diag_mode) return;

    /* OSD oben: Version links, fps rechts, Status darunter (KEINE Buttons) */
    snprintf(line, sizeof(line), "v0.1.%d", BUILD_NUMBER);
    display_draw_text(2, 2, line, 0xFFFF, 0x0000);
    snprintf(line, sizeof(line), "%lu fps", (unsigned long)stream_get_fps());
    display_draw_text(TFT_WIDTH - 70, 2, line, 0xFFFF, 0x0000);
    display_draw_text(2, 12, s_status, 0xFFFF, 0x0000);

    if (s_menu_open) {
        ui_draw_menu();
    }
}

bool ui_menu_is_open(void)
{
    return s_menu_open;
}

bool ui_diag_is_active(void)
{
    return s_diag_mode;
}

/* ------------------------------------------------------------------ */
/* CAM-Steuerung (POST an /api/config)                                 */
/* ------------------------------------------------------------------ */
static void ui_send_brightness(int delta)
{
    s_brightness += delta;
    if (s_brightness < -2) s_brightness = -2;
    if (s_brightness > 2)  s_brightness = 2;

    char host[40] = CAM_HOST_DEFAULT;
    char *h = nvs_config_get_str("cyd_host", "");
    if (h && strlen(h) > 0) { strncpy(host, h, sizeof(host) - 1); host[sizeof(host) - 1] = 0; }
    if (h) free(h);

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, CAM_PORT_DEFAULT, CAM_API_PATH);
    char body[16];
    snprintf(body, sizeof(body), "bri=%d", s_brightness);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    ui_set_status("Helligkeit %d", s_brightness);
}

static void ui_toggle_rotation(void)
{
    int r = (display_get_rotation() == 1) ? 2 : 1;
    display_set_rotation(r);
    ui_set_status("Drehung %s", (r == 1) ? "CW" : "CCW");
}

/* Kalibrier-Modus: Rohwerte werden geloggt (KALIB: x_raw=... y_raw=...).
 * Nutzer drueckt auf die 4 Ecken -> Werte ablesen -> Matrix in touch.c setzen. */
static void ui_start_calib(void)
{
    touch_set_calib_mode(true);
    ui_set_status("KALIB: Ecken druecken");
}

/* Diagnose: Panel-Geometrie-Test (4 Quadranten + Marker) fuer das
 * "1/4 fehlend"-Problem. Test bleibt stehen, bis ein Touch ihn beendet. */
static void ui_show_diag(void)
{
    s_diag_mode = true;
    s_menu_open = false;
    touch_set_calib_mode(false);
    display_test_pattern();
    ui_set_status("Diagnose-Test (Tippen=weiter)");
}

static void ui_close_menu(void)
{
    s_menu_open = false;
    touch_set_calib_mode(false);
    ui_set_status("Starte...");
}

/* Menue-Tipp auswerten */
static void ui_handle_menu_tap(int x, int y)
{
    if (y < MENU_Y || y > MENU_Y + 2 * MENU_BTN_H + 4) return;
    int row = (y < MENU_Y + MENU_BTN_H) ? 0 : 1;
    int col = (x < MENU_B2_X) ? 0 : (x < MENU_B3_X) ? 1 : 2;

    if (row == 0) {
        if (col == 0)       ui_send_brightness(1);
        else if (col == 1)  ui_send_brightness(-1);
        else                ui_toggle_rotation();
    } else {
        if (col == 0)       ui_start_calib();
        else if (col == 1)  ui_show_diag();
        else                ui_close_menu();
    }
}

/* ------------------------------------------------------------------ */
/* UI-Task                                                             */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    int px = -1, py = -1;
    TickType_t last_tap = 0;
    bool was_pressed = false;

    while (1) {
        int x, y;
        if (touch_get_point(&x, &y)) {
            /* Touch-Diagnose: Koordinaten im STATUS (am Display ablesbar) UND im
             * Log, damit der bewusste Touch-Test zuverlaessig ausgewertet werden
             * kann (das Log wird sonst von HTTP-Fehlern ueberflutet). */
            static TickType_t last_log = 0;
            if ((xTaskGetTickCount() - last_log) >= pdMS_TO_TICKS(300)) {
                ui_set_status("T:%d,%d", x, y);
                ESP_LOGI("ui", "Touch: x=%d y=%d", x, y);
                last_log = xTaskGetTickCount();
            }
            /* Tipp erkennen: groesser Koordinatensprung + Entprellzeit */
            if ((abs(x - px) > 40 || abs(y - py) > 40) &&
                (xTaskGetTickCount() - last_tap) > pdMS_TO_TICKS(300)) {
                last_tap = xTaskGetTickCount();
                if (s_diag_mode) {
                    /* Diagnose-Test beenden -> Menue oeffnen */
                    s_diag_mode = false;
                    s_menu_open = true;
                    ui_set_status("Menue");
                    ui_draw_overlay();
                } else if (s_menu_open) {
                    ui_handle_menu_tap(x, y);
                } else {
                    /* Tippen auf das Video oeffnet das Menue */
                    s_menu_open = true;
                    ui_set_status("Menue");
                    ui_draw_overlay();   /* Menue sofort zeichnen */
                }
            }
            was_pressed = true;
            px = x; py = y;
        } else {
            was_pressed = false;
            px = -1; py = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", TASK_STACK_UI, NULL, TASK_PRIORITY_UI, NULL);
}
