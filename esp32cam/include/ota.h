/*
 * ota.h - OTA-Update ueber Webserver fuer Heltec WiFi LoRa 32 V3
 */

#ifndef OTA_H
#define OTA_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_init(void);
esp_err_t ota_deinit(void);

/* OTA-Handler (/update, /status) auf dem Hauptserver registrieren */
esp_err_t ota_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */