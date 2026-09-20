/*
 * ui.h - Touch-UI fuer den s3lcd Display-Client (Kalibrier-Linien + Menue)
 *
 * 3 vertikale Kalibrier-Linien (rot/gelb/gruen) als Overlay ueber dem Video.
 * Tap aufs Display oeffnet ein Menue; daraus der Linien-Edit-Modus mit
 * Buttons im linken Bereich (OK / Farb-Linienwahl / < > / Dicke).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* UI initialisieren (NVS-Linien laden, Touch-Task starten). */
void ui_start(void);

/* Overlay ueber das laufende Video zeichnen (Kalibrier-Linien + ggf. Buttons).
 * Wird vom Stream-Task nach jedem Video-Blitz aufgerufen. */
void ui_draw_overlay(void);

/* true, wenn das Video pausiert werden soll (Menue/Edit-Modus offen). */
bool ui_video_paused(void);

/* true, wenn der Linien-Edit-Modus aktiv ist. */
bool ui_line_edit_active(void);

/* Statuszeile (oben, ueber dem Video) setzen. */
void ui_set_status(const char *fmt, ...);

/* Anzeige-Rotation setzen (0=ohne, 1=CW, 2=CCW) - Menue "Drehen". */
void ui_set_rotation(int rotation);

/* Vom Linien-Edit in NVS speichern (wird bei OK ausgeloest). */
void ui_lines_save(void);

/* Aktueller Bild-Rotationswinkel in Grad (0..359) fuer die Kamera-Ausrichtung. */
int ui_get_img_deg(void);
