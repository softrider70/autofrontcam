/*
 * main.c - Hauptprogramm Autofrontcam (ESP32-CAM, AI-Thinker)
 *
 * ESP32-CAM mit OV2640 Kamera. Bietet:
 *  - MJPEG-Stream ueber WiFi (http://<ip>:81/stream)
 *  - Einzelbilder (http://<ip>:81/capture)
 *  - OTA-Updates ueber eingebetteten Webserver
 *  - WiFi Captive Portal (AP bei fehlenden Credentials)
 *  - Stack- und Heap-Monitoring
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "config.h"
#include "camera.h"
#include "wifi.h"
#include "ota.h"
#include "nvs_config.h"
#include "voltage.h"
#include "sleep.h"
#include "lines.h"
#include "stack_monitor.h"
#include "heap_monitor.h"

static const char *TAG = "AUTOCAM";

/* =====================================================================
 * MJPEG-Stream: Boundary fuer Multipart-Antwort
 * ===================================================================== */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n"
    "X-Timestamp: %lu\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    uint8_t *buf = NULL;
    size_t len = 0;

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        /* Frame aufnehmen */
        if (camera_capture_jpeg(&buf, &len) != ESP_OK) {
            ESP_LOGE(TAG, "Capture fehlgeschlagen");
            continue;
        }

        /* Boundary + Header senden */
        char part_hdr[96];
        snprintf(part_hdr, sizeof(part_hdr), STREAM_PART, (unsigned)len,
                 (unsigned long)esp_timer_get_time() / 1000000);

        ret = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (ret != ESP_OK) break;
        ret = httpd_resp_send_chunk(req, part_hdr, strlen(part_hdr));
        if (ret != ESP_OK) break;

        /* Bilddaten senden */
        ret = httpd_resp_send_chunk(req, (const char *)buf, len);
        camera_fb_return();
        if (ret != ESP_OK) break;
    }

    return ret;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    uint8_t *buf = NULL;
    size_t len = 0;

    if (camera_capture_jpeg(&buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Capture fehlgeschlagen");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "inline; filename=capture.jpg");
    esp_err_t ret = httpd_resp_send(req, (const char *)buf, len);
    camera_fb_return();
    return ret;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    /* Eingebettete Web-UI (components/main/index.html) */
    extern const uint8_t index_html_start[] asm("_binary_index_html_start");
    extern const uint8_t index_html_end[] asm("_binary_index_html_end");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

/* =====================================================================
 * API: /api/config  (GET = JSON, POST = Form-encoded)
 * ===================================================================== */

/* Wert "key=..." aus Form-Body extrahieren */
static bool form_get(const char *body, const char *key, char *out, size_t maxlen)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);

        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = seg - klen - 1;
            if (vlen >= maxlen) vlen = maxlen - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    char buf[512];
    line_cfg_t r, y;
    lines_get(&r, &y);

    snprintf(buf, sizeof(buf),
             "{\"voltage\":%.1f,\"mode\":%d,\"threshold\":%.1f,"
             "\"red\":{\"x\":%d,\"angle\":%d,\"w\":%d,\"on\":%d},"
             "\"yellow\":{\"x\":%d,\"angle\":%d,\"w\":%d,\"on\":%d}}",
             voltage_get_last(),
             voltage_is_regulated() ? 1 : 0,
             voltage_get_threshold(),
             r.x_percent, r.angle_deg, r.width_px, r.enabled ? 1 : 0,
             y.x_percent, y.angle_deg, y.width_px, y.enabled ? 1 : 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char body[320];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_OK;
    }
    body[ret] = '\0';

    char tmp[16];
    line_cfg_t r, y;
    lines_get(&r, &y);

    if (form_get(body, "mode", tmp, sizeof(tmp)))
        voltage_set_mode(atoi(tmp) != 0);
    if (form_get(body, "threshold", tmp, sizeof(tmp)))
        voltage_set_threshold((float)atof(tmp));

    if (form_get(body, "lx", tmp, sizeof(tmp))) r.x_percent = clamp_int(atoi(tmp), 0, 100);
    if (form_get(body, "la", tmp, sizeof(tmp))) r.angle_deg = clamp_int(atoi(tmp), -45, 45);
    if (form_get(body, "lw", tmp, sizeof(tmp))) r.width_px = clamp_int(atoi(tmp), 1, 15);
    if (form_get(body, "lon", tmp, sizeof(tmp))) r.enabled = atoi(tmp) != 0;

    if (form_get(body, "l2x", tmp, sizeof(tmp))) y.x_percent = clamp_int(atoi(tmp), 0, 100);
    if (form_get(body, "l2a", tmp, sizeof(tmp))) y.angle_deg = clamp_int(atoi(tmp), -45, 45);
    if (form_get(body, "l2w", tmp, sizeof(tmp))) y.width_px = clamp_int(atoi(tmp), 1, 15);
    if (form_get(body, "l2on", tmp, sizeof(tmp))) y.enabled = atoi(tmp) != 0;

    lines_set(&r, &y);

    ESP_LOGI(TAG, "Config gespeichert (Modus=%d, Schwelle=%.1fV, Rot x=%d a=%d w=%d, Gelb x=%d a=%d w=%d)",
             voltage_is_regulated() ? 1 : 0, voltage_get_threshold(),
             r.x_percent, r.angle_deg, r.width_px,
             y.x_percent, y.angle_deg, y.width_px);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static void start_stream_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = STREAM_PORT;
    cfg.max_uri_handlers = 16;   /* Web-UI + Stream + API + OTA + Portal */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Stream-Server Start fehlgeschlagen");
        return;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_uri_t stream = {
        .uri = "/stream", .method = HTTP_GET, .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_uri_t capture = {
        .uri = "/capture", .method = HTTP_GET, .handler = capture_handler,
        .user_ctx = NULL
    };
    httpd_uri_t api_get = {
        .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t api_post = {
        .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &stream);
    httpd_register_uri_handler(server, &capture);
    httpd_register_uri_handler(server, &api_get);
    httpd_register_uri_handler(server, &api_post);

    /* OTA-Handler (/update, /status) auf dem Hauptserver registrieren */
    ota_register_handlers(server);

    /* WiFi-Captive-Portal nur registrieren, wenn benoetigt (keine Credentials) */
    if (wifi_portal_needed()) {
        wifi_register_portal(server);
    }

    ESP_LOGI(TAG, "Haupt-Webserver auf Port %d gestartet", STREAM_PORT);
}

/* =====================================================================
 * LED-Blinken (Status)
 * ===================================================================== */
static void led_task(void *pvParameters)
{
    bool state = LED_OFF;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        state = !state;
        gpio_set_level(LED_GPIO, state);
    }
}

/* =====================================================================
 * Hauptprogramm
 * ===================================================================== */
void app_main(void)
{
    /* OTA-Rollback: laufende Firmware als gueltig markieren.
     * WICHTIG: vor dem Deep-Sleep-Check (sleep_check_early), damit eine neue
     * Firmware, die direkt in den Sleep geht, nicht zurueckgerollt wird. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "=== Autofrontcam (ESP32-CAM) start ===");

    /* NVS initialisieren */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: erase + init");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    nvs_config_init();

    /* Kalibrierungslinien aus NVS laden */
    lines_init();

    /* Spannungsmessung initialisieren (Batterie ueber Spannungsteiler) */
    ret = voltage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Spannungsmessung Init fehlgeschlagen - weiter ohne");
    }

    /* Early-Check: wenn "Geregelt" und Spannung zu niedrig -> Deep-Sleep */
    sleep_check_early();

    /* Status-LED konfigurieren */
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO) | (1ULL << LED_FLASH_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, LED_OFF);              /* Status-LED aus (active-low) */
    gpio_set_level(LED_FLASH_GPIO, FLASH_LED_OFF);  /* Flash-LED aus (aktiv-high) */

    /* Monitore starten */
    stack_monitor_init();
    heap_monitor_init();

    /* Kamera initialisieren */
    ESP_LOGI(TAG, "Initialize Kamera...");
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Kamera init fehlgeschlagen - weiter ohne Kamera");
    }

    /* WiFi initialisieren (AP bei fehlenden Credentials) */
    ESP_LOGI(TAG, "Initialize WiFi...");
    wifi_init();

    /* OTA-Webserver starten */
    ESP_LOGI(TAG, "Start OTA webserver...");
    ota_init();

    /* MJPEG-Stream-Server starten */
    start_stream_server();

    /* LED-Task */
    xTaskCreate(led_task, "led", TASK_STACK_MONITOR, NULL,
                TASK_PRIORITY_MONITOR, NULL);

    /* Erste Spannungsmessung fuer den Log */
    float batt = voltage_read_batt();
    ESP_LOGI(TAG, "Batterie: %.1fV (Modus: %s, Grenzwert: %.1fV)",
             batt, voltage_is_regulated() ? "Geregelt" : "Dauerbetrieb",
             voltage_get_threshold());

    /* Spannungs-Monitor fuer Sleep/Wake-up (nur im Modus "Geregelt") */
    sleep_start_monitor();

    ESP_LOGI(TAG, "=== System ready ===");
}
