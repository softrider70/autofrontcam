#pragma once

/* Startet den UI-Task (Touch-Auswertung, Buttons, CAM-Steuerung) */
void ui_start(void);

/* Zeichnet OSD (Version/fps/Status) + Buttons - wird vom Stream-Task nach
 * jedem Bild aufgerufen, damit die UI ueber dem Bild liegt. */
void ui_draw_overlay(void);

/* Statuszeile setzen (wird im OSD angezeigt) */
void ui_set_status(const char *fmt, ...);
