/*
 * ota.c - OTA-Webserver fuer Autofrontcam (ESP32-CAM)
 *
 * Bietet eine minimale Web-UI zum Hochladen und Flashen von
 * Firmware-Updates (.bin-Dateien) ueber das WiFi.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "config.h"
#include "ota.h"
#include "wifi.h"

/* mdns ist optional - in ESP-IDF 6.1 nicht mehr als separate Komponente */
/* Wenn verfuegbar, wird es zur Verfuegung gestellt */
#if __has_include("mdns.h")
#include "mdns.h"
#define HAS_MDNS 1
#else
#define HAS_MDNS 0
#pragma message("mdns.h nicht gefunden - mDNS deaktiviert")
#endif

static const char *TAG = "OTA";

static httpd_handle_t ota_server = NULL;
static bool ota_running = false;

/* OTA-Update-Status */
static volatile bool ota_update_in_progress = false;

/* ====================================================================
 * HTML-Seiten
 * ==================================================================== */

static const char *OTA_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Autofrontcam OTA Update</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
    "h1{color:#333;border-bottom:2px solid #28a745;padding-bottom:10px}"
    ".container{max-width:500px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}"
    "label{display:block;margin:10px 0 5px;font-weight:bold;color:#555}"
    "input[type=file]{width:100%;padding:10px;margin:5px 0 15px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
    "input[type=submit]{background:#28a745;color:#fff;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;width:100%;font-size:16px}"
    "input[type=submit]:hover{background:#218838}"
    ".info{background:#e7f3ff;padding:10px;border-radius:4px;margin:15px 0;font-size:14px}"
    ".status{margin-top:15px;padding:10px;border-radius:4px;display:none}"
    "</style>"
    "<script>"
    "function uploadFile(){"
    "var file=document.getElementById('file').files[0];if(!file){alert('Bitte Datei auswaehlen');return}"
    "var form=new FormData();form.append('file',file);"
    "var xhr=new XMLHttpRequest();"
    "xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);document.getElementById('progress').style.display='block';document.getElementById('progress').innerHTML='Fortschritt: '+p+'%'}}"
    "xhr.onload=function(){document.getElementById('progress').innerHTML=xhr.responseText}"
    "xhr.onerror=function(){document.getElementById('progress').innerHTML='Fehler beim Upload'}"
    "xhr.open('POST','/update',true);xhr.send(form)}"
    "</script>"
    "</head><body>"
    "<div class='container'>"
    "<h1>Autofrontcam OTA Update</h1>"
    "<div class='info'>Waehle eine Firmware-Datei (.bin) aus und klicke auf 'Upload & Flashen'.<br>"
    "Das Board startet nach erfolgreichem Update automatisch neu.</div>"
    "<input type='file' id='file' accept='.bin'>"
    "<input type='submit' value='Upload & Flashen' onclick='uploadFile()'>"
    "<div id='progress' class='status'></div>"
    "</div></body></html>";

/* ====================================================================
 * HTTP-Handler
 * ==================================================================== */

static esp_err_t ota_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, OTA_HTML, strlen(OTA_HTML));
    return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    esp_err_t ret;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    if (ota_update_in_progress) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Update bereits aktiv", -1);
        return ESP_OK;
    }

    ota_update_in_progress = true;

    char buf[1024];
    int received;
    size_t total_received = 0;

    /* OTA-Partition ermitteln */
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "Keine OTA-Partition gefunden", -1);
        ota_update_in_progress = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA-Update gestartet (Partition: %s, Groesse: %u KB)",
             update_partition->label, update_partition->size / 1024);

    ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fehlgeschlagen: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "OTA-Init fehlgeschlagen", -1);
        ota_update_in_progress = false;
        return ESP_OK;
    }

    /* Daten in Chunks empfangen und schreiben */
    while (1) {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }

        ret = esp_ota_write(update_handle, buf, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fehlgeschlagen: %s", esp_err_to_name(ret));
            esp_ota_abort(update_handle);
            httpd_resp_set_status(req, "500 Internal Error");
            httpd_resp_send(req, "Schreibfehler", -1);
            ota_update_in_progress = false;
            return ESP_OK;
        }

        total_received += received;
    }

    ESP_LOGI(TAG, "OTA: %u Bytes empfangen", total_received);

    /* Update abschliessen */
    ret = esp_ota_end(update_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end fehlgeschlagen: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "OTA-Abschluss fehlgeschlagen", -1);
        ota_update_in_progress = false;
        return ESP_OK;
    }

    /* Partition als bootfaehig markieren */
    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition fehlgeschlagen: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "Boot-Partition-Fehler", -1);
        ota_update_in_progress = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA-Update erfolgreich! Starte neu...");

    char response[128];
    snprintf(response, sizeof(response),
             "Update erfolgreich! %u Bytes geschrieben. Neustart...", total_received);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, strlen(response));

    ota_update_in_progress = false;

    /* Neustart nach 1 Sekunde */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static const httpd_uri_t ota_index = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = ota_index_handler,
};

static const httpd_uri_t ota_update = {
    .uri = "/update",
    .method = HTTP_POST,
    .handler = ota_update_handler,
};

static const httpd_uri_t ota_update_get = {
    .uri = "/update",
    .method = HTTP_GET,
    .handler = ota_index_handler,
};

/* ====================================================================
 * Status-API (JSON)
 * ==================================================================== */

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char json[256];
    snprintf(json, sizeof(json),
             "{"
             "\"running\":\"%s\","
             "\"boot\":\"%s\","
             "\"next\":\"%s\","
             "\"updating\":%s"
             "}",
             running ? running->label : "?",
             boot ? boot->label : "?",
             next ? next->label : "?",
             ota_update_in_progress ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static const httpd_uri_t ota_status = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = ota_status_handler,
};

/* ====================================================================
 * Oeffentliche API
 * ==================================================================== */

esp_err_t ota_init(void)
{
    if (ota_running) return ESP_OK;

#if HAS_MDNS
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    } else {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("Autofrontcam");
        ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
    }
#endif

    /* HTTP-Server starten */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_WEBSERVER_PORT;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = 4096;

    esp_err_t ret = httpd_start(&ota_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    /* URI-Handler registrieren */
    httpd_register_uri_handler(ota_server, &ota_index);
    httpd_register_uri_handler(ota_server, &ota_update);
    httpd_register_uri_handler(ota_server, &ota_update_get);
    httpd_register_uri_handler(ota_server, &ota_status);

    ota_running = true;
    ESP_LOGI(TAG, "OTA-Webserver gestartet (Port %d)", OTA_WEBSERVER_PORT);

    /* Aktuelle Partition anzeigen */
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Aktive Partition: %s", running ? running->label : "?");

    return ESP_OK;
}

esp_err_t ota_deinit(void)
{
    if (!ota_running) return ESP_OK;

    if (ota_server) {
        httpd_stop(ota_server);
        ota_server = NULL;
    }

#if HAS_MDNS
    mdns_free();
#endif

    ota_running = false;
    ESP_LOGI(TAG, "OTA-Webserver deinitialisiert");
    return ESP_OK;
}