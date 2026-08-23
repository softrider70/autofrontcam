#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Startet WiFi (Station) + Stream-Task (JPEG abholen, dekodieren, anzeigen) */
void stream_start(void);

/* 1 wenn WiFi verbunden und Stream aktiv */
bool stream_is_connected(void);

/* Aktuelle FPS (Frames pro Sekunde) */
uint32_t stream_get_fps(void);
