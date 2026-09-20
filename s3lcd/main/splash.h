/*
 * splash.h - Boot-Splash (Bild statt Farb-Testmuster)
 */

#pragma once

/* Zeigt das eingebettete Splash-Bild (JPEG -> RGB565) auf dem ganzen Display.
 * Wird beim Boot direkt nach display_init()/Backlight aufgerufen und bleibt
 * sichtbar, bis das erste Videobild eintrifft. */
void splash_show(void);
