/*
 * lines.h - Kalibrierungslinien (Fahrzeugkante) fuer Autofrontcam
 *
 * Zwei Linien im Canvas-Overlay ueber dem Livestream:
 *   - Rot: Fahrzeugkante (Standard)
 *   - Gelb: optionale zweite Linie
 * Eigenschaften je Linie: X-Position (%), Winkel (Grad), Dicke (px), aktiv.
 * Alle Werte werden in NVS gespeichert und ueberleben Sleep/Neustart.
 */

#ifndef LINES_H
#define LINES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x_percent;   /* 0..100 (0 = links, 100 = rechts) */
    int angle_deg;   /* -45..+45 (Rotation um die Bildmitte) */
    int width_px;    /* 1..15 */
    bool enabled;
} line_cfg_t;

/* Einstellungen aus NVS laden (Defaults, falls nicht vorhanden) */
void lines_init(void);

/* Aktuelle Konfiguration holen */
void lines_get(line_cfg_t *red, line_cfg_t *yellow);

/* Konfiguration setzen und in NVS speichern */
void lines_set(const line_cfg_t *red, const line_cfg_t *yellow);

#ifdef __cplusplus
}
#endif

#endif /* LINES_H */
