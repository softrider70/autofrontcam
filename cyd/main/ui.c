/*
 * ui.c - OSD + Touch-Buttons (Helligkeit/Rotation) + CAM-Steuerung
 *
 * Die Buttons senden Bildparameter per HTTP-POST an die Config-API des
 * ESP32-CAM (/api/config, Form-Feld "bri"), damit Helligkeit auf dem
 * Kameramodul selbst wirkt (nicht nur am Display).
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

/* Button-Flaechen (Display-Koordinaten, 240x320) - unten, unterhalb des
 * Videobereichs (Video endet bei TFT_HEIGHT - UI_BTN_H, d.h. hier y=282). */
#define BTN_Y   (TFT_HEIGHT - UI_BTN_H)
#define BTN_H   34
#define BTN1_X0 4
#define BTN2_X0 84
#define BTN3_X0 164

void ui_set_status(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, args);
    va_end(args);
}

static void ui_draw_button(int x0, int w, const char *label, uint16_t bg, uint16_t fg)
{
    display_draw_filled_rect(x0, BTN_Y, w, BTN_H, bg);
    display_draw_rect(x0, BTN_Y, w, BTN_H, 0xFFFF);
    int lw = (int)strlen(label) * 6;
    int lx = x0 + (w - lw) / 2;
    if (lx < x0 + 2) lx = x0 + 2;
    display_draw_text(lx, BTN_Y + 12, label, fg, bg);
}

void ui_draw_overlay(void)
{
    char line[40];

    /* OSD oben: Version links, fps rechts, Status darunter */
    snprintf(line, sizeof(line), "v0.1.%d", BUILD_NUMBER);
    display_draw_text(2, 2, line, 0xFFFF, 0x0000);
    snprintf(line, sizeof(line), "%lu fps", (unsigned long)stream_get_fps());
    display_draw_text(180, 2, line, 0xFFFF, 0x0000);
    display_draw_text(2, 12, s_status, 0xFFFF, 0x0000);

    /* Buttons unten */
    ui_draw_button(BTN1_X0, 80, "BRI+", 0x001F, 0xFFFF);
    ui_draw_button(BTN2_X0, 80, "BRI-", 0x001F, 0xFFFF);
    ui_draw_button(BTN3_X0, 72, "ROT", 0x07E0, 0x0000);
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

/* ------------------------------------------------------------------ */
/* UI-Task                                                             */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    int px = -1, py = -1;
    TickType_t last_tap = 0;

    while (1) {
        int x, y;
        if (touch_get_point(&x, &y)) {
            /* Tipp erkennen: groesser Koordinatensprung + Entprellzeit */
            if ((abs(x - px) > 40 || abs(y - py) > 40) &&
                (xTaskGetTickCount() - last_tap) > pdMS_TO_TICKS(300)) {
                last_tap = xTaskGetTickCount();
                if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
                    if (x < BTN2_X0)       ui_send_brightness(1);
                    else if (x < BTN3_X0)  ui_send_brightness(-1);
                    else                   ui_toggle_rotation();
                }
            }
            px = x; py = y;
        } else {
            px = -1; py = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", TASK_STACK_UI, NULL, TASK_PRIORITY_UI, NULL);
}
