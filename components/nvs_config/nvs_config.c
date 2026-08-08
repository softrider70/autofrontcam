/*
 * nvs_config.c - NVS Konfigurationsspeicher fuer Heltec WiFi LoRa 32 V3
 *
 * Ermoeglicht persistentes Speichern/Lesen von Konfigurationswerten
 * (Node-ID, WiFi-Credentials, etc.) im NVS-Flashspeicher.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_config.h"

static const char *TAG = "NVS_CONFIG";
static const char *NVS_NAMESPACE = "lora_config";
static nvs_handle_t nvs_handle_store;
static bool nvs_initialized = false;

/* NVS initialisieren */
esp_err_t nvs_config_init(void)
{
    esp_err_t ret;

    if (nvs_initialized) {
        return ESP_OK;
    }

    /* NVS-Flash initialisieren (falls noch nicht geschehen) */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS-Partition loeschen und neu initialisieren */
        ESP_LOGW(TAG, "NVS-Partition wird zurueckgesetzt");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS-Flash-Init fehlgeschlagen: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* NVS-Handle oeffnen */
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle_store);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS-Open fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_initialized = true;
    ESP_LOGI(TAG, "NVS initialisiert (Namespace: %s)", NVS_NAMESPACE);
    return ESP_OK;
}

/* uint8 Wert lesen (mit Default) */
uint8_t nvs_config_get_u8(const char *key, uint8_t default_val)
{
    if (!nvs_initialized) {
        return default_val;
    }

    uint8_t value = 0;
    esp_err_t ret = nvs_get_u8(nvs_handle_store, key, &value);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_get_u8(%s) fehlgeschlagen: %s",
                     key, esp_err_to_name(ret));
        }
        return default_val;
    }
    return value;
}

/* uint8 Wert schreiben */
esp_err_t nvs_config_set_u8(const char *key, uint8_t val)
{
    if (!nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_set_u8(nvs_handle_store, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) fehlgeschlagen: %s",
                 key, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(nvs_handle_store);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fehlgeschlagen: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

/* int32 Wert lesen (mit Default) */
int32_t nvs_config_get_i32(const char *key, int32_t default_val)
{
    if (!nvs_initialized) {
        return default_val;
    }

    int32_t value = 0;
    esp_err_t ret = nvs_get_i32(nvs_handle_store, key, &value);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_get_i32(%s) fehlgeschlagen: %s",
                     key, esp_err_to_name(ret));
        }
        return default_val;
    }
    return value;
}

/* int32 Wert schreiben */
esp_err_t nvs_config_set_i32(const char *key, int32_t val)
{
    if (!nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_set_i32(nvs_handle_store, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) fehlgeschlagen: %s",
                 key, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(nvs_handle_store);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fehlgeschlagen: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

/* String Wert lesen (mit Default) - returned malloced string */
char* nvs_config_get_str(const char *key, const char *default_val)
{
    if (!nvs_initialized) {
        if (default_val) {
            return strdup(default_val);
        }
        return NULL;
    }

    size_t len = 0;
    esp_err_t ret = nvs_get_str(nvs_handle_store, key, NULL, &len);
    if (ret != ESP_OK || len == 0) {
        if (default_val) {
            return strdup(default_val);
        }
        return NULL;
    }

    char *value = malloc(len);
    if (!value) {
        ESP_LOGE(TAG, "malloc(%u) fehlgeschlagen", len);
        if (default_val) {
            return strdup(default_val);
        }
        return NULL;
    }

    ret = nvs_get_str(nvs_handle_store, key, value, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str(%s) fehlgeschlagen: %s",
                 key, esp_err_to_name(ret));
        free(value);
        if (default_val) {
            return strdup(default_val);
        }
        return NULL;
    }

    return value;
}

/* String Wert schreiben */
esp_err_t nvs_config_set_str(const char *key, const char *val)
{
    if (!nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_set_str(nvs_handle_store, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) fehlgeschlagen: %s",
                 key, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(nvs_handle_store);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fehlgeschlagen: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}