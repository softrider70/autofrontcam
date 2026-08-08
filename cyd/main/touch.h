#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialisiert XPT2046-Touch auf dem geteilten SPI-Bus */
esp_err_t touch_init(void);

/* Liefert aktuelle Touch-Koordinate (Display-Koordinaten) und true, wenn
 * wirklich gedrueckt wird (Druckerkennung ueber Z1/Z2). */
bool touch_get_point(int *x, int *y);

/* Kalibrier-Modus ein/aus (aus dem Touch-Menue): Im Kalibrier-Modus werden die
 * ROHWERTE (x_raw/y_raw) geloggt und touch_get_point liefert false, damit die
 * Umrechnung aus den gemessenen Eckenpunkten bestimmt werden kann. */
void touch_set_calib_mode(bool on);
bool touch_calib_mode(void);
