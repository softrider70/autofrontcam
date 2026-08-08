/*
 * camera.h - OV2640 Kamera-Treiber fuer ESP32-CAM
 *
 * Kapselt die esp32-camera Komponente (espressif/esp32-camera).
 */

#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialisieren der OV2640 Kamera */
esp_err_t camera_init(void);

/* Einzelnes JPEG-Bild aufnehmen (muss mit camera_fb_return freigegeben werden) */
esp_err_t camera_capture_jpeg(uint8_t **buf, size_t *len);

/* Bild freigeben */
void camera_fb_return(void);

/* Kamerastatus abfragen */
bool camera_is_ready(void);

/* Aktuelle Aufloesung und Qualitaet */
void camera_get_info(char *out, size_t len);

/* Bildparameter setzen (jeweils -2..+2, 0 = neutral) */
esp_err_t camera_set_picture(int brightness, int contrast, int saturation);

/* Nachtsicht-Modus: hoeherer Gain + laengere Belichtung fuer wenig Licht */
esp_err_t camera_set_night_mode(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
