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
#include <math.h>
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

/* =====================================================================
 * Kalibrierungslinien (Fahrzeugkanten) auf dem CYD-Display.
 *   rot   = rechter Fahrzeugrand (VERTIKALE Linie, Position x in %)
 *   gelb  = vorderer Fahrzeuganschlag (HORIZONTALE Linie, Position y in %)
 * Winkel -45..45 (Drehung um die Bildmitte), Breite fest 6 px.
 * Werte werden in NVS gespeichert (bleiben nach Neustart erhalten).
 * ===================================================================== */
typedef struct {
    int pos;    /* Position in %% (rot: x von links, gelb: y von oben) */
    int angle;  /* Neigung -45..45 Grad */
    int width;  /* Linienbreite in Pixel (fest 6) */
} ui_line_t;
static ui_line_t s_line_red    = { 85, 0, 6 };   /* rechter Rand */
static ui_line_t s_line_yellow = { 80, 0, 6 };   /* vorderer Anschlag (unten) */
static int s_line_edit = 0;   /* 0 = kein Linien-Modus, 1 = gelb, 2 = rot */

/* Linien-Parameter in NVS laden/speichern */
static void ui_lines_load(void)
{
    s_line_red.pos    = nvs_config_get_u8("line_rx", (uint8_t)s_line_red.pos);
    s_line_red.angle  = (int)nvs_config_get_u8("line_ra", (uint8_t)(s_line_red.angle + 90)) - 90;
    s_line_yellow.pos = nvs_config_get_u8("line_gy", (uint8_t)s_line_yellow.pos);
    s_line_yellow.angle = (int)nvs_config_get_u8("line_ga", (uint8_t)(s_line_yellow.angle + 90)) - 90;
    /* Breite ist fest 6 px (nicht aus NVS ueberschreiben) */
    s_line_red.width = 6;
    s_line_yellow.width = 6;
}
static void ui_line_save(ui_line_t *l, int is_red)
{
    if (is_red) {
        nvs_config_set_u8("line_rx", (uint8_t)l->pos);
        nvs_config_set_u8("line_ra", (uint8_t)(l->angle + 90));
        nvs_config_set_u8("line_rw", (uint8_t)l->width);
    } else {
        nvs_config_set_u8("line_gy", (uint8_t)l->pos);
        nvs_config_set_u8("line_ga", (uint8_t)(l->angle + 90));
        nvs_config_set_u8("line_gw", (uint8_t)l->width);
    }
}

/* ------------------------------------------------------------------ */
/* Touch-Menue-Layout (unterer Bildschirmbereich, 3 Reihen)             */
/* ------------------------------------------------------------------ */
#define MENU_Y      (TFT_HEIGHT - 3 * MENU_BTN_H - 2 * 4)   /* 142 bei 240 hoch */
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
    display_draw_text(TFT_WIDTH - 90, MENU_Y - 14, "Tippen daneben = zu", 0xFFFF, 0x0000);
    /* Reihe 0: Helligkeit + Drehen */
    ui_draw_button(MENU_B1_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "HELL+", 0x001F, 0xFFFF);
    ui_draw_button(MENU_B2_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "HELL-", 0x001F, 0xFFFF);
    ui_draw_button(MENU_B3_X, MENU_Y, MENU_BTN_W, MENU_BTN_H, "DREH", 0x07E0, 0x0000);
    /* Reihe 1: Kalibrierung + Diagnose + GELBE Linie */
    ui_draw_button(MENU_B1_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "TOUCH", 0x7BEF, 0x0000);
    ui_draw_button(MENU_B2_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "TEST", 0x7BEF, 0x0000);
    ui_draw_button(MENU_B3_X, MENU_Y + MENU_BTN_H + 4, MENU_BTN_W, MENU_BTN_H, "GELB", 0xFFE0, 0x0000);
    /* Reihe 2: ROTE Linie + Fertig */
    ui_draw_button(MENU_B1_X, MENU_Y + 2 * (MENU_BTN_H + 4), MENU_BTN_W, MENU_BTN_H, "ROT", 0xF800, 0xFFFF);
    ui_draw_button(MENU_B2_X, MENU_Y + 2 * (MENU_BTN_H + 4), MENU_BTN_W, MENU_BTN_H, "FERTIG", 0x0A8F, 0xFFFF);
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

    /* OSD oben: Version links, fps rechts, Status darunter (KEINE Buttons).
     * Die Zeilen werden VOR dem Text schwarz uebermalt, damit kuerzerer neuer
     * Text (z.B. "Verbunden" nach "CAM weg - Reconnect") die alten Zeichen
     * sauber ueberschreibt und kein Text-Rest stehen bleibt. */
    snprintf(line, sizeof(line), "v0.1.%d", BUILD_NUMBER);
    display_draw_text(2, 2, line, 0xFFFF, 0x0000);
    snprintf(line, sizeof(line), "%lu fps", (unsigned long)stream_get_fps());
    display_draw_filled_rect(TFT_WIDTH - 70, 2, 68, 8, 0x0000);   /* fps-Zeile loeschen */
    display_draw_text(TFT_WIDTH - 70, 2, line, 0xFFFF, 0x0000);
    display_draw_filled_rect(2, 12, TFT_WIDTH - 4, 8, 0x0000);    /* Status-Zeile loeschen */
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
    s_line_edit = 0;
    touch_set_calib_mode(false);
    ui_set_status("Starte...");
}

bool ui_line_edit_active(void)
{
    return s_line_edit != 0;
}

/* Linien-Modus: Rand-Buttons der AKTIVEN Linie auswerten und bewegen.
 * ROT  (s_line_edit==2) -> Buttons am LINKEN Rand (vertikal):  < >  D- D+  X
 * GELB (s_line_edit==1) -> Buttons am UNTEREN Rand (horizontal): ^ v D- D+ X
 * Tippen daneben beendet den Linien-Modus und oeffnet das Menue. */
static void ui_handle_line_tap(int x, int y)
{
    ui_line_t *l = (s_line_edit == 1) ? &s_line_yellow : &s_line_red;
    int is_red = (s_line_edit == 2);
    int slot = -1;

    if (is_red) {
        if (x < 4 || x > 48) { s_line_edit = 0; s_menu_open = true; ui_set_status("Menue"); ui_draw_overlay(); return; }
        slot = (y - 40) / 40;
        if (slot < 0 || slot > 4) { s_line_edit = 0; s_menu_open = true; ui_set_status("Menue"); ui_draw_overlay(); return; }
    } else {
        if (y < 200) { s_line_edit = 0; s_menu_open = true; ui_set_status("Menue"); ui_draw_overlay(); return; }
        slot = (x - 4) / 64;
        if (slot < 0 || slot > 4) { s_line_edit = 0; s_menu_open = true; ui_set_status("Menue"); ui_draw_overlay(); return; }
    }

    switch (slot) {
        case 0:  l->pos--;   break;
        case 1:  l->pos++;   break;
        case 2:  l->angle--; break;
        case 3:  l->angle++; break;
        default: s_line_edit = 0; s_menu_open = true; ui_set_status("Menue"); ui_draw_overlay(); return;
    }
    if (l->pos < 0) l->pos = 0;
    if (l->pos > 100) l->pos = 100;
    if (l->angle < -45) l->angle = -45;
    if (l->angle > 45) l->angle = 45;
    ui_line_save(l, is_red);
    ui_set_status(is_red ? "Rote Linie: x=%d%% A=%d" : "Gelbe Linie: y=%d%% A=%d", l->pos, l->angle);
}

/* Rand-Buttons der AKTIVEN Linie zeichnen (im Linien-Modus, ueber Video):
 * ROT  -> linke Kante (vertikal):   < > D- D+ X
 * GELB -> untere Kante (horizontal): ^ v D- D+ X */
static void ui_draw_edit_buttons(void)
{
    static const char *lred[5]    = { "<", ">", "D-", "D+", "X" };
    static const char *lyellow[5] = { "^", "v", "D-", "D+", "X" };
    const char **lab = (s_line_edit == 2) ? lred : lyellow;
    for (int k = 0; k < 5; k++) {
        if (s_line_edit == 2) {
            ui_draw_button(4, 40 + k * 40, 44, 36, lab[k], 0x0000, 0xFFFF);
        } else {
            ui_draw_button(4 + k * 64, 200, 60, 36, lab[k], 0x0000, 0xFFFF);
        }
    }
}

/* Zeichnet die Kalibrierungslinien (rot/gelb) + Rand-Buttons ueber das Video.
 * Wird vom Stream-Task nach jedem Bild aufgerufen. */
void ui_draw_video_overlay(void)
{
    if (s_diag_mode) return;

    /* Rote Linie: vertikal, Position x%, Neigung um die Bildmitte */    {
        int cx = (s_line_red.pos * TFT_WIDTH) / 100;
        int cy = TFT_HEIGHT / 2;
        float rad = s_line_red.angle * (float)M_PI / 180.0f;
        int half = TFT_HEIGHT / 2;
        int x1 = cx + (int)(half * sinf(rad)), y1 = cy - (int)(half * cosf(rad));
        int x2 = cx - (int)(half * sinf(rad)), y2 = cy + (int)(half * cosf(rad));
        display_draw_line(x1, y1, x2, y2, s_line_red.width, 0xF800);
    }
    /* Gelbe Linie: horizontal, Position y%, Neigung um die Bildmitte */
    {
        int cx = TFT_WIDTH / 2;
        int cy = (s_line_yellow.pos * TFT_HEIGHT) / 100;
        float rad = s_line_yellow.angle * (float)M_PI / 180.0f;
        int half = TFT_WIDTH / 2;
        int x1 = cx - (int)(half * cosf(rad)), y1 = cy - (int)(half * sinf(rad));
        int x2 = cx + (int)(half * cosf(rad)), y2 = cy + (int)(half * sinf(rad));
        display_draw_line(x1, y1, x2, y2, s_line_yellow.width, 0xFFE0);
    }
    /* Nur im Linien-Modus: Rand-Buttons der aktiven Linie (nicht im Hauptbild) */
    if (s_line_edit) ui_draw_edit_buttons();
}

/* Menue-Tipp auswerten (3 Reihen) */
static void ui_handle_menu_tap(int x, int y)
{
    if (y < MENU_Y || y > MENU_Y + 3 * MENU_BTN_H + 2 * 4) return;
    int row = (y < MENU_Y + MENU_BTN_H) ? 0 :
              (y < MENU_Y + 2 * MENU_BTN_H + 4) ? 1 : 2;
    int col = (x < MENU_B2_X) ? 0 : (x < MENU_B3_X) ? 1 : 2;

    if (row == 0) {
        if (col == 0)       ui_send_brightness(1);
        else if (col == 1)  ui_send_brightness(-1);
        else                ui_toggle_rotation();
    } else if (row == 1) {
        if (col == 0)       ui_start_calib();
        else if (col == 1)  ui_show_diag();
        else                { s_line_edit = 1; s_menu_open = false; ui_set_status("Gelbe Linie: Buttons unten"); ui_draw_video_overlay(); }
    } else {
        if (col == 0)       { s_line_edit = 2; s_menu_open = false; ui_set_status("Rote Linie: Buttons links"); ui_draw_video_overlay(); }
        else if (col == 1)  ui_close_menu();
    }
}

/* ------------------------------------------------------------------ */
/* UI-Task                                                             */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    TickType_t last_tap = 0;
    bool was_pressed = false;

    while (1) {
        int x, y;
        if (touch_get_point(&x, &y)) {
            /* Touch-Diagnose: Koordinaten im STATUS (am Display ablesbar) UND im
             * Log, damit die Touch-Position ueberpruefbar bleibt. */
            static TickType_t last_log = 0;
            if ((xTaskGetTickCount() - last_log) >= pdMS_TO_TICKS(300)) {
                ui_set_status("T:%d,%d", x, y);
                ESP_LOGI("ui", "Touch: x=%d y=%d", x, y);
                last_log = xTaskGetTickCount();
            }
            /* Tipp = NEUES AUFSETZEN (Flanke), nicht Bewegung.
             * Die alte Sprung-Erkennung (abs(x-px)>40) verschluckte Button-Tipps,
             * wenn der Finger vom Video zum Button glitt: der Sprung fiel in die
             * 300ms-Entprellzeit ODER es gab nach dem Aufsetzen keinen weiteren
             * Sprung -> Buttons reagierten nicht und die UI schien einzufrieren. */
            if (!was_pressed &&
                (xTaskGetTickCount() - last_tap) > pdMS_TO_TICKS(250)) {
                last_tap = xTaskGetTickCount();
                if (s_diag_mode) {
                    /* Diagnose-Test beenden -> Menue oeffnen */
                    s_diag_mode = false;
                    s_menu_open = true;
                    ui_set_status("Menue");
                    ui_draw_overlay();
                } else if (s_line_edit) {
                    /* Linien-Modus: nur Rand-Buttons der aktiven Linie; daneben -> Menue */
                    ui_handle_line_tap(x, y);
                } else if (s_menu_open) {
                    /* Tap ausserhalb der Menue-Buttons schliesst das Menue */
                    if (y < MENU_Y || x < 4 || x > MENU_B3_X + MENU_BTN_W) {
                        ui_close_menu();
                    } else {
                        ui_handle_menu_tap(x, y);
                    }
                } else {
                    /* Tippen auf das Video oeffnet das Menue */
                    s_menu_open = true;
                    ui_set_status("Menue");
                    ui_draw_overlay();   /* Menue sofort zeichnen */
                }
            }
            was_pressed = true;
        } else {
            was_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ui_start(void)
{
    ui_lines_load();
    xTaskCreate(ui_task, "ui", TASK_STACK_UI, NULL, TASK_PRIORITY_UI, NULL);
}
