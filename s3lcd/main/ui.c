/*
 * ui.c - Touch-UI fuer den s3lcd Display-Client
 *
 * 3 Kalibrier-Linien (rot/gelb/gruen; eine horizontal) als Overlay ueber dem
 * Video. Video laeuft IMMER (kein pausierendes Menue -> keine Stream-Stockung).
 * Tap aufs Video oeffnet den Linien-Edit-Modus: schmale, hohe Buttons am
 * LINKEN Rand (in den schwarzen Letterbox-Bereich), rechts/oben frei.
 * Alles wird in den Framebuffer gezeichnet und 1x pro Frame committet.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "display.h"
#include "touch.h"
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
typedef enum { UI_NORMAL, UI_LINES } ui_mode_t;
static ui_mode_t s_mode = UI_NORMAL;
static int  s_active = 0;            /* aktive Linie im Edit-Modus */
static bool s_touched_prev = false;  /* Touch-Entprellung (Edge) */

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
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Linien gespeichert");
}

/* ------------------------------------------------------------------ */
/* Buttons (Geometrie)                                                 */
/* ------------------------------------------------------------------ */
/* 3 GROSSE Tasten am linken Rand (Modus-Taste + 2 Stepper). Das Video ist um
 * UI_LEFT_W nach rechts verschoben -> Buttons liegen im schwarzen Rand. */
#define BTN_X    2
#define BTN_W    (UI_LEFT_W - 4)
#define BTN_H    100
#define BTN_GAP  4
#define BTN_Y(i) (2 + (i) * (BTN_H + BTN_GAP))   /* 2, 106, 210 */
#define UI_EDIT_NBTN 3

/* Tasten */
#define B_MODE   0   /* Modus-Taste (zeigt aktive Linienfarbe + Modus) */
#define B_MINUS  1   /* - */
#define B_PLUS   2   /* + */

/* Untermodi: Die Modus-Taste schaltet zyklisch durch; + / - fuehren aus. */
typedef enum { M_LINE, M_POS, M_DICK, M_LDREH, M_BDREH } ui_sub_t;
static ui_sub_t s_sub = M_LINE;
static int s_imgdeg = 0;   /* Bild-Rotation 0..359 (Kamera-Ausrichtung, client) */

static void ui_draw_button(int x, int y, int w, int h, const char *label,
                           uint16_t bg, uint16_t fg)
{
    display_draw_filled_rect(x, y, w, h, bg);
    display_draw_rect(x, y, w, h, 0xFFFF);
    /* Text zentrieren (bei schmalen Buttons ggf. 5x7 passt: max 7 Zeichen) */
    int tw = (int)strlen(label) * 6;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 7) / 2;
    display_draw_text(tx, ty, label, fg, bg);
}

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

/* Farbnamen: Die Modus-Taste zeigt im Farb-Modus den Namen der aktiven Linie */
static const char *const color_name[3] = { "ROT", "GELB", "GRUEN" };

/* Modus-Labels (Anzeige auf der grossen Modus-Taste) */
static const char *const sub_lbl[5] = { "", "POS", "DICKE", "DREH", "BDREH" };

/* Edit-Buttons: 3 grosse Tasten am linken Rand (Modus + Stepper -/+) */
static void ui_draw_edit_buttons(void)
{
    /* Modus-Taste: Hintergrund = aktive Linienfarbe, Text = Farbname bzw. Modus */
    const char *mlab = (s_sub == M_LINE) ? color_name[s_active] : sub_lbl[s_sub];
    ui_draw_button(BTN_X, BTN_Y(B_MODE), BTN_W, BTN_H,
                   mlab, s_lines[s_active].color, 0x0000);
    ui_draw_button(BTN_X, BTN_Y(B_MINUS), BTN_W, BTN_H, "-", 0x4208, 0xFFFF);
    ui_draw_button(BTN_X, BTN_Y(B_PLUS), BTN_W, BTN_H, "+", 0x4208, 0xFFFF);
}

void ui_draw_overlay(void)
{
    /* Statuszeile (temporaer, oben; Text ab 2/10 damit er nicht von der
     * linken UI-Spalte verdeckt wird) */
    if (s_status[0] && esp_timer_get_time() < s_status_until) {
        display_draw_filled_rect(0, 0, TFT_WIDTH, 14, 0x0000);
        display_draw_text((TFT_WIDTH * 2) / 10, 3, s_status, 0xFFFF, 0x0000);
    }

    /* Linke UI-Spalte immer schwarz halten -> keine Button-Geister, wenn der
     * Edit-Modus verlassen wird (das Video beginnt erst ab UI_LEFT_W). */
    display_fill_rect(0, 0, UI_LEFT_W, TFT_HEIGHT, 0x0000);

    /* 3 Kalibrier-Linien (aktive Linie im Edit-Modus weiss hervorheben) */
    for (int i = 0; i < UI_NLINES; i++) {
        uint16_t col = (s_mode == UI_LINES && i == s_active)
                       ? 0xFFFF : s_lines[i].color;
        ui_draw_one_line(&s_lines[i], col);
    }

    /* Edit-Buttons nur im Linien-Modus (linker Rand) */
    if (s_mode == UI_LINES) {
        ui_draw_edit_buttons();
    }
}

/* Ueber das (ggf. eingefrorene) Video im Framebuffer neu zeichnen + senden.
 * Wird vom Touch-Task nach Aktionen aufgerufen (sofortiges Feedback). */
static void ui_redraw(void)
{
    display_lock();
    ui_draw_overlay();
    display_commit();
    display_unlock();
}

static bool pt_in(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* ------------------------------------------------------------------ */
/* Touch-Aktionen: Screen-Tap toggelt, Modus-Taste + Stepper -/+       */
/* ------------------------------------------------------------------ */

/* Aktueller Bild-Rotationswinkel (fuer den Stream zum Rotieren des Videos) */
int ui_get_img_deg(void)
{
    return s_imgdeg;
}

/* Stepper-Aktion (+1/-1) im aktuellen Untermodus ausfuehren */
static void ui_do_step(int dir)
{
    ui_line_t *l = &s_lines[s_active];
    switch (s_sub) {
    case M_LINE:   /* aktive Linie wechseln */
        s_active = (s_active + dir + UI_NLINES) % UI_NLINES;
        ui_set_status("Linie %s", s_lines[s_active].horizontal ? "oben" : "rechts");
        break;
    case M_POS:    /* verschieben (vertikal: +/-; horizontal: - = hoch, + = runter) */
        l->pos += dir;
        if (l->pos < 0) l->pos = 0;
        if (l->pos > 100) l->pos = 100;
        break;
    case M_DICK:
        l->width += dir;
        if (l->width < 1) l->width = 1;
        if (l->width > 9) l->width = 9;
        break;
    case M_LDREH:  /* Linie drehen */
        l->angle += 5 * dir;
        if (l->angle > 45) l->angle = 45;
        if (l->angle < -45) l->angle = -45;
        ui_set_status("Winkel %d Grad", l->angle);
        break;
    case M_BDREH:  /* Bild schrittweise drehen (schiefe Kamera ausrichten) */
        s_imgdeg = (s_imgdeg + dir + 360) % 360;
        ui_set_status("Bild %d Grad", s_imgdeg);
        break;
    }
}

static void ui_handle_tap(int x, int y)
{
    if (s_mode == UI_NORMAL) {
        /* Tap -> Edit-Modus oeffnen */
        s_mode = UI_LINES;
        ui_set_status("Tap im Bild beendet - Taste waehlt Modus");
        return;
    }

    /* UI_LINES: 3 grosse Tasten links */
    if (pt_in(x, y, BTN_X, BTN_Y(B_MODE), BTN_W, BTN_H)) {
        s_sub = (ui_sub_t)((s_sub + 1) % 5);   /* naechster Untermodus */
        ui_set_status("Modus: %s", sub_lbl[s_sub]);
        return;
    }
    if (pt_in(x, y, BTN_X, BTN_Y(B_MINUS), BTN_W, BTN_H)) {
        ui_do_step(-1);
        return;
    }
    if (pt_in(x, y, BTN_X, BTN_Y(B_PLUS), BTN_W, BTN_H)) {
        ui_do_step(1);
        return;
    }
    /* Tap im Bild -> Edit-Modus beenden (speichern) */
    s_mode = UI_NORMAL;
    ui_lines_save();
    ui_set_status("Linien gespeichert");
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
    return s_mode == UI_LINES;
}

void ui_set_rotation(int rotation)
{
    display_set_rotation(rotation);
    ui_set_status("Rotation %d", rotation);
}

/* ------------------------------------------------------------------ */
/* Touch-Task                                                          */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    touch_screen_t p;
    for (;;) {
        if (touch_read_screen(&p) == ESP_OK && p.touched) {
            if (!s_touched_prev) {
                ui_handle_tap(p.x, p.y);
                ui_redraw();   /* sofort neu zeichnen (auch bei Stream-Stockung) */
            }
            s_touched_prev = true;
        } else {
            s_touched_prev = false;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void ui_start(void)
{
    ui_lines_load();
    s_status_until = 0;
    xTaskCreate(ui_task, "ui", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "UI gestartet (%d Linien, NVS geladen)", UI_NLINES);
}
