/*
 * config.h - Hardware-Konfiguration fuer "s3lcd" (LCDWIKI ES3C40P)
 *
 * Board: LCDWIKI ES3C40P (AliExpress: Aideepen) - "modernes CYD" mit:
 *  - ESP32-S3 (rev 0.2), 16MB Flash (25VQ128D), 8MB PSRAM (embedded)
 *  - 4.0" IPS 320x480, ST7796S (SPI), kapazitiv
 *  - FT6336U Touch (I2C, Adr 0x38), Lautsprecher (I2S), Mikro, LiPo, MicroSD,
 *    RGB-LED, BOOT-Taste, USB-C (native USB-Serial/JTAG)
 *
 * Pinbelegung VERIFIZIERT aus dem offiziellen LCDWIKI-Manual
 * (ESP-IDF-Beispiel): CS=10, DC/RS=46, SCK=12, MOSI=11, MISO=13,
 * RST=CHIP_PU (am ESP-Reset!), BL=45. Touch: SDA=16, SCL=15, RST=18, INT=17.
 *
 * Rolle: Display-Client fuer den Kamerastream vom ESP32-CAM
 * (Projekt ../esp32cam, SoftAP "Cam-AP" 10.1.1.1).
 * Basis: tft/cyd, angepasst auf ESP32-S3 + ES3C40P.
 *
 * STATUS: Bring-up (Display + Touch). Display-Init (ST7796S) und
 * Touch-Skalierung sind auf Hardware zu verifizieren!
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_NAME          "LCDWIKI ES3C40P"
#define BOARD_CHIP          "ESP32-S3"
#define APP_VERSION_MAJOR   0
#define APP_VERSION_MINOR   1

/* =====================================================================
 * Testmodus: 1 = Standalone-Bring-up (Display-Farbtest + Touch-Rohwerte,
 * ohne Kamera/WiFi). 0 = normaler Display-Client-Betrieb (Stream), sobald
 * der Client portiert ist.
 * ===================================================================== */
#define S3LCD_TEST_MODE     1

/* =====================================================================
 * Display ST7796S (320x480) - VERIFIZIERTE Pinbelegung (LCDWIKI-Manual).
 * SCK=12/MOSI=11/MISO=13/CS=10 liegen auf den FSPI-IOMUX-Pins des ESP32-S3
 * (volle Geschwindigkeit moeglich). RST haengt an CHIP_PU (kein Pin).
 * ===================================================================== */
#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SCK             12
#define TFT_MOSI            11
#define TFT_MISO            13
#define TFT_CS              10
#define TFT_DC              46
#define TFT_RST             -1      /* kein eigener Reset (CHIP_PU/ESP-Reset) */
#define TFT_BL              45      /* Backlight (active HIGH) */
#define TFT_BL_ON           1

/* Anzeige im Querformat (Kamerabild ist quer): 480 breit x 320 hoch.
 * ST7796S-Nativ ist 320x480 (Portrait); per MADCTL gedreht.
 * MADCTL 0x68 = MV|MX|BGR -> Querformat 480x320 (auf HW zu verifizieren!). */
#define TFT_WIDTH           480
#define TFT_HEIGHT          320
#define ST7796S_MADCTL      0x68

/* SPI-Takt: 40 MHz (alle Pins IOMUX; bei Problemen reduzieren) */
#define TFT_SPI_CLK_HZ      (40 * 1000 * 1000)
#define TFT_SPI_MAX_TRANS   32768

/* =====================================================================
 * Touch FT6336U (kapazitiv, I2C, Adresse 0x38)
 * VERIFIZIERTE Pins (LCDWIKI-Manual): SDA=16, SCL=15, RST=18, INT=17.
 * ===================================================================== */
#define TFT_HAVE_TOUCH      1
#define TOUCH_I2C_PORT      I2C_NUM_0
#define TOUCH_SDA           16
#define TOUCH_SCL           15
#define TOUCH_RST           18
#define TOUCH_INT           17
#define TOUCH_I2C_ADDR      0x38
#define TOUCH_I2C_CLK_HZ    400000

/* Roh-Koordinatenbereich des FT6336U -> Panel skalieren.
 * FT6336U liefert i.d.R. Werte bis ~1023 (12 Bit) bzw. bis zur im IC
 * hinterlegten Maximalgrenze. AUF HW KALIBRIEREN (Touch-Test ausgeben). */
#define TOUCH_RAW_MAX_X     1024
#define TOUCH_RAW_MAX_Y     1024
#define TOUCH_SWAP_XY       0       /* ggf. tauschen (Orientierung) */

/* =====================================================================
 * WiFi (Station) - verbindet sich mit dem ESP32-CAM SoftAP
 * (fuer den spaeteren Display-Client-Betrieb)
 * ===================================================================== */
#define WIFI_SSID_DEFAULT   "Cam-AP"    /* SoftAP des ESP32-CAM */
#define WIFI_PASS_DEFAULT   ""          /* Cam-AP ist offen (WIFI_AUTH_OPEN) */
#define WIFI_CONNECT_TIMEOUT_S 15
#define S3_STATIC_IP        "10.1.1.3"  /* dritte feste Adresse am Cam-AP */
#define S3_GATEWAY          "10.1.1.1"
#define S3_NETMASK          "255.255.255.0"

#define CAM_HOST_DEFAULT    "10.1.1.1"  /* SoftAP-IP des ESP32-CAM */
#define CAM_PORT_DEFAULT    80
#define CAM_CAPTURE_PATH    "/capture"
#define CAM_API_PATH        "/api/config"

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
