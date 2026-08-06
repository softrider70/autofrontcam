/*
 * wifi.h - WiFi-Management (Captive Portal) fuer Heltec WiFi LoRa 32 V3
 */

#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(void);
esp_err_t wifi_deinit(void);
bool wifi_is_connected(void);
const char *wifi_get_ip(void);
void wifi_reset_credentials(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */