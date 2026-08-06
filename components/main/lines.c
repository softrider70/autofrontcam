/*
 * lines.c - Kalibrierungslinien (Fahrzeugkante) fuer Autofrontcam
 *
 * Persistente Linien-Konfiguration in NVS (u8-Werte):
 *   Rot:    l_red_x  (0..100), l_red_a  (Winkel+90), l_red_w (Dicke), l_red_on
 *   Gelb:   l_yel_x  (0..100), l_yel_a  (Winkel+90), l_yel_w (Dicke), l_yel_on
 */

#include <stdio.h>
#include "config.h"
#include "lines.h"
#include "nvs_config.h"

static line_cfg_t red = { 50, 0, 3, true };     /* Fahrzeugkante */
static line_cfg_t yellow = { 60, 0, 3, false }; /* zweite Linie (optional) */

/* Winkel -90..90 <-> u8 0..180 */
static uint8_t angle_to_u8(int deg) { return (uint8_t)(deg + 90); }
static int u8_to_angle(uint8_t v) { return (int)v - 90; }

void lines_init(void)
{
    red.x_percent = nvs_config_get_u8("l_red_x", red.x_percent);
    red.angle_deg = u8_to_angle(nvs_config_get_u8("l_red_a", angle_to_u8(red.angle_deg)));
    red.width_px = nvs_config_get_u8("l_red_w", red.width_px);
    red.enabled = nvs_config_get_u8("l_red_on", red.enabled ? 1 : 0) != 0;

    yellow.x_percent = nvs_config_get_u8("l_yel_x", yellow.x_percent);
    yellow.angle_deg = u8_to_angle(nvs_config_get_u8("l_yel_a", angle_to_u8(yellow.angle_deg)));
    yellow.width_px = nvs_config_get_u8("l_yel_w", yellow.width_px);
    yellow.enabled = nvs_config_get_u8("l_yel_on", yellow.enabled ? 1 : 0) != 0;
}

void lines_get(line_cfg_t *r, line_cfg_t *y)
{
    *r = red;
    *y = yellow;
}

void lines_set(const line_cfg_t *r, const line_cfg_t *y)
{
    if (r) red = *r;
    if (y) yellow = *y;

    nvs_config_set_u8("l_red_x", (uint8_t)red.x_percent);
    nvs_config_set_u8("l_red_a", angle_to_u8(red.angle_deg));
    nvs_config_set_u8("l_red_w", (uint8_t)red.width_px);
    nvs_config_set_u8("l_red_on", red.enabled ? 1 : 0);

    nvs_config_set_u8("l_yel_x", (uint8_t)yellow.x_percent);
    nvs_config_set_u8("l_yel_a", angle_to_u8(yellow.angle_deg));
    nvs_config_set_u8("l_yel_w", (uint8_t)yellow.width_px);
    nvs_config_set_u8("l_yel_on", yellow.enabled ? 1 : 0);
}
