/*
 * display.c - tft-Display (2.8" ILI9341, 240x320), eigener Treiber
 *
 * Abgeleitet vom verifizierten cyd-Treiber (autofrontcam/cyd). Pins werden
 * aus config.h gelesen (DEFAULT-Annahme fuer ESP32-Dev-Board + 2.8"-Modul,
 * bitte an die eigene Verkabelung anpassen). Vollstaendiges ILI9341-Init;
 * TFT_RST >= 0 wird beim Init aktiv getoggelt (low->high).
 * ESP-IDF 6.x hat keinen ILI9341-Treiber mehr, daher Ansteuerung ueber
 * esp_lcd_panel_io_spi (Kommandos: tx_param, Pixeldaten: tx_color mit
 * RAMWR 0x2C). Farbdaten RGB565 BIG-ENDIAN; JPEG-Dekodierung liefert mit
 * swap_color_bytes=1 ebenfalls Big-Endian.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_system.h"
#include "config.h"
#include "display.h"

static const char *TAG = "display";

static spi_device_handle_t s_spi = NULL;
static SemaphoreHandle_t s_lcd_mutex = NULL;   /* schuetzt SPI-Zugriffe (UI- vs Stream-Task) */
static int s_rotation = DISPLAY_ROTATION;

/* RGB565 -> Big-Endian-Speicherformat fuer das Display */
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ------------------------------------------------------------------ */
/* ILI9341-Kommandos (roher SPI-Treiber wie im Referenzprojekt)       */
/* ------------------------------------------------------------------ */
static void lcd_write(bool is_cmd, const void *data, size_t len)
{
    if (len == 0) return;
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    gpio_set_level(TFT_DC, is_cmd ? 0 : 1);
    spi_transaction_t t = {
        .length = (int)(len * 8),
        .tx_buffer = data,
    };
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit (%s, %d B) fehlgeschlagen: %s",
                 is_cmd ? "cmd" : "data", (int)len, esp_err_to_name(err));
    }
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_write(true, &cmd, 1);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_write(true, &cmd, 1);
    if (len) lcd_write(false, data, len);
}

/* Adressfenster setzen (CASET/PASET/RAMWR) */
static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t d[4];
    d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)x0;
    d[2] = (uint8_t)(x1 >> 8); d[3] = (uint8_t)x1;
    lcd_cmd(0x2A);                 /* CASET */
    lcd_write(false, d, 4);
    d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)y0;
    d[2] = (uint8_t)(y1 >> 8); d[3] = (uint8_t)y1;
    lcd_cmd(0x2B);                 /* PASET */
    lcd_write(false, d, 4);
    lcd_cmd(0x2C);                 /* RAMWR */
}

/* Pixeldaten schreiben (DC=1) */
static void lcd_draw_bitmap(const void *data, size_t len)
{
    lcd_write(false, data, len);
}

/* Schnelle Rechteck-Fuellung: Fenster einmal setzen, dann alle Zeilen in
 * grossen Bloecken senden (statt einer Transaktion pro Zeile). Die schwarzen
 * contain-fit-Balken im Blit kosteten mit display_draw_filled_rect (1 Zeile
 * pro Transfer) ~100 ms/Frame - hier sind es nur noch wenige grosse Transfers. */
static void lcd_fill_rect_fast(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0 || !s_spi) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    const int rows = 16;
    size_t npx = (size_t)w * rows;
    uint16_t *buf = heap_caps_malloc(npx * 2, MALLOC_CAP_DMA);
    if (!buf) return;   /* selten: Balken bleibt schwarz vom Vor-Frame */
    uint16_t c = be16(color);
    for (size_t i = 0; i < npx; i++) buf[i] = c;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    while (h > 0) {
        int n = (h < rows) ? h : rows;
        lcd_draw_bitmap(buf, (size_t)w * n * 2);
        h -= n;
    }
    heap_caps_free(buf);
}

/* Synchrones SPI: keine ausstehenden Transfers -> kein Flush noetig */
static void lcd_flush(void) { }

static void ili9341_init_panel(void)
{
    /* Wenn TFT_RST >= 0 wurde der Reset-Pin in display_init() bereits aktiv
     * getoggelt (low->high). Kurz warten, dann SWRESET + Sleep-Out. */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x01);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Vollstaendige ILI9341-Init-Sequenz (verifiziert in cyd-display-car1) */
    static const uint8_t init_seq[] = {
        0xEF, 3, 0x03, 0x80, 0x02,
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x23,
        0xC1, 1, 0x11,
        0xC5, 2, 0x27, 0x2B,
        0xC7, 1, 0x1E,
        0x36, 1, ILI9341_MADCTL,
        0x3A, 1, 0x55,
        0xB1, 2, 0x00, 0x1B,
        0xB6, 3, 0x08, 0x82, 0x27,
        0xF2, 1, 0x00,
        0x26, 1, 0x01,
        0xE0, 15, 0x0F,0x31,0x2B,0x0C,0x0E,0x06,0x38,0x0F,0x44,0x49,0x09,0x06,0x15,0x12,0x0C,
        0xE1, 15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x37,0x06,0x09,0x0A,0x13,0x13,
        0x11, 0,
        0x29, 0,
    };
    size_t i = 0;
    while (i < sizeof(init_seq)) {
        uint8_t cmd = init_seq[i++];
        uint8_t cnt = init_seq[i++];
        lcd_cmd_data(cmd, &init_seq[i], cnt);
        i += cnt;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
/* ------------------------------------------------------------------ */
/* Oeffentliche API                                                    */
/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    /* Mutex fuer die SPI-Zugriffe: UI-Task (Menue/OSD) und Stream-Task (Video)
     * greifen parallel auf das Display zu -> ohne Mutex SPI-Assert. */
    s_lcd_mutex = xSemaphoreCreateMutex();

    /* BL, DC und (falls TFT_RST >= 0) RST als GPIO konfigurieren
     * (CS uebernimmt der SPI-Treiber) */
    uint64_t pin_mask = (1ULL << TFT_BL) | (1ULL << TFT_DC);
    if (TFT_RST >= 0) pin_mask |= (1ULL << TFT_RST);
    gpio_config_t io = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO-Konfiguration fehlgeschlagen");
    gpio_set_level(TFT_BL, !TFT_BL_ON);
    gpio_set_level(TFT_DC, 1);
    if (TFT_RST >= 0) {
        gpio_set_level(TFT_RST, 0);          /* Reset low */
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(TFT_RST, 1);          /* Reset high -> Panel startet */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* SPI-Bus (roher SPI-Treiber wie im verifizierten Referenzprojekt) */
    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SCK,
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 2 * 16,   /* gross genug fuer 16 Bildzeilen pro
                                                    SPI-Transfer (Blit-Optimierung) */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI-Bus init fehlgeschlagen");

    spi_device_interface_config_t dev_cfg = {
        /* 20 MHz statt 40 MHz: Full-Duplex-SPI > 26,7 MHz ist nur mit IOMUX-Pins
         * erlaubt (CYD-Pins 13/14/12). Die tft-Default-Pins (18/23/19) laufen ueber
         * die GPIO-Matrix -> 40 MHz ergab ESP_ERR_NOT_SUPPORTED (Abort beim Boot).
         * 20 MHz ist fuer jede Verdrahtung sicher; nur mit IOMUX-Pins auf 26/40 MHz
         * erhoehen. */
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS,
        .queue_size = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TFT_SPI_HOST, &dev_cfg, &s_spi), TAG, "SPI-Geraet fehlgeschlagen");

    ili9341_init_panel();
    ESP_LOGI(TAG, "ILI9341 initialisiert (%dx%d), freier Heap: %lu B",
             TFT_WIDTH, TFT_HEIGHT, (unsigned long)esp_get_free_heap_size());
    display_fill(0x0000);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(TFT_BL, on ? TFT_BL_ON : !TFT_BL_ON);
}

void display_fill(uint16_t color)
{
    if (!s_spi) return;
    uint16_t *row = heap_caps_malloc(TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "display_fill: kein DMA-Puffer (Heap %lu)",
                 (unsigned long)esp_get_free_heap_size());
        return;
    }
    uint16_t c = be16(color);
    for (int x = 0; x < TFT_WIDTH; x++) row[x] = c;

    lcd_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    for (int y = 0; y < TFT_HEIGHT; y++) {
        lcd_draw_bitmap(row, TFT_WIDTH * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_blit_decoded(const uint16_t *src, int src_w, int src_h)
{
    if (!s_spi || !src) return;
    if (src_w <= 0 || src_h <= 0) return;

    /* Videobereich VERKLEINERT und zentriert (gegen Tearing): Der Blit muss
     * kuerzer als eine Scanout-Periode (~16ms) sein, sonst entstehen farbige
     * horizontale Linien (Tearing). VIDEO_SCALE_PCT begrenzt die Flaeche. */
    const int vw_full = TFT_WIDTH;
    const int vh_full = TFT_HEIGHT - UI_OSD_H - UI_BTN_H;
    const int vw  = (vw_full * VIDEO_SCALE_PCT) / 100;
    const int vh  = (vh_full * VIDEO_SCALE_PCT) / 100;
    const int vx0 = (vw_full - vw) / 2;
    const int vy0 = UI_OSD_H + (vh_full - vh) / 2;
    if (vw <= 0 || vh <= 0) return;

    /* Bilddimensionen nach gewaehlter Drehung */
    int iw, ih;
    if (s_rotation == 0) { iw = src_w; ih = src_h; }
    else                 { iw = src_h; ih = src_w; }

    /* contain-fit: groesster Faktor, der in den Videobereich passt
     * (Integer * 1000), dann zentrieren. Kein Verzerren, kein Müll. */
    int s_x = (vw * 1000) / iw;
    int s_y = (vh * 1000) / ih;
    int scale = (s_x < s_y) ? s_x : s_y;
    if (scale <= 0) return;
    int dw = (iw * scale) / 1000;
    int dh = (ih * scale) / 1000;
    if (dw <= 0 || dh <= 0) return;
    int ox = vx0 + (vw - dw) / 2;
    int oy = vy0 + (vh - dh) / 2;

    /* Nicht vom Bild bedeckte Videobereich-Flaechen schwarz fuellen
     * (Balken oben/unten bzw. links/rechts), kein altes GRAM. Mit der
     * schnellen Fuellung (grosse SPI-Transfers statt 1 Zeile pro Transfer). */
    if (oy > vy0) {
        lcd_fill_rect_fast(vx0, vy0, vw, oy - vy0, 0x0000);
        lcd_fill_rect_fast(vx0, oy + dh, vw, (vy0 + vh) - (oy + dh), 0x0000);
    }
    if (ox > vx0) {
        lcd_fill_rect_fast(vx0, oy, ox - vx0, dh, 0x0000);
        lcd_fill_rect_fast(ox + dw, oy, (vx0 + vw) - (ox + dw), dh, 0x0000);
    }

    /* GROSSE SPI-TRANSFERS (originale 9fps-Variante): 16 Zeilen pro Transfer,
     * EINMAL das Adressfenster fuer das GESAMTE Bild setzen - der Controller
     * inkrementiert die Zielzeile nach den Daten automatisch. Diese Variante
     * zeigte im 9fps-Betrieb keine Stoerlinien. (Ein Adressfenster pro Block
     * brachte keinen Vorteil und konnte sporadisch farbige Linien erzeugen.) */
    const int xfer_rows = 16;
    uint16_t *rowbuf = heap_caps_malloc((size_t)dw * xfer_rows * 2, MALLOC_CAP_DMA);
    if (!rowbuf) {
        ESP_LOGE(TAG, "display_blit_decoded: kein DMA-Puffer");
        return;
    }

    /* Skalierung OHNE Divisionen pro Pixel (Xtensa-Divisionen sind teuer und
     * kosteten ~30-60 ms/Frame): Spalten-Mapping einmal vorab als Tabelle,
     * Zeilen-Mapping als Festkomma-Akkumulator (1<<12 = 4096 Schritte). */
    static uint16_t col_map[TFT_WIDTH];
    uint32_t rx_step = ((uint32_t)iw << 12) / (uint32_t)dw;
    uint32_t rx_f = 0;
    for (int dx = 0; dx < dw; dx++) {
        col_map[dx] = (uint16_t)(rx_f >> 12);
        rx_f += rx_step;
    }
    uint32_t ry_step = ((uint32_t)ih << 12) / (uint32_t)dh;
    uint32_t ry_f = 0;

    /* EINMAL das Adressfenster fuer das GESAMTE Bild */
    lcd_set_window(ox, oy, ox + dw - 1, oy + dh - 1);

    int dy = 0;
    while (dy < dh) {
        int nrows = (dh - dy < xfer_rows) ? (dh - dy) : xfer_rows;
        uint16_t *p = rowbuf;
        for (int k = 0; k < nrows; k++, dy++) {
            int ry = (int)(ry_f >> 12);
            ry_f += ry_step;
            if (ry >= ih) ry = ih - 1;
            if (s_rotation == 0) {            /* ohne Rotation: haeufigster Fall */
                const uint16_t *srow = &src[ry * src_w];
                for (int dx = 0; dx < dw; dx++) *p++ = srow[col_map[dx]];
            } else if (s_rotation == 1) {     /* CW */
                int sx = src_w - 1 - ry;
                for (int dx = 0; dx < dw; dx++) *p++ = src[col_map[dx] * src_w + sx];
            } else {                          /* CCW */
                for (int dx = 0; dx < dw; dx++) *p++ = src[(src_h - 1 - col_map[dx]) * src_w + ry];
            }
        }
        lcd_draw_bitmap(rowbuf, (size_t)dw * nrows * 2);
    }
    lcd_flush();
    heap_caps_free(rowbuf);
}

void display_set_rotation(int rotation)
{
    if (rotation < 0 || rotation > 2) rotation = 1;
    s_rotation = rotation;
}

int display_get_rotation(void)
{
    return s_rotation;
}

/* ------------------------------------------------------------------ */
/* 5x7-Font (Public Domain)                                            */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
    {0x00,0x06,0x09,0x09,0x06}, /* DEL */
};

void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg)
{
    if (!s_spi || !text) return;

    int len = (int)strlen(text);
    int total_w = len * 6;   /* 5px Glyphe + 1px Abstand */
    if (total_w <= 0) return;

    uint16_t *row = heap_caps_malloc((size_t)total_w * 2, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "display_draw_text: kein DMA-Puffer");
        return;
    }

    uint16_t col_be = be16(color);
    uint16_t bg_be  = be16(bg);

    for (int ry = 0; ry < 7; ry++) {
        for (int i = 0; i < len; i++) {
            /* Font ist spaltenorientiert: 5 Bytes = 5 Spalten, Bit ry = Zeile ry.
             * Nicht-ASCII-Zeichen defensiv auf '?' abbilden. */
            unsigned char ch = (unsigned char)text[i];
            if (ch < 32 || ch > 126) ch = '?';
            for (int c = 0; c < 5; c++) {
                uint8_t col = font5x7[ch - 32][c];
                row[i * 6 + c] = ((col >> ry) & 1) ? col_be : bg_be;
            }
            row[i * 6 + 5] = bg_be;
        }
        int yy = y + ry;
        if (yy < 0 || yy >= TFT_HEIGHT) continue;
        int xx0 = (x < 0) ? 0 : x;
        int ww = total_w;
        if (xx0 + ww > TFT_WIDTH) ww = TFT_WIDTH - xx0;
        if (ww <= 0) continue;
        lcd_set_window(xx0, yy, xx0 + ww - 1, yy);
        lcd_draw_bitmap(row + (xx0 - x), (size_t)ww * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_spi) return;
    lcd_fill_rect_fast(x, y, w, h, color);
    lcd_flush();
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_draw_filled_rect(x, y, w, 1, color);
    display_draw_filled_rect(x, y + h - 1, w, 1, color);
    display_draw_filled_rect(x, y, 1, h, color);
    display_draw_filled_rect(x + w - 1, y, 1, h, color);
}

/* Linie zeichnen mit Breite, als gefuelltes Parallelogramm (Scanline-Fill):
 * pro Bildzeile EIN Adressfenster + EIN Transfer statt pro Pixel. Das alte
 * Pixel-fuer-Pixel-Verfahren kostete ~17000 SPI-Transfers pro Frame (2 Linien
 * Breite 6) und drueckte die fps von 9 auf 3. Farbwert RGB565.
 * Fuer die Kalibrierungslinien (rot/gelb). */
void display_draw_line(int x0, int y0, int x1, int y1, int width, uint16_t color)
{
    if (!s_spi) return;
    if (width < 1) width = 1;

    float dxv = (float)(x1 - x0), dyv = (float)(y1 - y0);
    float len = sqrtf(dxv * dxv + dyv * dyv);
    if (len < 0.5f) return;
    float nx = -dyv / len;   /* senkrecht zur Linienrichtung */
    float ny =  dxv / len;
    float half = (float)width / 2.0f;

    /* 4 Ecken des dicken Linien-Rechtecks */
    float px[4], py[4];
    px[0] = x0 + nx * half;  py[0] = y0 + ny * half;
    px[1] = x1 + nx * half;  py[1] = y1 + ny * half;
    px[2] = x1 - nx * half;  py[2] = y1 - ny * half;
    px[3] = x0 - nx * half;  py[3] = y0 - ny * half;

    int ymin = (int)floorf(fminf(fminf(py[0], py[1]), fminf(py[2], py[3])));
    int ymax = (int)ceilf(fmaxf(fmaxf(py[0], py[1]), fmaxf(py[2], py[3])));
    if (ymin < 0) ymin = 0;
    if (ymax > TFT_HEIGHT - 1) ymax = TFT_HEIGHT - 1;
    if (ymin > ymax) return;

    uint16_t *row = heap_caps_malloc((size_t)TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) return;
    uint16_t c = be16(color);

    for (int y = ymin; y <= ymax; y++) {
        float fy = (float)y;
        float xmin = 1e9f, xmax = -1e9f;
        for (int e = 0; e < 4; e++) {
            int f = (e + 1) & 3;
            float ax = px[e], ay = py[e], bx = px[f], by = py[f];
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                float t = (fy - ay) / (by - ay);
                float x = ax + t * (bx - ax);
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmax < xmin) continue;
        int xi0 = (int)ceilf(xmin - 0.5f);
        int xi1 = (int)floorf(xmax + 0.5f);
        if (xi0 < 0) xi0 = 0;
        if (xi1 > TFT_WIDTH - 1) xi1 = TFT_WIDTH - 1;
        if (xi1 < xi0) continue;
        int n = xi1 - xi0 + 1;
        for (int i = 0; i < n; i++) row[i] = c;
        lcd_set_window(xi0, y, xi1, y);
        lcd_draw_bitmap(row, (size_t)n * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_test_pattern(void)
{
    /* PANEL-GEOMETRIE-TEST: zeigt, wie das Display wirklich adressiert ist.
     * 4 farbige Quadranten + weisser Rahmen + Kreuz + Eckmarker mit Zahlen.
     * Der Nutzer beschreibt, welche Quadranten/Marker sichtbar sind -> daraus
     * wird die echte Panel-Orientierung bestimmt (240x320 vs 320x240,
     * Spiegelung, gedreht). Loest das "1/4 fehlend"-Problem. */
    ESP_LOGI(TAG, "Geometrie-Test (%dx%d)", TFT_WIDTH, TFT_HEIGHT);
    display_fill(0x0000);

    /* Rahmen */
    display_draw_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, 0xFFFF);

    /* Quadranten (mit 1px Abstand zum Rahmen):
     *  LO = Rot, RO = Gruen, LU = Blau, RU = Gelb */
    int hw = TFT_WIDTH / 2, hh = TFT_HEIGHT / 2;
    display_draw_filled_rect(1, 1, hw - 1, hh - 1, 0xF800);                    /* LO Rot */
    display_draw_filled_rect(hw, 1, TFT_WIDTH - hw - 1, hh - 1, 0x07E0);       /* RO Gruen */
    display_draw_filled_rect(1, hh, hw - 1, TFT_HEIGHT - hh - 1, 0x001F);      /* LU Blau */
    display_draw_filled_rect(hw, hh, TFT_WIDTH - hw - 1, TFT_HEIGHT - hh - 1, 0xFFE0); /* RU Gelb */

    /* Mittiges Kreuz */
    display_draw_filled_rect(hw - 1, 0, 2, TFT_HEIGHT, 0xFFFF);
    display_draw_filled_rect(0, hh - 1, TFT_WIDTH, 2, 0xFFFF);

    /* Eckmarker (weisse 10x10-Quadrate mit Zahlen 1..4) */
    display_draw_filled_rect(2, 2, 10, 10, 0xFFFF);
    display_draw_text(4, 3, "1", 0x0000, 0xFFFF);
    display_draw_filled_rect(TFT_WIDTH - 12, 2, 10, 10, 0xFFFF);
    display_draw_text(TFT_WIDTH - 10, 3, "2", 0x0000, 0xFFFF);
    display_draw_filled_rect(2, TFT_HEIGHT - 12, 10, 10, 0xFFFF);
    display_draw_text(4, TFT_HEIGHT - 11, "3", 0x0000, 0xFFFF);
    display_draw_filled_rect(TFT_WIDTH - 12, TFT_HEIGHT - 12, 10, 10, 0xFFFF);
    display_draw_text(TFT_WIDTH - 10, TFT_HEIGHT - 11, "4", 0x0000, 0xFFFF);
}
