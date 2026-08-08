/*
 * config.h - Hardware-Konfiguration fuer CYD (Cheap Yellow Display)
 *
 * Board: ESP32-2432S028R (klassisch)
 * Chip: ESP32-WROOM-32 (ESP32-D0WD-V3, Dual-Core, 240MHz)
 * Display: ILI9341 240x320 SPI (SPI2_HOST)
 * Touch: XPT2046 resistiv (gleicher SPI-Bus, eigener CS)
 * Flash: 4MB, KEIN PSRAM
 *
 * Rolle: Display-Client - zeigt den Kamerastream vom ESP32-CAM
 * (Projekt ../esp32cam, SoftAP "Cam-AP" 10.1.1.1) auf dem Display an.
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_NAME          "CYD ESP32-2432S028R"
#define BOARD_CHIP          "ESP32-D0WD-V3"
#define APP_VERSION_MAJOR   0
#define APP_VERSION_MINOR   1

/* =====================================================================
 * Display ILI9341 (240x320) - CYD Pinbelegung
 * ===================================================================== */
#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SCK             18
#define TFT_MOSI            23
#define TFT_MISO            19      /* wird vom Touch benoetigt */
#define TFT_CS              5
#define TFT_DC              2
#define TFT_RST             4
#define TFT_BL              21      /* Backlight */
#define TFT_BL_ON           1       /* active HIGH auf dem CYD */

#define TFT_WIDTH           240
#define TFT_HEIGHT          320

/* =====================================================================
 * Touch XPT2046 - CYD Pinbelegung (gleicher SPI-Bus wie Display)
 * ===================================================================== */
#define TOUCH_CS            14
#define TOUCH_IRQ           27
#define TOUCH_MIN_X         300     /* Rohwert-Kalibrierung (min/max) */
#define TOUCH_MAX_X         3800
#define TOUCH_MIN_Y         300
#define TOUCH_MAX_Y         3800

/* =====================================================================
 * WiFi (Station) - verbindet sich mit dem ESP32-CAM SoftAP
 * ===================================================================== */
#define WIFI_SSID_DEFAULT   "Cam-AP"    /* SoftAP des ESP32-CAM */
#define WIFI_PASS_DEFAULT   ""          /* Cam-AP ist offen (WIFI_AUTH_OPEN) */
#define WIFI_CONNECT_TIMEOUT_S 15

/* =====================================================================
 * Kamera-Stream (HTTP GET /capture auf dem ESP32-CAM)
 * ===================================================================== */
#define CAM_HOST_DEFAULT    "10.1.1.1"  /* SoftAP-IP des ESP32-CAM */
#define CAM_PORT_DEFAULT    80
#define CAM_CAPTURE_PATH    "/capture"
#define CAM_API_PATH        "/api/config"

#define STREAM_POLL_MS      120         /* ~8 fps Polling */
#define STREAM_FETCH_TIMEOUT_MS 2000

/* JPEG-Dekodierung: VGA 640x480 -> Skalierung 1:4 -> 160x120 (38 KB, kein PSRAM)
 * Hinweis: DECODED_W/H muessen zur Kameraeinstellung des esp32cam passen
 * (CAM_FRAME_SIZE = FRAMESIZE_VGA). */
#define JPEG_DECODE_SCALE   JPEG_IMAGE_SCALE_1_4
#define DECODED_W           160
#define DECODED_H           120
#define JPEG_BUF_SIZE       32768       /* Puffer fuer ein JPEG (VGA q16 ~7KB) */

/* Anzeige-Drehung (fuer die 90°-Auffuellung des Porträt-Displays):
 * 1 = im Uhrzeigersinn, 2 = gegen den Uhrzeigersinn */
#define DISPLAY_ROTATION    1

/* =====================================================================
 * FreeRTOS Task-Konfiguration
 * ===================================================================== */
#define TASK_STACK_STREAM   4096
#define TASK_STACK_UI       3072
#define TASK_PRIORITY_STREAM 5
#define TASK_PRIORITY_UI    4

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
