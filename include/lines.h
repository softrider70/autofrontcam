/*
 * lines.h - Kalibrierungslinien (Fahrzeugkante) fuer Autofrontcam
 *
 * Zwei Linien im Canvas-Overlay ueber dem Livestream:
 *   - Rot: Fahrzeugkante (Standard)
 *   - Gelb: optionale zweite Linie
 *
 * Die Einstellungen werden GETRENNT fuer Hoch- und Querformat in NVS
 * gespeichert (andere Kamera-Ansicht je nach Bildausrichtung).
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

/* Beide Orientierungs-Saetze aus NVS laden (Defaults, falls nicht vorhanden) */
void lines_init(void);

/* Beide Saetze holen (Portrait + Landscape). Pointer duerfen NULL sein. */
void lines_get_dual(line_cfg_t *p_red, line_cfg_t *p_yellow,
                    line_cfg_t *l_red, line_cfg_t *l_yellow);

/* Beide Saetze setzen und in NVS speichern. Pointer duerfen NULL sein. */
void lines_set_dual(const line_cfg_t *p_red, const line_cfg_t *p_yellow,
                    const line_cfg_t *l_red, const line_cfg_t *l_yellow);

#ifdef __cplusplus
}
#endif

#endif /* LINES_H */
