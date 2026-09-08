/*
 * touch.h - FT6336U kapazitiver Touch (ES3C40P, I2C Adr 0x38)
 *
 * Bring-up: Initialisierung + Roh-Koordinaten lesen (fuer Test/Kalibrierung).
 * Skalierung/Orientierung auf Hardware verifizieren (TOUCH_* in config.h).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool touched;
    int  raw_x;     /* Rohwert des FT6336U (0..~1023) */
    int  raw_y;
} touch_point_t;

typedef struct {
    bool touched;
    int  x;         /* Display-Koordinate (0..TFT_WIDTH-1) */
    int  y;         /* Display-Koordinate (0..TFT_HEIGHT-1) */
} touch_screen_t;

esp_err_t touch_init(void);

/* Aktuellen (ersten) Touchpunkt lesen; touched=false wenn keiner. */
esp_err_t touch_read(touch_point_t *pt);

/* Touch lesen und per Kalibrierung (config.h) auf Display-Koordinaten mappen */
esp_err_t touch_read_screen(touch_screen_t *p);

