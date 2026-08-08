#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialisiert XPT2046-Touch auf dem geteilten SPI-Bus */
esp_err_t touch_init(void);

/* Liefert aktuelle Touch-Koordinate (Display-Koordinaten) und true, wenn
 * wirklich gedrueckt wird (Druckerkennung ueber Z1/Z2). */
bool touch_get_point(int *x, int *y);
