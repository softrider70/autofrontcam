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

/* Bildschirm komplett mit RGB565 fuellen (in den Framebuffer) */
void display_fill(uint16_t color);

/* Kompletten Framebuffer ans Panel senden (1x pro Frame nach dem Zeichnen) */
void display_commit(void);

/* Framebuffer-Zeichnen serialisieren (display_lock ... display_unlock) */
void display_lock(void);
void display_unlock(void);

/* Rechteck mit RGB565 fuellen */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* RGB565-Bildbuffer (w x h) mit oberer linker Ecke bei (x,y) anzeigen.
 * Muss voll im Bild liegen. Jeder Pixel ist uint16 (RGB565), wird big-endian
 * (high byte zuerst) auf den SPI gebracht. */
void display_blit(const uint16_t *pixels, int x, int y, int w, int h);

/* Bild um seinen Mittelpunkt um 'deg' Zehntelgrad (0..3599) drehen, in X/Y um
 * sx/sy Prozent strecken (100 = unveraendert, 200 = doppelt so breit/hoch,
 * fuer Keystone-Ausgleich), die Bildmitte um (ox,oy) Pixel verschieben und so
 * einpassen, dass es in den Rahmen (dw x dh) passt. Beim Strecken/Verschieben
 * darf das Bild ueber den Rand laufen; Ecken ausserhalb der Quelle werden
 * schwarz. */
void display_blit_rot_fit(const uint16_t *src, int sw, int sh,
                          int dx, int dy, int dw, int dh, int deg,
                          int sx_pct, int sy_pct, int ox, int oy);

/* Zeichenfunktionen (fuer OSD/Kalibrierlinien/UI) */
void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg);
void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_line(int x0, int y0, int x1, int y1, int width, uint16_t color);

/* Bildausrichtung (Anzeige): 0=ohne, 1=CW, 2=CCW */
void display_set_rotation(int rotation);
int  display_get_rotation(void);

/* Diagnose-Selbsttest: 4 Quadranten R/G/B/Schwarz + Rahmen + Orientierungs-Marker */
void display_test_pattern(void);

/* Externer Zugriff fuer spaetere Blit-Funktionen (Reset Window setzen) */
void display_set_window(int x0, int y0, int x1, int y1);

/* Farbpfad-Test (Bring-up): zur Laufzeit umschalten */
void display_set_byte_swap(bool swap);  /* Pixel-Bytes tauschen (lo/hi) */
void display_set_color_mode(bool bgr, bool invert);  /* MADCTL-BGR + INVON/INVOFF */
void display_set_madctl(uint8_t madctl);  /* voller MADCTL-Wert setzen (Orientierung) */
