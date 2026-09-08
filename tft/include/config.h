/*
 * config.h - Hardware-Konfiguration fuer "tft" (ESP32 + 2.8" SPI-Display)
 *
 * Board: generisches ESP32-Dev-Board (ESP32-WROOM-32, Dual-Core, 240MHz)
 * Chip: ESP32-WROOM-32 (ESP32-D0WD-V3)
 * Display: 2.8" ILI9341 240x320 SPI (SPI2_HOST) - Pinbelegung = DEFAULT,
 *          BITTE AN DEINE VERKABELUNG ANPASSEN!
 * Touch: XPT2046 resistiv (optional) - default AUS (TFT_HAVE_TOUCH 0)
 * Flash: 4MB, KEIN PSRAM
 *
 * Rolle: Display-Client - zeigt den Kamerastream vom ESP32-CAM
 * (Projekt ../esp32cam, SoftAP "Cam-AP" 10.1.1.1) auf dem Display an.
 * Basis: cyd/ (Cheap Yellow Display) - abgeleitet fuer dieses Board
 * im Git-Branch "tft".
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_NAME          "ESP32 + 2.8\" TFT"
#define BOARD_CHIP          "ESP32-WROOM-32"
#define APP_VERSION_MAJOR   0
#define APP_VERSION_MINOR   1

/* =====================================================================
 * Testmodus: 1 = Standalone-Test (Display + Touch, ohne Kamera/WiFi) -
 * sinnvoll, solange das Sendermodul (ESP32-CAM) nicht verfuegbar ist.
 * 0 = normaler Display-Client-Betrieb (Stream vom ESP32-CAM).
 * ===================================================================== */
#define TFT_TEST_MODE       1

/* =====================================================================
 * Display ILI9341 (2.8", 240x320) - VERIFIZIERTE Pinbelegung (Nutzer):
 * CS=15, RST=12, DC=2, MOSI(SDI)=13, SCK=14, BL(LED)=21, MISO=16.
 * TFT_RST >= 0 wird vom Treiber aktiv getoggelt (low->high beim Init).
 * Hinweis: SCK=14/MOSI=13 sind VSPI-IOMUX, MISO=16 NICHT -> Full-Duplex
 * limitiert auf 26,7 MHz (Treiber nutzt daher 20 MHz).
 * ===================================================================== */
#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SCK             14
#define TFT_MOSI            13
#define TFT_MISO            16
#define TFT_CS              15
#define TFT_DC              2
#define TFT_RST             12      /* >=0: Treiber toggelt Reset beim Init
                                       (Achtung: GPIO12 = Strapping-Pin MTDI) */
#define TFT_BL              21      /* Backlight (active HIGH) */
#define TFT_BL_ON           1

/* Native ILI9341-Aufloesung ist 240x320 (Portrait). Falls dein Panel ein
 * ST7796 (320x240 native) ist, auf TFT_WIDTH=320/TFT_HEIGHT=240 + MADCTL
 * 0x40 umstellen (siehe cyd/). */
#define TFT_WIDTH           240
#define TFT_HEIGHT          320

/* MADCTL: 0x08 = Standard-ILI9341-Portrait mit BGR-Farbordnung.
 * Andere Werte: 0x00 (RGB), 0x40 (MX), 0x60 (MV, Landscape). */
#define ILI9341_MADCTL      0x08

/* =====================================================================
 * Touch XPT2046 resistiv - VERIFIZIERTE Pinbelegung (Nutzer):
 * IRQ=VP(36), D0/MISO=VN(39), DIN/MOSI=33, CS=25, CLK=26.
 * Eigener SPI3-Bus (wie cyd/touch.c), Touch ist AKTIV (TFT_HAVE_TOUCH 1).
 * ===================================================================== */
#define TFT_HAVE_TOUCH      1       /* Touch vorhanden (XPT2046, eigener SPI3-Bus) */
#define TOUCH_SPI_HOST      SPI3_HOST
#define TOUCH_SCLK          26
#define TOUCH_MOSI          33
#define TOUCH_MISO          39      /* GPIO39 = VN (Input-Pin) */
#define TOUCH_CS            25
#define TOUCH_IRQ           36      /* GPIO36 = VP (Input-Pin) */

/* =====================================================================
 * WiFi (Station) - verbindet sich mit dem ESP32-CAM SoftAP
 * ===================================================================== */
#define WIFI_SSID_DEFAULT   "Cam-AP"    /* SoftAP des ESP32-CAM */
#define WIFI_PASS_DEFAULT   ""          /* Cam-AP ist offen (WIFI_AUTH_OPEN) */
#define WIFI_CONNECT_TIMEOUT_S 15

/* =====================================================================
 * Statische IP (feste Adresse am Cam-AP, kein DHCP)
 * Sender (CAM) ist immer 10.1.1.1, Empfaenger immer 10.1.1.2.
 * ACHTUNG: cyd/ und tft/ teilen sich diese IP - nur EIN Geraet gleichzeitig
 * am Cam-AP betreiben (oder die IP hier anpassen).
 * ===================================================================== */
#define TFT_STATIC_IP       "10.1.1.2"
#define TFT_GATEWAY         "10.1.1.1"
#define TFT_NETMASK         "255.255.255.0"

/* Farbinversion (1 = INVON 0x21 senden) - fuer manche Panels noetig
 * (ST7789-Variante zeigt sonst invertierte Farben = schwarz als weiss). */
#define TFT_INVERT_COLOR    0

/* =====================================================================
 * Kamera-Stream (HTTP GET /capture auf dem ESP32-CAM)
 * ===================================================================== */
#define CAM_HOST_DEFAULT    "10.1.1.1"  /* SoftAP-IP des ESP32-CAM */
#define CAM_PORT_DEFAULT    80
#define CAM_CAPTURE_PATH    "/capture"
#define CAM_API_PATH        "/api/config"

/* Roh-JPEG-Stream der CAM (Port 8080): persistente TCP-Verbindung, pro Frame
 * 4-Byte-Laenge (big-endian) + JPEG-Daten. Kein HTTP-Handshake pro Frame
 * (das war der fps-Engpass beim Einzelbild-Fetch). */
#define STREAM_TCP_PORT     8080

#define STREAM_POLL_MS      5           /* 5 ms zwischen Frames (Stream): kein kuenstliches
                                           Warten mehr - 50 ms kosteten ~20% der Frame-Zeit.
                                           Ohne Verbindung nutzt stream.c 50 ms (Leerlauf). */
#define STREAM_FETCH_TIMEOUT_MS 1500    /* 1.5s: groesserer Puffer fuer groessere JPEGs (q12 ~10KB).
                                            1s liess bei q8-JPEGs haeufig EAGAIN ausloesen (fps-Einbruch). */
#define STREAM_FAIL_THRESHOLD 3         /* Fetch-Fehler bis HTTP-Client-Neuaufbau +
                                           hartes WiFi-Reconnect (Verbindungs-Watchdog) */

/* JPEG-Dekodierung: wird adaptiv an die tatsaechliche Kameragroesse angepasst
 * (kein PSRAM). Maximaler Dekodier-Puffer: SVGA 800x600 bei 1:4 = 200x150x2 = 60000 B. */
#define JPEG_DECODE_SCALE   JPEG_IMAGE_SCALE_1_4
#define DECODED_W           160
#define DECODED_H           120
#define MAX_DECODED_BUF     60000       /* Obergrenze fuer Dekodier-Puffer */
#define JPEG_BUF_SIZE       40960       /* Puffer fuer ein JPEG (SVGA q16 ~32KB) */

/* Anzeige-Drehung: 0 = keine (Kamerabild ist quer, Display ist quer 320x240).
 * 1 = im Uhrzeigersinn, 2 = gegen den Uhrzeigersinn (nur falls noetig). */
#define DISPLAY_ROTATION    0

/* =====================================================================
 * UI-Layout: Schmale OSD-Leiste oben, KEINE dauerhafte Button-Leiste unten.
 * Das Touch-Menue wird nur temporaer (per Tippen) ueber das Video gelegt,
 * dadurch bleibt das Video fast vollflaechig. */
#define UI_OSD_H        20      /* Hoehe OSD oben (Version/fps/Status) */
#define UI_BTN_H        0       /* kein fester Button-Bereich (Menue temporaer) */

/* Video-Anzeige: volle Flaeche (100%). Eine Verkleinerung (55%) hat die
 * farbigen Linien NICHT reduziert - die Ursache war nicht Tearing, sondern
 * unfertige JPEG-Frames im Stream (per SOI/EOI-Validierung gefiltert). */
#define VIDEO_SCALE_PCT     100

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
