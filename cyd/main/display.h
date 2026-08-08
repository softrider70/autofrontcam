#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Initialisiert SPI-Bus + ILI9341-Display + Backlight */
esp_err_t display_init(void);

/* Backlight an/aus */
void display_backlight(bool on);

/* Bildschirm mit Farbe (RGB565) fuellen */
void display_fill(uint16_t color);

/* Dekodiertes RGB565-Bild (u16, Big-Endian wie vom Display erwartet) anzeigen.
 * src_w/src_h = Breite/Hoehe des dekodierten Bildes.
 * Verwendet die interne Rotation (display_set_rotation). */
void display_blit_decoded(const uint16_t *src, int src_w, int src_h);

/* Rotation setzen: 0=ohne, 1=CW, 2=CCW */
void display_set_rotation(int rotation);
int  display_get_rotation(void);

/* Text (5x7) zeichnen, color/bg als RGB565 */
void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg);

/* Rechtecke zeichnen (RGB565) */
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color);

/* Diagnose-Selbsttest: Rot -> Gruen -> Blau -> Schwarz nacheinander (je ~0,8s) */
void display_test_pattern(void);
