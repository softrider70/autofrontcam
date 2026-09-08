/*
 * stream.h - Display-Client (Kamerabild von der ESP32-CAM)
 *
 * Minimal-Version (v1): WiFi STA am Cam-AP, holt JPEGs per /capture,
 * dekodiert mit esp_jpeg und zeigt das Bild zentriert (1:1) an.
 */

#pragma once

/* Startet WiFi (Station) + Stream-Loop; blockiert (kehrt nicht zurueck). */
void stream_start(void);
