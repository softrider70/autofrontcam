#pragma once

#include <stdbool.h>

/* Startet den UI-Task (Touch-Auswertung, Buttons, CAM-Steuerung) */
void ui_start(void);

/* Zeichnet OSD (Version/fps/Status) - wird vom Stream-Task nach jedem Bild
 * aufgerufen, damit die Statuszeile ueber dem Bild liegt. */
void ui_draw_overlay(void);

/* Statuszeile setzen (wird im OSD angezeigt) */
void ui_set_status(const char *fmt, ...);

/* Menue-Zustand: true, wenn das Touch-Menue gerade offen ist. Der Stream-Task
 * pausiert dann das Bildzeichnen, damit das Menue nicht uebermalt wird. */
bool ui_menu_is_open(void);

/* Diagnose-Zustand: true, solange der Panel-Geometrie-Test angezeigt wird
 * (Stream pausiert, Test bleibt stehen). Beendet durch einen Touch. */
bool ui_diag_is_active(void);
