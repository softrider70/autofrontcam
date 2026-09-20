/*
 * stream.c - Display-Client (Kamerabild von der ESP32-CAM)
 *
 * Verbindet sich als WiFi-Station mit dem ESP32-CAM SoftAP ("Cam-AP", offen),
 * liest den Roh-JPEG-Stream der CAM (TCP Port 8080: 4-Byte-Laenge big-endian
 * + JPEG), dekodiert die Frames mit esp_jpeg (automatische Skalierung auf das
 * 480x320-Display) und zeigt sie per display_blit an. Ohne UI/OSD.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "jpeg_decoder.h"
#include "config.h"
#include "display.h"
#include "stream.h"
#include "ui.h"

static const char *TAG = "s3lcd_stream";
static volatile bool s_ip_ok = false;

/* TCP-Socket des Roh-JPEG-Streams + Reconnect-Anforderung (Trigger aus der UI) */
static int s_sock = -1;
static volatile bool s_reconn_req = false;

void stream_request_reconnect(void)
{
    s_reconn_req = true;
}

/* ------------------------------------------------------------------ */
/* Helligkeit/Kontrast client-seitig (64K-LUT in PSRAM)                */
/* ------------------------------------------------------------------ */
static uint16_t *s_lut = NULL;
static int s_lut_bri = 0, s_lut_con = 0;

void stream_set_picture(int bri, int con)
{
    if (bri < -2) bri = -2;
    if (bri > 2)  bri = 2;
    if (con < -2) con = -2;
    if (con > 2)  con = 2;
    if (s_lut && bri == s_lut_bri && con == s_lut_con) return;
    if (!s_lut && bri == 0 && con == 0) {   /* nichts zu tun, LUT nicht bauen */
        s_lut_bri = bri; s_lut_con = con;
        return;
    }

    if (!s_lut) {
        s_lut = heap_caps_malloc(65536 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_lut) {
            ESP_LOGW(TAG, "Kein PSRAM fuer Bild-LUT - Helligkeit/Kontrast aus");
            return;
        }
    }

    /* Kontrast als Steigung um die Mitte, Helligkeit als Offset:
     * con -2..2 -> 50..150 %, bri -2..2 -> -80..+80 */
    int scale = 100 + con * 25;
    int off   = bri * 40;
    for (int i = 0; i < 65536; i++) {
        int r = ((i >> 11) & 0x1F) * 255 / 31;
        int g = ((i >> 5) & 0x3F) * 255 / 63;
        int b = (i & 0x1F) * 255 / 31;
        r = ((r - 128) * scale) / 100 + 128 + off;
        g = ((g - 128) * scale) / 100 + 128 + off;
        b = ((b - 128) * scale) / 100 + 128 + off;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        s_lut[i] = (uint16_t)(((r * 31 / 255) << 11) |
                              ((g * 63 / 255) << 5) |
                              (b * 31 / 255));
    }
    s_lut_bri = bri;
    s_lut_con = con;
    ESP_LOGI(TAG, "Bildparameter client-seitig: Helligkeit %d, Kontrast %d", bri, con);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_ok = false;
        ESP_LOGW(TAG, "WLAN getrennt - reconnect");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip_ok = true;
        ESP_LOGI(TAG, "IP erhalten: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID_DEFAULT, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS_DEFAULT, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    /* Statische IP am Cam-AP (Sender 10.1.1.1, wir 10.1.1.3) */
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n) {
        esp_netif_dhcpc_stop(n);
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_str_to_ip4(S3_STATIC_IP, &ip.ip);
        esp_netif_str_to_ip4(S3_NETMASK, &ip.netmask);
        esp_netif_str_to_ip4(S3_GATEWAY, &ip.gw);
        esp_netif_set_ip_info(n, &ip);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Liest ein JPEG-Frame vom Roh-Stream der CAM (TCP 8080): erst 4 Byte
 * Laenge (big-endian), dann die JPEG-Daten. Liefert die Laenge oder 0.
 * Die TCP-Verbindung bleibt persistent; bei Stream-/Verbindungsfehlern wird
 * der Socket geschlossen und beim naechsten Aufruf neu verbunden. */
static size_t fetch_frame_tcp(uint8_t *dst, size_t cap)
{
    int sock = s_sock;

    /* Reconnect angefordert (UI-Trigger "Neu verbinden")? */
    if (s_reconn_req) {
        s_reconn_req = false;
        if (sock >= 0) { close(sock); sock = -1; s_sock = -1; }
        ESP_LOGI(TAG, "Reconnect angefordert");
    }

    if (sock < 0) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        s_sock = sock;
        if (sock >= 0) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(STREAM_TCP_PORT);
            if (inet_pton(AF_INET, CAM_HOST_DEFAULT, &sa.sin_addr) != 1) {
                close(sock);
                sock = -1;
                s_sock = -1;
                ESP_LOGE(TAG, "Ungueltige CAM-IP %s", CAM_HOST_DEFAULT);
                return 0;
            }
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
                ESP_LOGW(TAG, "Stream-Connect zu %s:%d fehlgeschlagen",
                         CAM_HOST_DEFAULT, STREAM_TCP_PORT);
                close(sock);
                sock = -1;
                s_sock = -1;
                return 0;
            }
            ESP_LOGI(TAG, "Stream verbunden: %s:%d", CAM_HOST_DEFAULT, STREAM_TCP_PORT);
        } else {
            ESP_LOGE(TAG, "socket() fehlgeschlagen");
            return 0;
        }
    }

    /* Frame-Laenge (4 Byte, big-endian) lesen */
    uint8_t lbuf[4];
    size_t got = 0;
    while (got < sizeof(lbuf)) {
        int n = recv(sock, lbuf + got, sizeof(lbuf) - got, 0);
        if (n <= 0) goto stream_err;
        got += (size_t)n;
    }
    uint32_t len = ((uint32_t)lbuf[0] << 24) | ((uint32_t)lbuf[1] << 16) |
                   ((uint32_t)lbuf[2] << 8) | (uint32_t)lbuf[3];
    if (len == 0 || len > cap) {
        ESP_LOGW(TAG, "Ungueltige Frame-Laenge %u", (unsigned)len);
        goto stream_err;
    }

    /* JPEG-Daten lesen */
    size_t total = 0;
    while (total < len) {
        int n = recv(sock, dst + total, (int)(len - total), 0);
        if (n <= 0) goto stream_err;
        total += (size_t)n;
    }

    /* SOI/EOI-Pruefung (unfertige Stream-Frames verwerfen) */
    if (total > 8 && dst[0] == 0xFF && dst[1] == 0xD8 &&
        dst[total - 2] == 0xFF && dst[total - 1] == 0xD9) {
        return total;
    }

stream_err:
    ESP_LOGW(TAG, "Stream-Fehler - Socket geschlossen (Reconnect)");
    if (sock >= 0) { close(sock); sock = -1; s_sock = -1; }
    return 0;
}

/* JPEG dekodieren und zentriert anzeigen. */
static void show_jpeg(const uint8_t *jpeg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)len,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        /* WICHTIG: swap=0 (Little-Endian in Puffer) - display_blit liest die
         * Pixel als uint16_t (XTensa little-endian) und sendet high-first.
         * NICHT wie CYD (dessen blit_decoded sendet roh big-endian -> swap=1)! */
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t info = { 0 };
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        return;
    }

    /* Groesste Reduktion waehlen, die in den Videobereich (nach der linken
     * UI-Spalte) passt */
    const int vw = TFT_WIDTH - UI_LEFT_W;
    int w = info.width, h = info.height;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    if (w > vw || h > TFT_HEIGHT)          scale = JPEG_IMAGE_SCALE_1_2;
    if (w / 2 > vw || h / 2 > TFT_HEIGHT)  scale = JPEG_IMAGE_SCALE_1_4;
    if (w / 4 > vw || h / 4 > TFT_HEIGHT)  scale = JPEG_IMAGE_SCALE_1_8;
    cfg.out_scale = scale;

    /* Ausgabepuffer in PSRAM (8 MB) - genug fuer 1:1 bis SVGA */
    uint32_t cap = (uint32_t)w * h * 2;
    uint16_t *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!out) {
        ESP_LOGW(TAG, "Kein PSRAM-Puffer (%u B) - Frame uebersprungen", (unsigned)cap);
        return;
    }
    cfg.outbuf = (uint8_t *)out;
    cfg.outbuf_size = cap;

    esp_jpeg_image_output_t img = { 0 };
    esp_err_t de = esp_jpeg_decode(&cfg, &img);
    if (de == ESP_OK && img.width > 0 && img.height > 0 &&
        img.width <= vw && img.height <= TFT_HEIGHT) {
        /* Helligkeit/Kontrast client-seitig anwenden (LUT, nur wenn gesetzt) */
        if (s_lut && (s_lut_bri != 0 || s_lut_con != 0)) {
            uint16_t *pp = out;
            for (uint32_t n = 0; n < (uint32_t)img.width * img.height; n++) {
                pp[n] = s_lut[pp[n]];
            }
        }
        /* Nach rechts verschoben (linke UI-Spalte frei), vertikal zentriert */
        int x = UI_LEFT_W + (vw - img.width) / 2;
        int y = (TFT_HEIGHT - img.height) / 2;
        /* Videobereich zuerst schwarz (verhindert Geister der Rotation) */
        display_fill_rect(UI_LEFT_W, 0, vw, TFT_HEIGHT, 0x0000);
        /* Ggf. drehen/strecken (Kamera-Ausrichtung + Keystone-Ausgleich).
         * Die Einpassung beim Drehen verhindert ein fast schwarzes Bild. */
        int deg = ui_get_img_deg();
        int sx = ui_get_stretch_x();
        int sy = ui_get_stretch_y();
        int ox = ui_get_offset_x();
        int oy = ui_get_offset_y();
        if (deg == 0 && sx == 100 && sy == 100 && ox == 0 && oy == 0) {
            display_blit(out, x, y, img.width, img.height);
        } else {
            display_blit_rot_fit(out, img.width, img.height,
                                 UI_LEFT_W, 0, vw, TFT_HEIGHT, deg, sx, sy, ox, oy);
        }
        static int64_t last_log = 0;
        int64_t now = esp_timer_get_time();
        if (now - last_log > 2000000) {
            int dg = ui_get_img_deg();
            ESP_LOGI(TAG, "Frame angezeigt: %ux%u (JPEG %u B, Scale %d, Dreh %d.%d Grad, Streck %d%%/%d%%, Versatz %+d/%+d px)",
                     (unsigned)img.width, (unsigned)img.height,
                     (unsigned)len, (int)scale, dg / 10, dg % 10,
                     ui_get_stretch_x(), ui_get_stretch_y(),
                     ui_get_offset_x(), ui_get_offset_y());
            last_log = now;
        }
    } else if (de != ESP_OK) {
        ESP_LOGW(TAG, "JPEG-Dekodierung fehlgeschlagen (%u B)", (unsigned)len);
    } else {
        ESP_LOGW(TAG, "JPEG zu gross fuer Display: %ux%u",
                 (unsigned)img.width, (unsigned)img.height);
    }
    heap_caps_free(out);
}

void stream_start(void)
{
    ESP_LOGI(TAG, "Stream-Client start (AP %s, statische IP %s)", WIFI_SSID_DEFAULT, S3_STATIC_IP);
    wifi_init_sta();

    int wait = 0;
    while (!s_ip_ok && wait < (WIFI_CONNECT_TIMEOUT_S * 10)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
    }
    if (!s_ip_ok) {
        ESP_LOGW(TAG, "Keine Verbindung zum Cam-AP nach %ds - versuche weiter",
                 WIFI_CONNECT_TIMEOUT_S);
    }

    uint8_t *jpeg = malloc(JPEG_BUF_SIZE);
    if (!jpeg) {
        ESP_LOGE(TAG, "JPEG-Puffer (%d B) fehlgeschlagen", JPEG_BUF_SIZE);
        return;
    }

    uint32_t frames = 0;
    uint32_t fails = 0;
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (!s_ip_ok) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        size_t len = fetch_frame_tcp(jpeg, JPEG_BUF_SIZE);
        if (len < 4) {
            fails++;
            if (fails >= STREAM_FAIL_THRESHOLD) {
                ESP_LOGW(TAG, "CAM-Stream nicht erreichbar - WiFi-Reconnect");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_wifi_connect();
                fails = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        fails = 0;
        display_lock();
        show_jpeg(jpeg, len);   /* dekodiert + zeichnet in Framebuffer */
        ui_draw_overlay();      /* Linien/Buttons in Framebuffer */
        display_commit();       /* Framebuffer 1x ans Panel */
        display_unlock();
        frames++;
        if ((frames % 25) == 0) {
            int64_t dt = (esp_timer_get_time() - t0) / 1000;
            ESP_LOGI(TAG, "%lu Frames in %lld ms (%.1f fps)",
                     (unsigned long)frames, (long long)dt,
                     (double)frames * 1000.0 / (double)(dt ? dt : 1));
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_POLL_MS));
    }
}
