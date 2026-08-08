/*
 * wifi.c - WiFi-Manager (Station/AP/Captive Portal) fuer Heltec WiFi LoRa 32 V3
 *
 * Startet zunaechst im Station-Modus. Falls keine gespeicherten Credentials
 * vorhanden sind oder der Verbindungsversuch fehlschlaegt, wird ein
 * Captive Portal im AP-Modus gestartet.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "config.h"
#include "nvs_config.h"
#include "wifi.h"

static const char *TAG = "WIFI";

/* Events */
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1

static EventGroupHandle_t wifi_event_group = NULL;
static esp_netif_t *netif_sta = NULL;
static esp_netif_t *netif_ap = NULL;
static bool wifi_connected = false;
static char current_ip[16] = {0};
static bool portal_needed = false;      /* Portal wird auf dem Hauptserver registriert */
static bool portal_registered = false;

/* ====================================================================
 * Captive Portal HTML-Seite (Konfiguration)
 * ==================================================================== */

static const char *PORTAL_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Autofrontcam WiFi Konfiguration</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
    "h1{color:#333;border-bottom:2px solid #007bff;padding-bottom:10px}"
    ".container{max-width:400px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}"
    "label{display:block;margin:10px 0 5px;font-weight:bold;color:#555}"
    "input[type=text],input[type=password]{width:100%;padding:10px;margin:5px 0 15px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
    "input[type=submit]{background:#007bff;color:#fff;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;width:100%;font-size:16px}"
    "input[type=submit]:hover{background:#0056b3}"
    ".info{background:#e7f3ff;padding:10px;border-radius:4px;margin:15px 0;font-size:14px}"
    "</style></head><body>"
    "<div class='container'>"
    "<h1>Autofrontcam Konfiguration</h1>"
    "<div class='info'>Gib deine WiFi-Zugangsdaten ein, um die Kamera mit deinem Netzwerk zu verbinden.</div>"
    "<form method='POST' action='/save'>"
    "<label>SSID (Netzwerkname):</label>"
    "<input type='text' name='ssid' placeholder='z.B. FritzBox123' required>"
    "<label>Passwort:</label>"
    "<input type='password' name='password' placeholder='WiFi-Passwort'>"
    "<input type='submit' value='Verbinden'>"
    "</form></div></body></html>";

static const char *SAVED_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Konfiguration gespeichert</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
    ".container{max-width:400px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);text-align:center}"
    "h1{color:#28a745;border-bottom:2px solid #28a745;padding-bottom:10px}"
    ".msg{font-size:18px;margin:20px 0;color:#333}"
    "</style></head><body>"
    "<div class='container'>"
    "<h1>Gespeichert!</h1>"
    "<div class='msg'>Die Konfiguration wurde gespeichert.<br>"
    "Die Kamera startet neu und verbindet sich mit dem angegebenen Netzwerk.<br><br>"
    "Nach erfolgreicher Verbindung ist die Kamera erreichbar unter:<br>"
    "<b>http://autocam.local</b></div>"
    "</div></body></html>";

/* ====================================================================
 * HTTP-Handler
 * ==================================================================== */

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
    return ESP_OK;
}

/* URL-Decodierung (Formularwerte): '+', = Leerzeichen, '%XX' = Sonderzeichen */
static void url_decode(char *out, size_t out_size, const char *in)
{
    size_t o = 0;
    while (*in && o + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            char hi = in[1], lo = in[2];
            int h = (hi >= '0' && hi <= '9') ? hi - '0' :
                    (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 :
                    (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
            int l = (lo >= '0' && lo <= '9') ? lo - '0' :
                    (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 :
                    (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
            if (h >= 0 && l >= 0) {
                out[o++] = (char)(h * 16 + l);
                in += 3;
                continue;
            }
        }
        out[o++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[o] = '\0';
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char content[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(content)) {
        httpd_resp_set_status(req, "413 Request Entity Too Large");
        httpd_resp_send(req, "Zu viele Daten", -1);
        return ESP_OK;
    }

    ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "Empfangsfehler", -1);
        return ESP_OK;
    }
    content[ret] = '\0';

    /* SSID und Passwort parsen (URL-dekodiert) */
    char ssid[64] = {0};
    char password[64] = {0};

    char *param = content;
    while (*param) {
        char *next = strchr(param, '&');
        if (next) *next = '\0';

        char *eq = strchr(param, '=');
        if (eq) {
            *eq = '\0';
            char *val = eq + 1;

            if (strncmp(param, "ssid", 4) == 0) {
                url_decode(ssid, sizeof(ssid), val);
            } else if (strncmp(param, "password", 8) == 0) {
                url_decode(password, sizeof(password), val);
            }
        }

        if (next)
            param = next + 1;
        else
            break;
    }

    ESP_LOGI(TAG, "Captive Portal: SSID=%s", ssid);

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "SSID darf nicht leer sein", -1);
        return ESP_OK;
    }

    /* In NVS speichern */
    nvs_config_set_str("wifi_ssid", ssid);
    nvs_config_set_str("wifi_password", password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, strlen(SAVED_HTML));

    /* Neustart nach 2 Sekunden */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

static const httpd_uri_t uri_wifi = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = portal_get_handler,
};

static const httpd_uri_t uri_save = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = portal_save_handler,
};

/* Portal-Handler auf dem Hauptserver (Web-UI) registrieren */
esp_err_t wifi_register_portal(httpd_handle_t server)
{
    if (portal_registered) return ESP_OK;
    if (!server) return ESP_ERR_INVALID_ARG;

    httpd_register_uri_handler(server, &uri_wifi);
    httpd_register_uri_handler(server, &uri_save);
    portal_registered = true;
    ESP_LOGI(TAG, "WiFi-Portal registriert (/wifi, /save)");
    return ESP_OK;
}

bool wifi_portal_needed(void)
{
    return portal_needed;
}

/* ====================================================================
 * WiFi-Event-Handler
 * ==================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Station-Modus gestartet, verbinde...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi-Verbindung getrennt");
                wifi_connected = false;
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                /* Verbinde erneut */
                esp_wifi_connect();
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Client mit AP verbunden");
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Client von AP getrennt");
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            esp_ip4addr_ntoa(&event->ip_info.ip, current_ip, sizeof(current_ip));
            ESP_LOGI(TAG, "IP-Adresse erhalten: %s", current_ip);
            wifi_connected = true;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ====================================================================
 * Oeffentliche API
 * ==================================================================== */

esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initialisiere WiFi");

    /* Event-Group */
    wifi_event_group = xEventGroupCreate();

    /* Netzwerk-Interfaces initialisieren */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    netif_sta = esp_netif_create_default_wifi_sta();
    netif_ap = esp_netif_create_default_wifi_ap();

    /* WiFi-Konfiguration */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Events registrieren */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    /* Gespeicherte Credentials aus NVS laden */
    char *ssid = nvs_config_get_str("wifi_ssid", NULL);
    char *password = nvs_config_get_str("wifi_password", NULL);

    if (ssid && strlen(ssid) > 0) {
        /* Station-Modus mit gespeicherten Credentials */
        ESP_LOGI(TAG, "Verwende gespeicherte WiFi-Credentials (SSID: %s)", ssid);

        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        if (password) {
            strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Auf Verbindung warten (max. CONNECT_TIMEOUT Sekunden) */
        EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_S * 1000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Mit '%s' verbunden (IP: %s)", ssid, current_ip);
        } else {
            ESP_LOGW(TAG, "Verbindung zu '%s' fehlgeschlagen", ssid);
            wifi_connected = false;
        }
    } else {
        ESP_LOGI(TAG, "Keine gespeicherten WiFi-Credentials");
    }

    if (ssid) free(ssid);
    if (password) free(password);

    /* Wenn nicht verbunden, AP-Modus + Captive Portal starten */
    if (!wifi_connected) {
        ESP_LOGI(TAG, "Starte AP-Modus (SSID: %s, IP: %s)", WIFI_AP_SSID, WIFI_AP_IP);

        /* AP-Konfiguration - nur EIN Client erlaubt (zwei iPhones gleichzeitig
         * blockierten frueher den SoftAP-Kanal). */
        wifi_config_t ap_config = {
            .ap = {
                .ssid_len = strlen(WIFI_AP_SSID),
                .channel = 1,
                .max_connection = 1,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        strncpy((char *)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* AP + Station */
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

        if (!wifi_connected) {
            /* Station-Konfiguration leer lassen - nur AP aktiv */
            wifi_config_t sta_config = {0};
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        }

        ESP_ERROR_CHECK(esp_wifi_start());

        /* AP-IP zuverlaessig nach dem Start setzen: DHCP stoppen, IP setzen, neu starten.
         * Werte muessen zu WIFI_AP_IP / WIFI_AP_NETMASK in config.h passen. */
        esp_netif_dhcps_stop(netif_ap);
        esp_netif_ip_info_t ip_info = {0};
        IP4_ADDR(&ip_info.ip, 10, 1, 1, 1);
        IP4_ADDR(&ip_info.gw, 10, 1, 1, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(netif_ap, &ip_info);
        esp_netif_dhcps_start(netif_ap);
        ESP_LOGI(TAG, "AP-IP gesetzt: " IPSTR, IP2STR(&ip_info.ip));

        /* Captive Portal wird spaeter auf dem Hauptserver registriert */
        portal_needed = true;
        ESP_LOGI(TAG, "WiFi-Portal wird benoetigt (keine Credentials)");
    }

    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    portal_registered = false;
    portal_needed = false;

    esp_wifi_stop();
    esp_wifi_deinit();

    wifi_connected = false;
    current_ip[0] = '\0';

    ESP_LOGI(TAG, "WiFi deinitialisiert");
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return wifi_connected;
}

const char *wifi_get_ip(void)
{
    return current_ip;
}

void wifi_reset_credentials(void)
{
    nvs_config_set_str("wifi_ssid", "");
    nvs_config_set_str("wifi_password", "");
    ESP_LOGI(TAG, "WiFi-Credentials zurueckgesetzt");
}