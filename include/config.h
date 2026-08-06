/*
 * config.h - Hardware-Konfiguration fuer ESP32-CAM (AI-Thinker)
 *
 * Board: AI-Thinker ESP32-CAM
 * Chip: ESP32-D0WD-V3 (Dual-Core Xtensa LX6, 240MHz)
 * Kamera: OV2640 (SCCB/I2C, DVP parallel)
 * Flash: 4MB, PSRAM: 4MB (SPIRAM)
 * WiFi: 802.11 b/g/n
 */

#ifndef CONFIG_H
#define CONFIG_H


#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * Board-Identifikation
 * ===================================================================== */
#define BOARD_NAME              "AI-Thinker ESP32-CAM"
#define BOARD_CHIP              "ESP32-D0WD-V3"
#define APP_VERSION_MAJOR       0
#define APP_VERSION_MINOR       1

/* =====================================================================
 * LED (ESP32-CAM: rote Status-LED auf GPIO33, Flash-LED GPIO4)
 * ===================================================================== */
#define LED_GPIO                33          /* Status-LED (rot) */
#define LED_FLASH_GPIO          4           /* Kamera-Blitz */
#define LED_ON                  0           /* Status-LED (GPIO33) ist active-low */
#define LED_OFF                 1
#define FLASH_LED_ON            1           /* Flash-LED (GPIO4) ist AKTIV-HIGH */
#define FLASH_LED_OFF           0

/* =====================================================================
 * Kamera OV2640 - ESP32-CAM Pinbelegung (AI-Thinker)
 * ===================================================================== */
#define CAM_PIN_PWDN            32
#define CAM_PIN_RESET           -1
#define CAM_PIN_XCLK            0
#define CAM_PIN_SIOD            26
#define CAM_PIN_SIOC            27
#define CAM_PIN_Y9              35
#define CAM_PIN_Y8              34
#define CAM_PIN_Y7              39
#define CAM_PIN_Y6              36
#define CAM_PIN_Y5              21
#define CAM_PIN_Y4              19
#define CAM_PIN_Y3              18
#define CAM_PIN_Y2              5
#define CAM_PIN_VSYNC           25
#define CAM_PIN_HREF            23
#define CAM_PIN_PCLK            22

/* Kamera-Parameter */
#define CAM_XCLK_FREQ_HZ        16000000    /* 16 MHz: stabileres DVP-Timing (weniger NO-SOI) */
#define CAM_LEDC_CHANNEL        LEDC_CHANNEL_0
#define CAM_PIXEL_FORMAT        PIXFORMAT_JPEG
#define CAM_FRAME_SIZE          FRAMESIZE_SVGA    /* 800x600 */
#define CAM_JPEG_QUALITY        12
#define CAM_FB_COUNT            4           /* mehr Puffer = weniger NO-SOI/Ueberlauf */

/* =====================================================================
 * MicroSD (ESP32-CAM Karten-Slot, SPI)
 * ===================================================================== */
#define SD_CS_GPIO              4           /* SPI CS */
#define SD_SCK_GPIO             14
#define SD_MOSI_GPIO            15
#define SD_MISO_GPIO            2

/* =====================================================================
 * Spannungsmessung (Fahrzeugbatterie ueber Spannungsteiler)
 * ===================================================================== */
#define VOLT_PIN                35          /* GPIO35 = ADC1_CH7 (nur Input) */
#define VOLT_DIVIDER_FACTOR     5.55f       /* Batterie = ADC_V * 5,55 (100k/22k) */
#define VOLT_THRESHOLD_DEFAULT  128         /* 12,8V in 0,1V-Schritten (NVS u8) */
#define VOLT_MODE_DEFAULT       0           /* 0 = Dauerbetrieb, 1 = Geregelt */
#define VOLT_MONITOR_INTERVAL_MS 30000      /* Messintervall im Betrieb (30s) */
#define SLEEP_CHECK_INTERVAL_MS 10000       /* Wakeup-Intervall im Deep-Sleep (10s) */

/* =====================================================================
 * WiFi & OTA
 * ===================================================================== */
#define WIFI_AP_SSID            "Cam-AP"
#define WIFI_AP_IP              "10.1.1.1"
#define WIFI_AP_NETMASK         "255.255.255.0"
#define WIFI_AP_IP_BYTE0        10
#define WIFI_AP_IP_BYTE1        1
#define WIFI_AP_IP_BYTE2        1
#define WIFI_AP_IP_BYTE3        1
#define WIFI_CONNECT_TIMEOUT_S  15
#define WIFI_MAX_RETRY          3

#define OTA_WEBSERVER_PORT      80
#define OTA_CHECK_INTERVAL_MS   60000       /* 60s */

/* =====================================================================
 * mDNS
 * ===================================================================== */
#define MDNS_HOSTNAME           "autocam"

/* =====================================================================
 * FreeRTOS Task-Konfiguration
 * ===================================================================== */
#define TASK_STACK_CAM          4096
#define TASK_STACK_WIFI         4096
#define TASK_STACK_OTA          4096
#define TASK_STACK_MONITOR      1024

#define TASK_PRIORITY_CAM       5
#define TASK_PRIORITY_WIFI      4
#define TASK_PRIORITY_OTA       3
#define TASK_PRIORITY_MONITOR   1

/* =====================================================================
 * Watchdog
 * ===================================================================== */
#define WDT_TIMEOUT_S           10

/* =====================================================================
 * Aufnahme / Stream
 * ===================================================================== */
#define STREAM_PORT             80          /* Haupt-Webserver: Web-UI + Stream + OTA + Portal */
#define SNAPSHOT_INTERVAL_MS    1000        /* Snapshot fuer Status */

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
