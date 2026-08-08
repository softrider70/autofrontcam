/*
 * dns_server.c - DNS-Intercept fuer Autofrontcam (Captive Portal)
 *
 * Einfacher UDP-DNS-Server auf Port 53. Jede DNS-Abfrage wird mit einem
 * A-Record auf die AP-IP beantwortet (Wildcard). Damit landet z.B.
 * captive.apple.com auf dem ESP, und iOS/Android oeffnen den Browser
 * automatisch auf der Web-UI (http://10.1.1.1/).
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "config.h"
#include "dns_server.h"

static const char *TAG = "DNS";
static TaskHandle_t dns_task_h = NULL;
static volatile bool dns_running = false;

static void dns_server_task(void *pv)
{
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket-Erstellung fehlgeschlagen");
        vTaskDelete(NULL);
        return;
    }
    if (bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "Bind auf Port 53 fehlgeschlagen");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS-Intercept aktiv -> %s", WIFI_AP_IP);

    uint8_t buf[512];
    while (dns_running) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        /* Nur einfache Query (QR=0) mit mind. 1 Question beantworten */
        if ((buf[2] & 0x80) != 0) continue;

        /* Antwort-Header: QR=1, RD/RA, RCODE=0 -> 0x8180; ANCOUNT=1 */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;   /* ANCOUNT */
        buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT */
        buf[10] = 0x00; buf[11] = 0x00; /* ARCOUNT */

        /* Question-Namen ueberspringen (0-terminierte Labels) */
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
        }
        p += 5; /* 0-Byte + QTYPE(2) + QCLASS(2) */

        if (p + 16 > (int)sizeof(buf)) continue;

        /* Answer: Pointer auf Question-Name, Type A, Class IN, TTL, RDATA */
        buf[p++] = 0xC0; buf[p++] = 0x0C;   /* Name-Pointer */
        buf[p++] = 0x00; buf[p++] = 0x01;   /* Type A */
        buf[p++] = 0x00; buf[p++] = 0x01;   /* Class IN */
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C; /* TTL 60 */
        buf[p++] = 0x00; buf[p++] = 0x04;   /* RDLENGTH 4 */
        buf[p++] = WIFI_AP_IP_BYTE0;
        buf[p++] = WIFI_AP_IP_BYTE1;
        buf[p++] = WIFI_AP_IP_BYTE2;
        buf[p++] = WIFI_AP_IP_BYTE3;

        sendto(sock, buf, p, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (dns_running) return;
    dns_running = true;
    if (xTaskCreate(dns_server_task, "dns", 3072, NULL, 5, &dns_task_h) != pdPASS) {
        ESP_LOGE(TAG, "DNS-Task-Erstellung fehlgeschlagen");
        dns_running = false;
    }
}

void dns_server_stop(void)
{
    dns_running = false;
    dns_task_h = NULL;
}
