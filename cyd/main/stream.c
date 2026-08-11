/*
 * stream.c - WiFi (Station) + Kamerastream-Abruf + JPEG-Dekodierung + Anzeige
 *
 * Verbindet sich als WiFi-Station mit dem ESP32-CAM SoftAP ("Cam-AP", offen),
 * liest den Roh-JPEG-Stream der CAM (TCP Port 8080: 4-Byte-Laenge + JPEG),
 * dekodiert die Frames mit esp_jpeg (Skalierung 1:2 -> 200x148) und zeigt sie
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
#include "lwip/sockets.h"
#include "lwip/inet.h"
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

/* =====================================================================
 * 4-stufige Dual-Core-Pipeline (3 Tasks, 2 Kerne):
 *   fetch_task (Kern 0): HTTP-Fetch in 2 JPEG-Puffer (Ping-Pong)
 *   decode_task (Kern 1): JPEG-Dekodierung (JPEG-Puffer -> decoded-Puffer)
 *   blit_task  (Kern 0): Anzeige (SPI) + OSD
 * Dadurch laufen Fetch, Dekodierung und Anzeige parallel (ueber Puffer-
 * Queues) statt seriell -> mehr fps. WICHTIG: Der WiFi-Stack laeuft fest auf
 * Kern 0 - die CPU-lastige Dekodierung liegt deshalb auf Kern 1 und blockiert
 * den WiFi-Stack (und damit den Fetch) nicht mehr.
 * ===================================================================== */
#define STREAM_JPEG_BUFS 2
static uint8_t *s_jpeg_buf[STREAM_JPEG_BUFS];   /* 2 JPEG-Puffer (fetch -> decode) */
static int      s_jpeg_len[STREAM_JPEG_BUFS];   /* Laenge des Frames je Puffer */
static QueueHandle_t s_jpeg_free_q;             /* freie JPEG-Puffer (fetch holt) */
static QueueHandle_t s_jpeg_ready_q;            /* fertige JPEG-Puffer (decode holt) */

#define STREAM_DEC_BUFS 2
static uint16_t *s_dec_buf[STREAM_DEC_BUFS];    /* 2 dekodierte RGB565-Puffer */
static size_t    s_dec_cap[STREAM_DEC_BUFS];    /* Kapazitaet der Puffer      */
static int       s_dec_w[STREAM_DEC_BUFS];      /* Breite des dekodierten Frames */
static int       s_dec_h[STREAM_DEC_BUFS];      /* Hoehe  des dekodierten Frames */
static QueueHandle_t s_dec_free_q;              /* freie dekodierte Puffer (decode holt) */
static QueueHandle_t s_dec_ready_q;             /* fertige dekodierte Puffer (blit holt) */

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

    /* Power Save AUS: Der Modem-Sleep verursachte am CYD periodische
     * Verbindungsabbrueche (alle ~2,4s "WLAN getrennt, verbinde neu..."),
     * weil der Cam-AP ein langes Beacon-Intervall hat und der CYD im
     * Power-Save-Zyklus als inaktiv gilt. Konstanter Empfang = stabile
     * Verbindung = mehr fps. */
    esp_wifi_set_ps(WIFI_PS_NONE);

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

/* Ziel-Puffer der Pipeline: .data zeigt je auf s_jpeg_buf[idx] */
static http_buf_t s_fetch_buf;

/* ------------------------------------------------------------------ */
/* fetch_task (Kern 0): WiFi + Roh-JPEG-Stream der CAM (TCP 8080)      */
/* ------------------------------------------------------------------ */
static void fetch_task(void *arg)
{
    wifi_sta_init();

    char host[40] = CAM_HOST_DEFAULT;
    char *h = nvs_config_get_str("cyd_host", "");
    if (h && strlen(h) > 0) { strncpy(host, h, sizeof(host) - 1); host[sizeof(host) - 1] = 0; }
    if (h) free(h);

    /* Zwei JPEG-Puffer allokieren (klassischer CYD hat kein PSRAM) */
    for (int i = 0; i < STREAM_JPEG_BUFS; i++) {
        s_jpeg_buf[i] = heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_8BIT);
        if (!s_jpeg_buf[i]) {
            ESP_LOGE(TAG, "Nicht genug RAM fuer JPEG-Puffer %d", i);
            ui_set_status("RAM-Fehler");
            vTaskDelete(NULL);
        }
    }

    int sock = -1;
    int consec_fail = 0;   /* aufeinanderfolgende Stream-Fehler */

    while (1) {
        if (!s_connected) {
            /* Verbindungsaufbau / keine Verbindung: warten (blit-Task zeichnet OSD) */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Verbindung zum CAM-Stream aufbauen/wiederherstellen (persistent) */
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port = htons(STREAM_TCP_PORT);
                inet_pton(AF_INET, host, &sa.sin_addr);
                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
                    close(sock);
                    sock = -1;
                } else {
                    ESP_LOGI(TAG, "Stream verbunden (Port %d)", STREAM_TCP_PORT);
                    consec_fail = 0;
                }
            }
            if (sock < 0) {
                consec_fail++;
                ESP_LOGW(TAG, "Stream-Connect fehlgeschlagen (%d)", consec_fail);
                if (consec_fail >= STREAM_FAIL_THRESHOLD) {
                    ESP_LOGW(TAG, "CAM nicht erreichbar - WiFi-Reconnect");
                    ui_set_status("CAM weg - Reconnect");
                    esp_wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_wifi_connect();
                    consec_fail = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        /* Auf einen freien JPEG-Puffer warten (decode ist mit dem letzten fertig) */
        int j = -1;
        if (xQueueReceive(s_jpeg_free_q, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;   /* kein freier Puffer (decode haengt) - erneut versuchen */
        }

        /* ---- Frame lesen: 4-Byte-Laenge (big-endian) + JPEG-Daten ---- */
        bool stream_ok = true;
        uint8_t lbuf[4];
        int got = 0;
        while (got < 4) {
            int n = recv(sock, lbuf + got, 4 - got, 0);
            if (n <= 0) { stream_ok = false; break; }
            got += n;
        }
        uint32_t len = 0;
        if (stream_ok) {
            len = ((uint32_t)lbuf[0] << 24) | ((uint32_t)lbuf[1] << 16) |
                  ((uint32_t)lbuf[2] << 8) | (uint32_t)lbuf[3];
            if (len == 0 || len > JPEG_BUF_SIZE) stream_ok = false;
        }
        if (stream_ok) {
            s_fetch_buf.data = s_jpeg_buf[j];
            s_fetch_buf.len = 0;
            s_fetch_buf.cap = JPEG_BUF_SIZE;
            while (s_fetch_buf.len < (int)len) {
                int n = recv(sock, s_fetch_buf.data + s_fetch_buf.len,
                             (int)len - s_fetch_buf.len, 0);
                if (n <= 0) { stream_ok = false; break; }
                s_fetch_buf.len += n;
            }
        }

        if (!stream_ok || s_fetch_buf.len <= 8) {
            /* Stream gestoert: Socket schliessen (Reconnect im naechsten Loop) */
            if (sock >= 0) { close(sock); sock = -1; }
            xQueueSend(s_jpeg_free_q, &j, 0);
            consec_fail++;
            ESP_LOGW(TAG, "Stream-Fehler (%d)", consec_fail);
            if (consec_fail >= STREAM_FAIL_THRESHOLD) {
                ESP_LOGW(TAG, "CAM nicht erreichbar - WiFi-Reconnect");
                ui_set_status("CAM weg - Reconnect");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_wifi_connect();
                consec_fail = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        consec_fail = 0;

        /* Frame bereit: Nur gueltige JPEGs (SOI/EOI-Marker) weitergeben.
         * Unfertige Stream-Frames wuerden sonst Dekodierfehler (farbige
         * Linien) erzeugen - sie werden verworfen. */
        uint8_t *jd = s_fetch_buf.data;
        if (s_fetch_buf.len > 8 &&
            jd[0] == 0xFF && jd[1] == 0xD8 &&
            jd[s_fetch_buf.len - 2] == 0xFF && jd[s_fetch_buf.len - 1] == 0xD9) {
            s_jpeg_len[j] = s_fetch_buf.len;
            xQueueSend(s_jpeg_ready_q, &j, 0);
            /* Stream liefert Frames -> irrefuehrenden "WLAN getrennt"-Status
             * loeschen (der WiFi-Stack kann kurz einen Disconnect gemeldet
             * haben, waehrend die TCP-Verbindung zum Stream weiterlaeuft). */
            ui_set_status("Verbunden");
        } else {
            xQueueSend(s_jpeg_free_q, &j, 0);   /* kaputtes Frame verwerfen */
        }
    }
}

/* ------------------------------------------------------------------ */
/* decode_task (Kern 1): JPEG-Dekodierung (CPU-lastig, kein WiFi-Stoer) */
/* ------------------------------------------------------------------ */
static void decode_task(void *arg)
{
    /* Diagnose: nur bei Groessenwechsel loggen */
    static int last_log_w = 0, last_log_h = 0;

    while (1) {
        int j = -1, d = -1;
        if (xQueueReceive(s_jpeg_ready_q, &j, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;   /* kein fertiger JPEG - warten */
        }
        if (xQueueReceive(s_dec_free_q, &d, pdMS_TO_TICKS(500)) != pdTRUE) {
            /* kein freier decoded-Puffer (blit haengt) - JPEG-Puffer zurueck */
            xQueueSend(s_jpeg_free_q, &j, 0);
            continue;
        }

        uint8_t *jpeg_buf = s_jpeg_buf[j];
        int jpeg_len = s_jpeg_len[j];

        esp_jpeg_image_cfg_t jcfg = {
            .indata = jpeg_buf,
            .indata_size = (uint32_t)jpeg_len,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = JPEG_DECODE_SCALE,   /* nur fuer get_image_info (volle Dims) */
            .flags = { .swap_color_bytes = 1 },
        };
        esp_jpeg_image_output_t info;
        bool decoded_ok = false;
        if (esp_jpeg_get_image_info(&jcfg, &info) == ESP_OK &&
            info.width > 0 && info.height > 0) {
            /* Dekodier-Skalierung: 1:2 nur fuer kleine Bilder (QVGA 320x240),
             * sonst 1:4. CIF 400x296 -> 100x74: schneller Decode (~90ms) und
             * mehr fps (die 9fps-Konfiguration). 1:2 (200x148) war zwar
             * schaerfer, kostete aber ~120ms Decode und damit fps. */
            int div = (info.width <= 320 && info.height <= 240) ? 2 : 4;
            size_t need = (size_t)(info.width / div) * (info.height / div) * 2;
            if (info.width != last_log_w || info.height != last_log_h) {
                ESP_LOGI(TAG, "JPEG %d B -> %dx%d (Skalierung 1:%d, Puffer %d B)",
                         jpeg_len, info.width, info.height, div, (int)need);
                last_log_w = info.width;
                last_log_h = info.height;
            }
            if (need > MAX_DECODED_BUF) {
                ESP_LOGW(TAG, "Bild %dx%d zu gross (%d B > %d) - Frame uebersprungen",
                         info.width, info.height, (int)need, MAX_DECODED_BUF);
            } else {
                if (!s_dec_buf[d] || need > s_dec_cap[d]) {
                    heap_caps_free(s_dec_buf[d]);
                    s_dec_buf[d] = heap_caps_malloc(need, MALLOC_CAP_8BIT);
                    s_dec_cap[d] = s_dec_buf[d] ? need : 0;
                }
                if (s_dec_buf[d]) {
                    jcfg.out_scale = (div == 2) ? JPEG_IMAGE_SCALE_1_2 : JPEG_IMAGE_SCALE_1_4;
                    jcfg.outbuf = (uint8_t *)s_dec_buf[d];
                    jcfg.outbuf_size = s_dec_cap[d];
                    esp_jpeg_image_output_t out;
                    /* Diagnose: Dekodierzeit messen (laeuft auf Kern 1) */
                    TickType_t td0 = xTaskGetTickCount();
                    esp_err_t dec_ok = esp_jpeg_decode(&jcfg, &out);
                    TickType_t td1 = xTaskGetTickCount();
                    int dec_ms = (int)((td1 - td0) * portTICK_PERIOD_MS);
                    if (dec_ok == ESP_OK && out.width > 0 && out.height > 0) {
                        s_dec_w[d] = (int)out.width;
                        s_dec_h[d] = (int)out.height;
                        decoded_ok = true;
                        if (dec_ms > 50) {
                            ESP_LOGW(TAG, "Dekodierung langsam: %d ms (%dx%d)",
                                     dec_ms, out.width, out.height);
                        }
                    }
                }
            }
        }
        /* JPEG-Puffer freigeben (decode ist fertig damit) */
        xQueueSend(s_jpeg_free_q, &j, 0);
        if (decoded_ok) {
            xQueueSend(s_dec_ready_q, &d, 0);
        } else {
            xQueueSend(s_dec_free_q, &d, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* blit_task (Kern 0): Anzeige (SPI) + OSD                             */
/* ------------------------------------------------------------------ */
static void blit_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t frame_count = 0;
    bool was_connected = false;
    TickType_t last_overlay = 0;

    while (1) {
        /* Auf einen fertig dekodierten Frame warten (max. 500 ms: so bleibt der
         * Overlay/OSD auch ohne Frames aktiv und die UI friert nicht ein). */
        int d = -1;
        bool have_frame = (xQueueReceive(s_dec_ready_q, &d, pdMS_TO_TICKS(500)) == pdTRUE);

        if (have_frame) {
            /* Wenn Touch-Menue ODER Diagnose-Test aktiv sind: Bild nicht zeichnen
             * (sonst wuerde es Menue bzw. Geometrie-Test uebermalen). */
            if (!ui_menu_is_open() && !ui_diag_is_active()) {
                /* Diagnose: Anzeigezeit messen (laeuft jetzt auf Kern 1) */
                TickType_t tb0 = xTaskGetTickCount();
                display_blit_decoded(s_dec_buf[d], s_dec_w[d], s_dec_h[d]);
                TickType_t tb1 = xTaskGetTickCount();
                int blit_ms = (int)((tb1 - tb0) * portTICK_PERIOD_MS);
                if (blit_ms > 30) {
                    ESP_LOGW(TAG, "Anzeige langsam: %d ms (%dx%d)",
                             blit_ms, s_dec_w[d], s_dec_h[d]);
                }
            }
            frame_count++;
            /* Puffer freigeben, damit fetch+decode ihn wieder fuellen kann */
            xQueueSend(s_dec_free_q, &d, 0);
        }

        /* Bildschirm leeren, wenn die Verbindung gerade verloren ging */
        if (!s_connected && was_connected) {
            display_fill(0x0000);
            ui_set_status("WLAN getrennt");
            last_overlay = 0;   /* OSD sofort wieder zeichnen */
        } else if (s_connected && !was_connected) {
            /* Wieder verbunden: irrefuehrenden "WLAN getrennt"-Status zuruecksetzen
             * (wurde einmal gesetzt und blieb trotz laufendem Stream stehen). */
            ui_set_status("Verbunden");
            last_overlay = 0;   /* OSD sofort wieder zeichnen */
        }
        was_connected = s_connected;

        /* Overlay/Menue/OSD regelmaessig aktualisieren (ALLE 500ms, unabhaengig
         * von Frames): verhindert, dass die UI einfriert, wenn der HTTP-Fetch
         * haengt (z.B. iPhone am Cam-AP) oder das Touch-Menue offen ist. */
        if ((xTaskGetTickCount() - last_overlay) >= pdMS_TO_TICKS(500)) {
            ui_draw_overlay();
            last_overlay = xTaskGetTickCount();
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last) >= pdMS_TO_TICKS(1000)) {
            s_fps = frame_count;
            frame_count = 0;
            last = now;
            /* Diagnose: echte fps alle 5s ins Log (sonst nur im OSD sichtbar) */
            static int sec = 0;
            if (++sec % 5 == 0) {
                ESP_LOGI(TAG, "Diagnose: fps=%lu verbunden=%d",
                         (unsigned long)s_fps, (int)s_connected);
            }
        }
    }
}

void stream_start(void)
{
    /* JPEG- und decoded-Puffer-Queues: initial alle Puffer frei (Indizes 0 und 1) */
    s_jpeg_free_q  = xQueueCreate(STREAM_JPEG_BUFS, sizeof(int));
    s_jpeg_ready_q = xQueueCreate(STREAM_JPEG_BUFS, sizeof(int));
    for (int i = 0; i < STREAM_JPEG_BUFS; i++) {
        xQueueSend(s_jpeg_free_q, &i, 0);
    }
    s_dec_free_q  = xQueueCreate(STREAM_DEC_BUFS, sizeof(int));
    s_dec_ready_q = xQueueCreate(STREAM_DEC_BUFS, sizeof(int));
    for (int i = 0; i < STREAM_DEC_BUFS; i++) {
        xQueueSend(s_dec_free_q, &i, 0);
    }

    /* Kern-Verteilung wie in der bewaehrten 9fps-Konfiguration: fetch+decode
     * auf Kern 0 (dort laeuft auch der WiFi-Stack; der Fetch ist I/O-gebunden),
     * Anzeige (SPI) auf Kern 1. So laufen Dekodierung (Kern 0) und Anzeige
     * (Kern 1) parallel -> mehr fps. */
    if (xTaskCreatePinnedToCore(fetch_task,  "fetch",  TASK_STACK_STREAM, NULL,
                                TASK_PRIORITY_STREAM, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(decode_task, "decode", TASK_STACK_STREAM, NULL,
                                TASK_PRIORITY_STREAM, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(blit_task,   "blit",   TASK_STACK_STREAM, NULL,
                                TASK_PRIORITY_STREAM, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Task-Erstellung fehlgeschlagen");
    }
}

bool stream_is_connected(void)
{
    return s_connected;
}

uint32_t stream_get_fps(void)
{
    return s_fps;
}
