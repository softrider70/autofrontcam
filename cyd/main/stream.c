/*
 * stream.c - WiFi (Station) + Kamerastream-Abruf + JPEG-Dekodierung + Anzeige
 *
 * Verbindet sich als WiFi-Station mit dem ESP32-CAM SoftAP ("Cam-AP", offen),
 * holt per HTTP GET /capture einzelne JPEG-Frames ab, dekodiert sie mit
 * esp_jpeg (Skalierung 1:4 -> 160x120, kein PSRAM noetig) und zeigt sie
 * per display_blit_decoded() auf dem Display an.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "nvs_config.h"
#include "jpeg_decoder.h"
#include "config.h"
#include "display.h"
#include "stream.h"
#include "ui.h"

static const char *TAG = "stream";

static bool s_connected = false;
static uint32_t s_fps = 0;

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;

/* ------------------------------------------------------------------ */
/* WiFi                                                               */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "WLAN getrennt, verbinde neu...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Verbunden: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_sta_init(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    /* Feste IP (kein DHCP): Sender ist immer 10.1.1.1, Empfaenger immer 10.1.1.2 */
    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_ip_info_t ip_info = {0};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CYD_STATIC_IP, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CYD_GATEWAY, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CYD_NETMASK, &ip_info.netmask));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));
    ESP_LOGI(TAG, "Statische IP gesetzt: %s (GW %s)", CYD_STATIC_IP, CYD_GATEWAY);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ssid[33] = WIFI_SSID_DEFAULT;
    char pass[65] = WIFI_PASS_DEFAULT;
    char *s = nvs_config_get_str("cyd_ssid", "");
    if (s && strlen(s) > 0) { strncpy(ssid, s, sizeof(ssid) - 1); ssid[sizeof(ssid) - 1] = 0; }
    if (s) free(s);
    s = nvs_config_get_str("cyd_pass", "");
    if (s && strlen(s) > 0) { strncpy(pass, s, sizeof(pass) - 1); pass[sizeof(pass) - 1] = 0; }
    if (s) free(s);

    wifi_config_t wcfg = {
        .sta = {
            /* Schwellwert OPEN -> erlaubt Verbindungen zu offenen APs (Cam-AP)
             * UND zu gesicherten Netzen (WPA/WPA2). */
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    if (strlen(pass) > 0) {
        strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    }

    ESP_LOGI(TAG, "Verbinde mit SSID '%s' ...", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_S * 1000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Zeitueberschreitung beim WiFi-Verbinden - warte im Hintergrund auf Retry");
    }
}

/* ------------------------------------------------------------------ */
/* HTTP-Fetch des JPEG-Frames                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *data;
    int len;
    int cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        int room = buf->cap - buf->len;
        if (room > 0) {
            int n = (evt->data_len < room) ? evt->data_len : room;
            memcpy(buf->data + buf->len, evt->data, n);
            buf->len += n;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Stream-Task                                                         */
/* ------------------------------------------------------------------ */
static void stream_task(void *arg)
{
    wifi_sta_init();

    char host[40] = CAM_HOST_DEFAULT;
    char *h = nvs_config_get_str("cyd_host", "");
    if (h && strlen(h) > 0) { strncpy(host, h, sizeof(host) - 1); host[sizeof(host) - 1] = 0; }
    if (h) free(h);

    uint8_t *jpeg_buf = heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "Nicht genug RAM fuer JPEG-Puffer");
        ui_set_status("RAM-Fehler");
        vTaskDelete(NULL);
    }
    /* Dekodier-Puffer wird adaptiv anhand der tatsaechlichen JPEG-Groesse
     * angelegt (klassischer CYD hat kein PSRAM). */
    uint16_t *decoded = NULL;
    size_t decoded_cap = 0;
    static int last_log_w = 0, last_log_h = 0;

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, CAM_PORT_DEFAULT, CAM_CAPTURE_PATH);

    http_buf_t buf = { .data = jpeg_buf, .len = 0, .cap = JPEG_BUF_SIZE };

    esp_http_client_config_t hcfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &buf,
        .timeout_ms = STREAM_FETCH_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP-Client konnte nicht erstellt werden");
        ui_set_status("HTTP-Fehler");
        vTaskDelete(NULL);
    }

    TickType_t last = xTaskGetTickCount();
    uint32_t frame_count = 0;
    bool was_connected = false;

    while (1) {
        if (s_connected && client) {
            buf.len = 0;
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK && buf.len > 8) {
                esp_jpeg_image_cfg_t jcfg = {
                    .indata = jpeg_buf,
                    .indata_size = (uint32_t)buf.len,
                    .out_format = JPEG_IMAGE_FORMAT_RGB565,
                    .out_scale = JPEG_DECODE_SCALE,
                    .flags = { .swap_color_bytes = 1 },
                };
                esp_jpeg_image_output_t info;
                if (esp_jpeg_get_image_info(&jcfg, &info) == ESP_OK &&
                    info.width > 0 && info.height > 0) {
                    /* Diagnose: nur bei Groessenwechsel loggen */
                    if (info.width != last_log_w || info.height != last_log_h) {
                        ESP_LOGI(TAG, "JPEG %d B -> %dx%d, Puffer %d B", buf.len,
                                 info.width, info.height, info.output_len);
                        last_log_w = info.width;
                        last_log_h = info.height;
                    }
                    if (info.output_len > MAX_DECODED_BUF) {
                        ESP_LOGW(TAG, "Bild %dx%d zu gross (%d B > %d) - Frame uebersprungen",
                                 info.width, info.height, info.output_len, MAX_DECODED_BUF);
                    } else {
                        if (!decoded || info.output_len > decoded_cap) {
                            heap_caps_free(decoded);
                            decoded = heap_caps_malloc(info.output_len, MALLOC_CAP_8BIT);
                            decoded_cap = decoded ? info.output_len : 0;
                            if (!decoded) {
                                ESP_LOGW(TAG, "Kein RAM fuer %d B Dekodier-Puffer", info.output_len);
                            }
                        }
                        if (decoded) {
                            jcfg.outbuf = (uint8_t *)decoded;
                            jcfg.outbuf_size = decoded_cap;
                            esp_jpeg_image_output_t out;
                            if (esp_jpeg_decode(&jcfg, &out) == ESP_OK &&
                                out.width > 0 && out.height > 0) {
                                display_blit_decoded(decoded, out.width, out.height);
                                ui_draw_overlay();
                                frame_count++;
                            }
                        }
                    }
                }
            }
        }

        /* Bildschirm leeren, wenn die Verbindung gerade verloren ging */
        if (!s_connected && was_connected) {
            display_fill(0x0000);
            ui_set_status("WLAN getrennt");
        }
        was_connected = s_connected;
        if (!s_connected) {
            ui_draw_overlay();
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last) >= pdMS_TO_TICKS(1000)) {
            s_fps = frame_count;
            frame_count = 0;
            last = now;
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_POLL_MS));
    }
}

void stream_start(void)
{
    xTaskCreate(stream_task, "stream", TASK_STACK_STREAM, NULL, TASK_PRIORITY_STREAM, NULL);
}

bool stream_is_connected(void)
{
    return s_connected;
}

uint32_t stream_get_fps(void)
{
    return s_fps;
}
