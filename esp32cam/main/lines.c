/*
 * lines.c - Kalibrierungslinien (Fahrzeugkante) fuer Autofrontcam
 *
 * Getrennte Linien-Konfiguration fuer Hoch- und Querformat in NVS (u8):
 *   Portrait:  pr_red_x/a/w/on, pr_yel_x/a/w/on
 *   Landscape: lr_red_x/a/w/on, lr_yel_x/a/w/on
 */

#include <stdio.h>
#include "config.h"
#include "lines.h"
#include "nvs_config.h"

/* Defaults: Portrait (Hochformat) */
static line_cfg_t p_red = { 50, 0, 3, true };
static line_cfg_t p_yellow = { 60, 0, 3, false };

/* Defaults: Landscape (Querformat) */
static line_cfg_t l_red = { 50, 0, 3, true };
static line_cfg_t l_yellow = { 60, 0, 3, false };

/* Winkel -90..90 <-> u8 0..180 */
static uint8_t angle_to_u8(int deg) { return (uint8_t)(deg + 90); }
static int u8_to_angle(uint8_t v) { return (int)v - 90; }

void lines_init(void)
{
    /* --- Portrait --- */
    p_red.x_percent = nvs_config_get_u8("pr_red_x", p_red.x_percent);
    p_red.angle_deg = u8_to_angle(nvs_config_get_u8("pr_red_a", angle_to_u8(p_red.angle_deg)));
    p_red.width_px = nvs_config_get_u8("pr_red_w", p_red.width_px);
    p_red.enabled = nvs_config_get_u8("pr_red_on", p_red.enabled ? 1 : 0) != 0;

    p_yellow.x_percent = nvs_config_get_u8("pr_yel_x", p_yellow.x_percent);
    p_yellow.angle_deg = u8_to_angle(nvs_config_get_u8("pr_yel_a", angle_to_u8(p_yellow.angle_deg)));
    p_yellow.width_px = nvs_config_get_u8("pr_yel_w", p_yellow.width_px);
    p_yellow.enabled = nvs_config_get_u8("pr_yel_on", p_yellow.enabled ? 1 : 0) != 0;

    /* --- Landscape --- */
    l_red.x_percent = nvs_config_get_u8("lr_red_x", l_red.x_percent);
    l_red.angle_deg = u8_to_angle(nvs_config_get_u8("lr_red_a", angle_to_u8(l_red.angle_deg)));
    l_red.width_px = nvs_config_get_u8("lr_red_w", l_red.width_px);
    l_red.enabled = nvs_config_get_u8("lr_red_on", l_red.enabled ? 1 : 0) != 0;

    l_yellow.x_percent = nvs_config_get_u8("lr_yel_x", l_yellow.x_percent);
    l_yellow.angle_deg = u8_to_angle(nvs_config_get_u8("lr_yel_a", angle_to_u8(l_yellow.angle_deg)));
    l_yellow.width_px = nvs_config_get_u8("lr_yel_w", l_yellow.width_px);
    l_yellow.enabled = nvs_config_get_u8("lr_yel_on", l_yellow.enabled ? 1 : 0) != 0;

    /* Gelbe Linie ist immer aktiv (keine Ausschalt-Option) */
    p_yellow.enabled = true;
    l_yellow.enabled = true;
}

void lines_get_dual(line_cfg_t *p_red_o, line_cfg_t *p_yellow_o,
                    line_cfg_t *l_red_o, line_cfg_t *l_yellow_o)
{
    if (p_red_o) *p_red_o = p_red;
    if (p_yellow_o) *p_yellow_o = p_yellow;
    if (l_red_o) *l_red_o = l_red;
    if (l_yellow_o) *l_yellow_o = l_yellow;
}

void lines_set_dual(const line_cfg_t *p_red_o, const line_cfg_t *p_yellow_o,
                    const line_cfg_t *l_red_o, const line_cfg_t *l_yellow_o)
{
    if (p_red_o) p_red = *p_red_o;
    if (p_yellow_o) p_yellow = *p_yellow_o;
    if (l_red_o) l_red = *l_red_o;
    if (l_yellow_o) l_yellow = *l_yellow_o;

    /* Gelbe Linie ist immer aktiv (keine Ausschalt-Option) */
    p_yellow.enabled = true;
    l_yellow.enabled = true;

    /* --- Portrait --- */
    nvs_config_set_u8("pr_red_x", (uint8_t)p_red.x_percent);
    nvs_config_set_u8("pr_red_a", angle_to_u8(p_red.angle_deg));
    nvs_config_set_u8("pr_red_w", (uint8_t)p_red.width_px);
    nvs_config_set_u8("pr_red_on", p_red.enabled ? 1 : 0);

    nvs_config_set_u8("pr_yel_x", (uint8_t)p_yellow.x_percent);
    nvs_config_set_u8("pr_yel_a", angle_to_u8(p_yellow.angle_deg));
    nvs_config_set_u8("pr_yel_w", (uint8_t)p_yellow.width_px);
    nvs_config_set_u8("pr_yel_on", p_yellow.enabled ? 1 : 0);

    /* --- Landscape --- */
    nvs_config_set_u8("lr_red_x", (uint8_t)l_red.x_percent);
    nvs_config_set_u8("lr_red_a", angle_to_u8(l_red.angle_deg));
    nvs_config_set_u8("lr_red_w", (uint8_t)l_red.width_px);
    nvs_config_set_u8("lr_red_on", l_red.enabled ? 1 : 0);

    nvs_config_set_u8("lr_yel_x", (uint8_t)l_yellow.x_percent);
    nvs_config_set_u8("lr_yel_a", angle_to_u8(l_yellow.angle_deg));
    nvs_config_set_u8("lr_yel_w", (uint8_t)l_yellow.width_px);
    nvs_config_set_u8("lr_yel_on", l_yellow.enabled ? 1 : 0);
}
