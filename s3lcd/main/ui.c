/*
 * ui.c - Touch-UI fuer den s3lcd Display-Client
 *
 * Zustaende:
 *   IDLE   = normales Videobild + Linien. Tippen aufs Bild -> Funktionsauswahl.
 *   MENU   = 12 gleich grosse Buttons (4x3) ueber den ganzen Bildschirm.
 *   ADJUST = Video + Overlay; Touch wird ausgewertet (X = Wert, Y = drehen).
 *            Nach IDLE_TIMEOUT_MS ohne Touch wird gespeichert (falls geaendert)
 *            und wieder das normale Videobild gezeigt.
 * Kein MENU-Button mehr - das ganze Bild ist die Bedienflaeche.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "config.h"
#include "display.h"
#include "touch.h"
#include "stream.h"
#include "ui.h"

static const char *TAG = "s3lcd_ui";

/* ------------------------------------------------------------------ */
/* Kalibrier-Linien                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     horizontal;  /* false = vertikal, true = horizontal */
    int      pos;         /* Position in % (x von links bzw. y von oben) */
    int      angle;       /* Neigung in Grad (-45..45), Drehung um die Bildmitte */
    int      width;       /* Dicke in Pixel */
    uint16_t color;       /* RGB565 */
} ui_line_t;

#define UI_NLINES 3
static ui_line_t s_lines[UI_NLINES] = {
    { false, 85, 0, 3, 0xF800 },   /* rot:   vertikal  -> rechte Grenze  */
    { true,  12, 0, 3, 0xFFE0 },   /* gelb:  horizontal -> obere Grenze  */
    { false, 50, 0, 3, 0x07E0 },   /* gruen: vertikal  -> Mitte          */
};

/* ------------------------------------------------------------------ */
/* UI-Zustand                                                          */
/* ------------------------------------------------------------------ */
typedef enum { UI_IDLE, UI_MENU, UI_ADJUST } ui_state_t;
static ui_state_t s_state = UI_IDLE;

/* Rollierende Modi (Modus-Button schaltet weiter). Jede Linie hat einen
 * eigenen Positions- und einen eigenen Dicken-Modus. */
typedef enum {
    MOD_ROT, MOD_GRUEN, MOD_GELB,
    MOD_DROT, MOD_DGRUEN, MOD_DGELB,
    MOD_BDREH, MOD_HELL, MOD_KONTRAST, MOD_RECONN, MOD_CAMRESET, MOD_COUNT
} ui_mod_t;
static const char *const mod_lbl[MOD_COUNT] = {   /* kurz: fuer den Button */
    "ROT", "GRUEN", "GELB",
    "DICKE ROT", "DICKE GRUEN", "DICKE GELB",
    "DREH-BILD", "HELLIGKEIT", "KONTRAST", "VERBINDEN", "CAM-RESET" };
static const char *const mod_desc[MOD_COUNT] = {  /* lang: Statuszeile */
    "ROT: X=pos, Y=dreh", "GRUEN: X=pos, Y=dreh", "GELB: X=pos, Y=dreh",
    "DICKE ROT: X", "DICKE GRUEN: X", "DICKE GELB: X",
    "DREH-BILD: Y=drehen", "HELLIGKEIT: X=-/+", "KONTRAST: X=-/+",
    "NEU VERBINDEN: antippen", "CAM-RESET: antippen" };

/* Linienindex zu einem Modus (-1 = keine Linie) */
static int mod_line(int m)
{
    switch (m) {
    case MOD_ROT: case MOD_DROT:     return 0;   /* rot   (vertikal)   */
    case MOD_GELB: case MOD_DGELB:   return 1;   /* gelb  (horizontal) */
    case MOD_GRUEN: case MOD_DGRUEN: return 2;   /* gruen (vertikal)   */
    default: return -1;
    }
}

static int s_mod = MOD_ROT;
static int s_active = 0;            /* aktive Linie (0=rot,1=gelb,2=gruen) */
static int s_imgdeg = 0;            /* Bild-Rotation 0..359 */
static int s_bri = 0;               /* CAM-Helligkeit (-2..2) */
static int s_con = 0;               /* CAM-Kontrast (-2..2) */

static bool s_prev_down = false;    /* Touch war beim letzten Poll aktiv */
static bool s_gest = false;         /* Geste aktiv (Finger auf dem Feld) */
static int  s_axis = 0;             /* 0=noch offen, 1=X (Wert), 2=Y (Drehen) */
static int  s_nx = 0, s_ny = 0;     /* Nullpunkt = letzte Fingerposition */
static int  s_accx = 0, s_accy = 0; /* Summe seit Nullpunkt (Achsenwahl) */
static int  s_rem = 0;              /* Restbetrag fuer gestufte Werte */
static int  s_ang = 0;              /* Restbetrag Winkel (halbe Pixel) */
static int  s_warm = 0;             /* Warmlauf-Samples nach dem Aufsetzen */
static int64_t s_last_redraw = 0;   /* Drossel fuer Zwischen-Redraws */
static bool s_dirty = false;        /* Wert seit der Auswahl geaendert? */
static int64_t s_idle_at = 0;       /* Timeout in den Normalbetrieb (0 = aus) */
static bool s_cam_zero = false;     /* CAM-Bildparameter neutralisiert? */

/* Achsenwahl: erst ab dieser Strecke wird ausgewertet (Totzone gegen Zittern),
 * bei unklarer Richtung erst ab GEST_FORCE. */
#define GEST_DEADZONE 12
#define GEST_FORCE    60

static char s_status[48] = "";
static int64_t s_status_until = 0;

/* ------------------------------------------------------------------ */
/* NVS (eigener Namespace "s3lcd_ui")                                  */
/* ------------------------------------------------------------------ */
static void ui_lines_load(void)
{
    nvs_handle_t h;
    if (nvs_open("s3lcd_ui", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    static const char *pk[UI_NLINES] = { "l0p", "l1p", "l2p" };
    static const char *wk[UI_NLINES] = { "l0w", "l1w", "l2w" };
    static const char *ak[UI_NLINES] = { "l0a", "l1a", "l2a" };
    static const char *hk[UI_NLINES] = { "l0h", "l1h", "l2h" };
    for (int i = 0; i < UI_NLINES; i++) {
        if (nvs_get_u8(h, pk[i], &v) == ESP_OK && v <= 100) s_lines[i].pos = v;
        if (nvs_get_u8(h, wk[i], &v) == ESP_OK && v >= 1 && v <= 15) s_lines[i].width = v;
        if (nvs_get_u8(h, ak[i], &v) == ESP_OK) s_lines[i].angle = (int)v - 45; /* 0..90 -> -45..45 */
        if (nvs_get_u8(h, hk[i], &v) == ESP_OK) s_lines[i].horizontal = (v != 0);
    }
    nvs_close(h);
}

void ui_lines_save(void)
{
    nvs_handle_t h;
    if (nvs_open("s3lcd_ui", NVS_READWRITE, &h) != ESP_OK) return;
    static const char *pk[UI_NLINES] = { "l0p", "l1p", "l2p" };
    static const char *wk[UI_NLINES] = { "l0w", "l1w", "l2w" };
    static const char *ak[UI_NLINES] = { "l0a", "l1a", "l2a" };
    static const char *hk[UI_NLINES] = { "l0h", "l1h", "l2h" };
    for (int i = 0; i < UI_NLINES; i++) {
        nvs_set_u8(h, pk[i], (uint8_t)s_lines[i].pos);
        nvs_set_u8(h, wk[i], (uint8_t)s_lines[i].width);
        nvs_set_u8(h, ak[i], (uint8_t)(s_lines[i].angle + 45)); /* -45..45 -> 0..90 */
        nvs_set_u8(h, hk[i], s_lines[i].horizontal ? 1 : 0);
    }
    /* Bildparameter (client-seitig) mitspeichern */
    nvs_set_u8(h, "bri", (uint8_t)(s_bri + 2));
    nvs_set_u8(h, "con", (uint8_t)(s_con + 2));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Linien gespeichert");
}

/* Helligkeit/Kontrast (client-seitig) aus NVS laden und anwenden */
static void ui_pic_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("s3lcd_ui", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "bri", &v) == ESP_OK && v <= 4) s_bri = (int)v - 2;
        if (nvs_get_u8(h, "con", &v) == ESP_OK && v <= 4) s_con = (int)v - 2;
        nvs_close(h);
    }
    stream_set_picture(s_bri, s_con);
}

/* Helligkeit/Kontrast in NVS sichern */
static void ui_pic_save(void)
{
    nvs_handle_t h;
    if (nvs_open("s3lcd_ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bri", (uint8_t)(s_bri + 2));
    nvs_set_u8(h, "con", (uint8_t)(s_con + 2));
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Buttons (Geometrie)                                                 */
/* ------------------------------------------------------------------ */
/* Statuszeile oben (Bildschirm wird durch das Raster nicht ueberdeckt) */
#define STATUS_H   18

/* Funktions-Raster: 4x3 gleich grosse Buttons ueber den ganzen Bildschirm */
#define GRID_COLS  4
#define GRID_ROWS  3
#define GRID_CW    (TFT_WIDTH / GRID_COLS)
#define GRID_CH    ((TFT_HEIGHT - STATUS_H) / GRID_ROWS)
#define GRID_PAD   3

/* Zeit ohne Touch im Justier-Modus -> speichern + normales Videobild */
#define IDLE_TIMEOUT_MS 5000

/* Button mit zweiter Zeile (Bedienhinweis) */
static void ui_draw_button2(int x, int y, int w, int h, const char *label,
                            const char *hint, uint16_t bg, uint16_t fg)
{
    display_draw_filled_rect(x, y, w, h, bg);
    display_draw_rect(x, y, w, h, 0xFFFF);
    int tw = (int)strlen(label) * 6;
    int ty = y + h / 2 - 11;
    display_draw_text(x + (w - tw) / 2, ty, label, fg, bg);
    if (hint && hint[0]) {
        int hw = (int)strlen(hint) * 6;
        display_draw_text(x + (w - hw) / 2, ty + 13, hint, 0xBDF7, bg);
    }
}

/* Funktionsliste des Rasters (Reihenfolge = Buttonreihenfolge) */
typedef struct {
    const char *lbl;    /* Beschriftung */
    const char *hint;   /* zweite Zeile */
    int         mode;   /* MOD_* oder -1 = Menue schliessen */
} ui_gridbtn_t;

static const ui_gridbtn_t s_gridbtn[GRID_COLS * GRID_ROWS] = {
    { "ROT",         "X=pos Y=dreh", MOD_ROT },
    { "GRUEN",       "X=pos Y=dreh", MOD_GRUEN },
    { "GELB",        "X=pos Y=dreh", MOD_GELB },
    { "DICKE ROT",   "X=dicke",      MOD_DROT },
    { "DICKE GRUEN", "X=dicke",      MOD_DGRUEN },
    { "DICKE GELB",  "X=dicke",      MOD_DGELB },
    { "DREH-BILD",   "Y=drehen",     MOD_BDREH },
    { "HELLIGKEIT",  "X=-/+",        MOD_HELL },
    { "KONTRAST",    "X=-/+",        MOD_KONTRAST },
    { "VERBINDEN",   "antippen",     MOD_RECONN },
    { "CAM-RESET",   "antippen",     MOD_CAMRESET },
    { "ENDE",        "zurueck",      -1 },
};

/* ------------------------------------------------------------------ */
/* Overlay zeichnen (vom Stream nach jedem Video-Blitz aufgerufen)     */
/* ------------------------------------------------------------------ */
/* Eine Kalibrier-Linie zeichnen (vertikal oder horizontal, um die Bildmitte
 * geneigt um l->angle). */
static void ui_draw_one_line(const ui_line_t *l, uint16_t col)
{
    float rad = l->angle * (float)M_PI / 180.0f;
    if (!l->horizontal) {
        /* vertikale Linie: Mittelpunkt (cx, H/2) IM VIDE0BEREICH (rechts der
         * UI-Spalte), Neigung um die Mitte. Oben/unten je 1/20 kuerzer, damit
         * die Linie nicht ueber den Bildinhalt hinausragt. */
        int cx = UI_LEFT_W + (l->pos * (TFT_WIDTH - UI_LEFT_W - 1)) / 100;
        int cy = TFT_HEIGHT / 2;
        int r  = (TFT_HEIGHT * 18) / 20;
        int x1 = cx - (int)(sinf(rad) * r / 2);
        int y1 = cy - (int)(cosf(rad) * r / 2);
        int x2 = cx + (int)(sinf(rad) * r / 2);
        int y2 = cy + (int)(cosf(rad) * r / 2);
        display_draw_line(x1, y1, x2, y2, l->width, col);
    } else {
        /* horizontale Linie: Mittelpunkt (W/2, cy), Neigung um die Mitte */
        int cx = TFT_WIDTH / 2;
        int cy = (l->pos * (TFT_HEIGHT - 1)) / 100;
        int r  = TFT_WIDTH;
        int x1 = cx - (int)(cosf(rad) * r / 2);
        int y1 = cy - (int)(sinf(rad) * r / 2);
        int x2 = cx + (int)(cosf(rad) * r / 2);
        int y2 = cy + (int)(sinf(rad) * r / 2);
        display_draw_line(x1, y1, x2, y2, l->width, col);
    }
}

/* Raster zeichnen: 12 gleich grosse Buttons ueber den ganzen Bildschirm. Der
 * zuletzt benutzte Button ist hervorgehoben. */
static void ui_draw_grid(void)
{
    display_fill_rect(0, STATUS_H, TFT_WIDTH, TFT_HEIGHT - STATUS_H, 0x0000);
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int i  = r * GRID_COLS + c;
            int x  = c * GRID_CW + GRID_PAD;
            int y  = STATUS_H + r * GRID_CH + GRID_PAD;
            int w  = GRID_CW - 2 * GRID_PAD;
            int h  = GRID_CH - 2 * GRID_PAD;
            int m  = s_gridbtn[i].mode;
            uint16_t bg = 0x4208;
            if (m >= 0 && m == s_mod) {
                int li = mod_line(m);
                bg = (li >= 0) ? s_lines[li].color : 0x001F;
            }
            ui_draw_button2(x, y, w, h, s_gridbtn[i].lbl, s_gridbtn[i].hint,
                            bg, (bg == 0x4208) ? 0xFFFF : 0x0000);
        }
    }
}

/* Overlay: Auswahl-Raster ODER Video + Linien. Die Statuszeile wird zuletzt
 * gezeichnet, damit sie immer sichtbar ist. */
void ui_draw_overlay(void)
{
    if (s_state == UI_MENU) {
        ui_draw_grid();
    } else {
        /* Linke UI-Spalte schwarz (Video beginnt erst ab UI_LEFT_W) */
        display_fill_rect(0, 0, UI_LEFT_W, TFT_HEIGHT, 0x0000);
        /* 3 Kalibrier-Linien (im Justier-Modus: aktive Linie weiss) */
        int hl = (s_state == UI_ADJUST) ? mod_line(s_mod) : -1;
        for (int i = 0; i < UI_NLINES; i++) {
            ui_draw_one_line(&s_lines[i], (i == hl) ? 0xFFFF : s_lines[i].color);
        }
    }

    if (s_status[0] && esp_timer_get_time() < s_status_until) {
        display_draw_filled_rect(0, 0, TFT_WIDTH, STATUS_H - 2, 0x0000);
        display_draw_text(6, 5, s_status, 0xFFFF, 0x0000);
    }
}

/* Neu zeichnen + senden (nach Button-Aktionen) */
static void ui_redraw(void)
{
    display_lock();
    ui_draw_overlay();
    display_commit();
    display_unlock();
}

/* Aktueller Bild-Rotationswinkel (fuer den Stream) */
int ui_get_img_deg(void)
{
    return s_imgdeg;
}

/* CAM-Konfiguration remote senden; liefert den HTTP-Status (-1 = Fehler) */
static int ui_cam_set(const char *field, int val)
{
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/api/config",
             CAM_HOST_DEFAULT, CAM_PORT_DEFAULT);
    char body[24];
    snprintf(body, sizeof(body), "%s=%d", field, val);
    esp_http_client_config_t cc = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 1200,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cc);
    if (!h) return -1;
    esp_http_client_set_post_field(h, body, strlen(body));
    esp_err_t e = esp_http_client_perform(h);
    int code = (e == ESP_OK) ? esp_http_client_get_status_code(h) : -1;
    if (e != ESP_OK) ESP_LOGW(TAG, "CAM %s=%d POST fehlgeschlagen", field, val);
    else             ESP_LOGI(TAG, "CAM %s=%d -> HTTP %d", field, val, code);
    esp_http_client_cleanup(h);
    return code;
}

/* ------------------------------------------------------------------ */
/* CAM-Fernreset                                                       */
/* ------------------------------------------------------------------ */
/* Antwort-Body einsammeln. WICHTIG: esp_http_client_perform() liest den
 * Response-Body selbst weg (esp_http_client_read_response liefert danach 0),
 * der Body kommt nur noch im HTTP_EVENT_ON_DATA an -> hier mitschreiben. */
typedef struct {
    char buf[16];
    int  len;
} ui_cam_rsp_t;

static esp_err_t ui_cam_http_event(esp_http_client_event_t *evt)
{
    ui_cam_rsp_t *r = (ui_cam_rsp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r && evt->data && evt->data_len > 0) {
        int space = (int)sizeof(r->buf) - 1 - r->len;
        int n = (evt->data_len < space) ? evt->data_len : space;
        if (n > 0) {
            memcpy(r->buf + r->len, evt->data, (size_t)n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Fernreset der CAM. Rueckgabe: 1 = "RESTART" bestaetigt, 0 = Antwort ohne
 * RESTART (Key unbekannt), -1 = keine/abgebrochene Antwort.
 * Hintergrund: /api/config antwortet auf JEDEN POST mit 200 "OK", der
 * Statuscode allein belegt den Neustart also nicht. */
static int ui_cam_reset(void)
{
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/api/config",
             CAM_HOST_DEFAULT, CAM_PORT_DEFAULT);
    ui_cam_rsp_t rsp = { .buf = "", .len = 0 };
    esp_http_client_config_t cc = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 1500,
        .event_handler = ui_cam_http_event, .user_data = &rsp,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cc);
    if (!h) return -1;
    const char *body = "reset=1";
    esp_http_client_set_post_field(h, body, strlen(body));
    esp_err_t e = esp_http_client_perform(h);
    int res = -1;
    if (e == ESP_OK) {
        int code = esp_http_client_get_status_code(h);
        if (strstr(rsp.buf, "RESTART")) res = 1;
        else if (rsp.len > 0)           res = 0;
        ESP_LOGI(TAG, "CAM reset -> HTTP %d, Antwort \"%s\" -> %s", code, rsp.buf,
                 res == 1 ? "Neustart bestaetigt" : (res == 0 ? "kein RESTART" : "leer"));
    } else {
        ESP_LOGW(TAG, "CAM reset POST fehlgeschlagen: %s", esp_err_to_name(e));
    }
    esp_http_client_cleanup(h);
    return res;
}

/* ------------------------------------------------------------------ */
/* Gesten-Auswertung                                                    */
/* ------------------------------------------------------------------ */

/* Gestufte Umrechnung px -> Schritte. Der Rest wird mitgenommen, sonst gehen
 * kleine Bewegungen verloren. */
static int ui_steps(int dpx, int per)
{
    int v = s_rem + dpx;
    int st = v / per;
    s_rem = v - st * per;
    return st;
}

/* Winkel in 1-Grad-Schritten: 2 px = 1 Grad. */
static int ui_deg_steps(int dpx)
{
    s_ang += dpx;
    int d = s_ang / 2;
    s_ang -= d * 2;
    return d;
}

/* X-Achse: Wert aendern (Position / Dicke / CAM-Wert) */
static void ui_apply_x(int dpx)
{
    ui_line_t *l = &s_lines[s_active];
    int st;
    switch (s_mod) {
    case MOD_ROT:
    case MOD_GRUEN:
    case MOD_GELB:      /* Position: 3 px = 1 % */
        st = ui_steps(dpx, 3);
        if (st) {
            l->pos += st;
            if (l->pos < 0) l->pos = 0;
            if (l->pos > 100) l->pos = 100;
            s_dirty = true;
            ui_set_status("Pos %s %d%%", mod_lbl[s_mod], l->pos);
        }
        break;
    case MOD_DROT:
    case MOD_DGRUEN:
    case MOD_DGELB:     /* Dicke: 30 px = 1 Stufe */
        st = ui_steps(dpx, 30);
        if (st) {
            l->width += st;
            if (l->width < 1) l->width = 1;
            if (l->width > 9) l->width = 9;
            s_dirty = true;
            ui_set_status("Dicke %s %d px", mod_lbl[s_mod], l->width);
        }
        break;
    case MOD_HELL:      /* 60 px = 1 Stufe, CAM -2..2 */
        st = ui_steps(dpx, 60);
        if (st) {
            s_bri += st;
            if (s_bri < -2) s_bri = -2;
            if (s_bri > 2) s_bri = 2;
            stream_set_picture(s_bri, s_con);
            s_dirty = true;
            ui_set_status("Helligkeit %d", s_bri);
        }
        break;
    case MOD_KONTRAST:
        st = ui_steps(dpx, 60);
        if (st) {
            s_con += st;
            if (s_con < -2) s_con = -2;
            if (s_con > 2) s_con = 2;
            stream_set_picture(s_bri, s_con);
            s_dirty = true;
            ui_set_status("Kontrast %d", s_con);
        }
        break;
    default:
        break;
    }
}

/* Y-Achse: drehen in 1-Grad-Schritten (2 px = 1 Grad). Finger hoch = gegen
 * den Uhrzeigersinn. */
static void ui_apply_y(int dpy)
{
    int ddeg = ui_deg_steps(dpy);
    if (!ddeg) return;
    s_dirty = true;

    if (s_mod == MOD_BDREH) {
        /* positives deg = im Bild im Uhrzeigersinn -> hoch muss abziehen */
        s_imgdeg = ((s_imgdeg + ddeg) % 360 + 360) % 360;
        ui_set_status("Bild %d Grad", s_imgdeg);
    } else if (mod_line(s_mod) >= 0) {
        ui_line_t *l = &s_lines[s_active];
        l->angle -= ddeg;              /* hoch (dpy<0) -> Winkel + */
        if (l->angle > 45) l->angle = 45;
        if (l->angle < -45) l->angle = -45;
        ui_set_status("Winkel %s %d Grad", mod_lbl[s_mod], l->angle);
    }
}

/* Gestenposition verarbeiten. Erste Beruehrung (aus ui_touch_down) setzt den
 * Nullpunkt. Die Achse wird erst festgelegt, wenn die Summe seit dem
 * Nullpunkt die Totzone ueberschreitet und eine Richtung klar dominiert -
 * danach ist sie fuer diese Geste gesperrt (nie bewegen UND drehen). */
static void ui_gesture(int x, int y)
{
    if (!s_gest) {
        s_gest = true; s_axis = 0;
        s_accx = 0; s_accy = 0; s_rem = 0; s_ang = 0; s_warm = 2;
        s_nx = x; s_ny = y;
        return;
    }

    if (s_warm > 0) {
        /* Die ersten Werte nur zum Einrasten benutzen: das erste Sample nach
         * dem Aufsetzen springt beim FT6336U gern -> sonst falsche Achse. */
        s_warm--;
        s_nx = x; s_ny = y;
        return;
    }

    s_accx += x - s_nx;
    s_accy += y - s_ny;
    s_nx = x; s_ny = y;          /* Nullpunkt immer nachfuehren (keine Spruenge) */

    if (s_axis == 0) {
        int adx = abs(s_accx), ady = abs(s_accy);
        int tot = adx + ady;
        if (tot < GEST_DEADZONE) return;          /* Totzone: kein Zittern */
        if      (adx > ady * 2) s_axis = 1;       /* klar X */
        else if (ady > adx * 2) s_axis = 2;       /* klar Y */
        else if (tot >= GEST_FORCE) s_axis = (adx >= ady) ? 1 : 2;
        else return;                              /* Richtung noch unklar */
        ESP_LOGI(TAG, "Geste: Achse %s (sum x=%d y=%d)",
                 s_axis == 1 ? "X" : "Y", s_accx, s_accy);
    }

    if (s_axis == 1) ui_apply_x(s_accx);
    else             ui_apply_y(s_accy);
    s_accx = 0; s_accy = 0;

    /* gedrosselt neu zeichnen -> Feedback auch ohne naechstes Stream-Frame */
    int64_t now = esp_timer_get_time();
    if (now - s_last_redraw > 150000) {
        s_last_redraw = now;
        ui_redraw();
    }
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */
void ui_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    s_status_until = esp_timer_get_time() + 3000000;   /* 3 s anzeigen */
}

bool ui_video_paused(void)
{
    /* Video laeuft IMMER (kein pausierendes Menue) -> keine Stream-Stockung */
    return false;
}

bool ui_line_edit_active(void)
{
    return s_state != UI_IDLE;
}

void ui_set_rotation(int rotation)
{
    display_set_rotation(rotation);
    ui_set_status("Rotation %d", rotation);
}

/* ------------------------------------------------------------------ */
/* Touch-Task                                                          */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Touch-Task (Gesten)                                                 */
/* ------------------------------------------------------------------ */

/* Nullpunkt nachziehen (z.B. nach kurzem Touch-Aussetzer) */
static void ui_gest_rebase(int x, int y)
{
    if (!s_gest) return;
    s_nx = x; s_ny = y;
}

/* Button im Raster gedrueckt */
static void ui_grid_press(int idx, int x, int y)
{
    int m = s_gridbtn[idx].mode;

    if (m < 0) {                       /* ENDE -> normales Videobild */
        s_state = UI_IDLE;
        ui_redraw();
        return;
    }

    s_mod = m;
    int li = mod_line(m);
    if (li >= 0) s_active = li;
    s_rem = 0; s_ang = 0;
    s_dirty = false;

    if (m == MOD_RECONN) {
        ESP_LOGI(TAG, "Aktion: Stream neu verbinden");
        stream_request_reconnect();
        ui_set_status("Neu verbinden angefordert");
        s_state = UI_IDLE;
        ui_redraw();
        return;
    }
    if (m == MOD_CAMRESET) {
        int r = ui_cam_reset();
        if (r == 1)      ui_set_status("CAM startet neu (bestaetigt)");
        else if (r == 0) ui_set_status("CAM: keine RESTART-Antwort");
        else             ui_set_status("CAM: keine Antwort");
        s_state = UI_IDLE;
        ui_redraw();
        return;
    }

    /* Justier-Funktion: Video + Overlay, Touch wird ausgewertet */
    ESP_LOGI(TAG, "Justieren: %s", mod_lbl[m]);
    s_state = UI_ADJUST;
    s_idle_at = 0;                     /* laeuft erst nach dem Loslassen */
    ui_set_status("%s", mod_desc[m]);
    ui_gesture(x, y);                  /* Nullpunkt = aktuelle Fingerposition */
    ui_redraw();
}

/* Finger aufgesetzt: Auswahl oeffnen, Button druecken oder justieren */
static void ui_touch_down(int x, int y)
{
    if (s_state == UI_IDLE) {
        /* Tippen aufs Bild -> Funktionsauswahl (dieser Touch oeffnet nur) */
        s_state = UI_MENU;
        ui_set_status("Funktion waehlen");
        ui_redraw();
        return;
    }

    if (s_state == UI_MENU) {
        if (y < STATUS_H) return;                    /* Statuszeile */
        int c = x / GRID_CW;
        int r = (y - STATUS_H) / GRID_CH;
        if (c < 0 || c >= GRID_COLS || r < 0 || r >= GRID_ROWS) return;
        ui_grid_press(r * GRID_COLS + c, x, y);
        return;
    }

    /* UI_ADJUST: Geste starten, Timeout anhalten */
    s_idle_at = 0;
    ui_gesture(x, y);
}

/* Finger bewegt: Geste fortsetzen (kein Full-Commit hier - der Stream rendert
 * jedes Frame mit dem neuen Stand; das spart SPI-Bandbreite) */
static void ui_touch_move(int x, int y)
{
    if (!s_gest) return;
    ui_gesture(x, y);
}

/* Finger losgelassen: Geste beenden und Timeout fuer den Normalbetrieb starten.
 * Gespeichert wird, wenn der Timeout ablaeuft und sich etwas geaendert hat. */
static void ui_touch_up(void)
{
    if (!s_gest) return;
    s_gest = false;
    s_axis = 0;
    if (s_state == UI_ADJUST && s_idle_at == 0) {
        s_idle_at = esp_timer_get_time() + (int64_t)IDLE_TIMEOUT_MS * 1000;
    }
}

static void ui_task(void *arg)
{
    touch_screen_t p;
    int miss = 0;
    for (;;) {
        bool down = (touch_read_screen(&p) == ESP_OK) && p.touched;
        if (down) {
            if (!s_prev_down) {
                ESP_LOGI(TAG, "Touch %d,%d (raw %d,%d)", p.x, p.y, p.rx, p.ry);
                ui_touch_down(p.x, p.y);
            } else if (miss > 0) {
                ui_gest_rebase(p.x, p.y);   /* Aussetzer: Nullpunkt nachziehen */
            } else {
                ui_touch_move(p.x, p.y);
            }
            miss = 0;
            s_prev_down = true;
        } else if (s_prev_down) {
            /* 3 Aussetzer (~60 ms) = wirklich losgelassen (der FT6336U setzt
             * waehrend des Ziehens gelegentlich kurz aus) */
            if (++miss >= 3) {
                ui_touch_up();
                ui_redraw();   /* sofort Feedback (auch bei Stream-Stockung) */
                s_prev_down = false;
                miss = 0;
            }
        }

        /* Justier-Modus: 5 s ohne Touch -> speichern (falls geaendert) und
         * wieder das normale Videobild zeigen */
        if (s_state == UI_ADJUST && !s_gest && s_idle_at &&
            esp_timer_get_time() >= s_idle_at) {
            s_idle_at = 0;
            if (s_dirty) {
                if (mod_line(s_mod) >= 0) ui_lines_save();
                else ui_pic_save();
                s_dirty = false;
                ui_set_status("Gespeichert");
            }
            s_state = UI_IDLE;
            ui_redraw();
        }

        /* CAM-Bildparameter einmalig auf 0 setzen: Helligkeit/Kontrast regelt
         * jetzt der Client (sonst wuerden beide gleichzeitig wirken). */
        if (!s_cam_zero && esp_timer_get_time() > 5000000) {
            s_cam_zero = true;
            ui_cam_set("bri", 0);
            ui_cam_set("con", 0);
            ESP_LOGI(TAG, "CAM-Bildparameter auf 0 (Client regelt Helligkeit/Kontrast)");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_start(void)
{
    ui_lines_load();
    ui_pic_load();
    s_state = UI_IDLE;
    s_mod = MOD_ROT;
    s_active = 0;
    s_rem = 0; s_ang = 0;
    s_dirty = false;
    s_idle_at = 0;
    s_last_redraw = 0;
    s_status_until = 0;
    xTaskCreate(ui_task, "ui", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "UI gestartet (Funktionsraster %dx%d, %d Linien, Helligkeit %d, Kontrast %d)",
             GRID_COLS, GRID_ROWS, UI_NLINES, s_bri, s_con);
}
