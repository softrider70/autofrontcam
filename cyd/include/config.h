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
 * Display ILI9341 (240x320) - VERIFIZIERTE CYD-Pinbelegung
 * (aus cyd-display-car1 uebernommen: CLK=14, MOSI=13, CS=15, DC=2,
 *  RST=-1 [haengt am ESP32-Reset], BL=21)
 * ===================================================================== */
#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SCK             14
#define TFT_MOSI            13
#define TFT_MISO            12
#define TFT_CS              15
#define TFT_DC              2
#define TFT_RST             -1      /* kein eigener Reset-Pin (am ESP32-RST) */
#define TFT_BL              21      /* Backlight (active HIGH) */
#define TFT_BL_ON           1

#define TFT_WIDTH           240
#define TFT_HEIGHT          320

/* MADCTL 0x40 (MX=1, RGB) - korrigierte Farben/Ausrichtung laut Referenz */
#define ILI9341_MADCTL      0x40

/* =====================================================================
 * Touch FT6236 kapazitiv (I2C) - CYD (nicht XPT2046!)
 * ===================================================================== */
#define TOUCH_I2C_PORT      I2C_NUM_0
#define TOUCH_SDA           6
#define TOUCH_SCL           5
#define TOUCH_ADDR          0x38

/* =====================================================================
 * WiFi (Station) - verbindet sich mit dem ESP32-CAM SoftAP
 * ===================================================================== */
#define WIFI_SSID_DEFAULT   "Cam-AP"    /* SoftAP des ESP32-CAM */
#define WIFI_PASS_DEFAULT   ""          /* Cam-AP ist offen (WIFI_AUTH_OPEN) */
#define WIFI_CONNECT_TIMEOUT_S 15

/* =====================================================================
 * Statische IP fuer den CYD (feste Adresse am Cam-AP, kein DHCP)
 * Sender (CAM) ist immer 10.1.1.1, Empfaenger (CYD) immer 10.1.1.2.
 * ===================================================================== */
#define CYD_STATIC_IP       "10.1.1.2"
#define CYD_GATEWAY         "10.1.1.1"
#define CYD_NETMASK         "255.255.255.0"

/* Farbinversion (1 = INVON 0x21 senden) - fuer manche CYD-Panels noetig
 * (ST7789-Variante zeigt sonst invertierte Farben = schwarz als weiss). */
#define CYD_INVERT_COLOR    0

/* =====================================================================
 * Kamera-Stream (HTTP GET /capture auf dem ESP32-CAM)
 * ===================================================================== */
#define CAM_HOST_DEFAULT    "10.1.1.1"  /* SoftAP-IP des ESP32-CAM */
#define CAM_PORT_DEFAULT    80
#define CAM_CAPTURE_PATH    "/capture"
#define CAM_API_PATH        "/api/config"

#define STREAM_POLL_MS      50          /* ~20 fps Polling */
#define STREAM_FETCH_TIMEOUT_MS 2000

/* JPEG-Dekodierung: wird adaptiv an die tatsaechliche Kameragroesse angepasst
 * (kein PSRAM). Maximaler Dekodier-Puffer: SVGA 800x600 bei 1:4 = 200x150x2 = 60000 B. */
#define JPEG_DECODE_SCALE   JPEG_IMAGE_SCALE_1_4
#define DECODED_W           160
#define DECODED_H           120
#define MAX_DECODED_BUF     60000       /* Obergrenze fuer Dekodier-Puffer */
#define JPEG_BUF_SIZE       40960       /* Puffer fuer ein JPEG (SVGA q16 ~32KB) */

/* Anzeige-Drehung (fuer die 90°-Auffuellung des Porträt-Displays):
 * 1 = im Uhrzeigersinn, 2 = gegen den Uhrzeigersinn */
#define DISPLAY_ROTATION    1

/* =====================================================================
 * UI-Layout: OSD-Bereich oben, Button-Leiste unten. Der Videobereich
 * liegt dazwischen und wird vom Bild nicht uebermalt -> kein Flackern. */
#define UI_OSD_H        20      /* Hoehe OSD oben (Version/fps/Status) */
#define UI_BTN_H        38      /* Hoehe Button-Leiste unten */

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
