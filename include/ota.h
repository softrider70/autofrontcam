/*
 * ota.h - OTA-Update ueber Webserver fuer Heltec WiFi LoRa 32 V3
 */

#ifndef OTA_H
#define OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_init(void);
esp_err_t ota_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */