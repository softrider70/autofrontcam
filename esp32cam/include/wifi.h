/*
 * wifi.h - WiFi-Management (Captive Portal) fuer Heltec WiFi LoRa 32 V3
 */

#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(void);
esp_err_t wifi_deinit(void);
bool wifi_is_connected(void);
const char *wifi_get_ip(void);
void wifi_reset_credentials(void);

/* Wird ein WiFi-Captive-Portal benoetigt (keine Credentials / nicht verbunden)? */
bool wifi_portal_needed(void);

/* Portal-Handler (/wifi, /save) auf dem Hauptserver registrieren */
esp_err_t wifi_register_portal(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */