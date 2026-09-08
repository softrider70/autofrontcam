/*
 * display.h - ST7796S Display-Treiber (ES3C40P, 320x480 SPI, hier quer 480x320)
 *
 * Roher SPI-Treiber (synchron, DC manuell) - Muster wie tft/cyd.
 * Bring-up: Init, Backlight, Fuellen, Rechteck, Farbtest.
 * (Blit/Text folgen mit dem Stream-Client-Port in einem spaeteren Schritt.)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t display_init(void);
void display_backlight(bool on);

/* Bildschirm komplett mit RGB565 fuellen */
void display_fill(uint16_t color);

/* Rechteck mit RGB565 fuellen */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* RGB565-Bildbuffer (w x h) mit oberer linker Ecke bei (x,y) anzeigen.
 * Muss voll im Bild liegen. Jeder Pixel ist uint16 (RGB565), wird big-endian
 * (high byte zuerst) auf den SPI gebracht. */
void display_blit(const uint16_t *pixels, int x, int y, int w, int h);

/* Diagnose-Selbsttest: 4 Quadranten R/G/B/Schwarz + Rahmen + Orientierungs-Marker */
void display_test_pattern(void);

/* Externer Zugriff fuer spaetere Blit-Funktionen (Reset Window setzen) */
void display_set_window(int x0, int y0, int x1, int y1);

/* Farbpfad-Test (Bring-up): zur Laufzeit umschalten */
void display_set_byte_swap(bool swap);  /* Pixel-Bytes tauschen (lo/hi) */
void display_set_color_mode(bool bgr, bool invert);  /* MADCTL-BGR + INVON/INVOFF */
void display_set_madctl(uint8_t madctl);  /* voller MADCTL-Wert setzen (Orientierung) */
