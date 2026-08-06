/*
 * nvs_config.h - NVS Konfigurationsspeicher fuer Heltec WiFi LoRa 32 V3
 */

#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t nvs_config_init(void);
uint8_t nvs_config_get_u8(const char *key, uint8_t default_val);
esp_err_t nvs_config_set_u8(const char *key, uint8_t val);
char* nvs_config_get_str(const char *key, const char *default_val);
esp_err_t nvs_config_set_str(const char *key, const char *val);

#endif /* NVS_CONFIG_H */