/*
 * splash.c - Boot-Splash: eingebettetes Foto statt Farb-Testmuster
 *
 * Das Bild liegt als JPEG im Flash (CMake EMBED_FILES) und wird beim Boot mit
 * esp_jpeg in einen PSRAM-Puffer dekodiert und in den Framebuffer kopiert.
 * Ein JPEG statt Rohdaten spart Flash: 480x320 RGB565 waeren 307 KB, das JPEG
 * ist ~39 KB. Dekodieren dauert nur wenige Millisekunden (macht der Stream
 * ohnehin pro Frame).
 *
 * Bild: "2024 Skoda Octavia Estate 1.5 TSI SE in Listers Skoda Coventry 01.jpg"
 *   Motiv:  blaue Skoda Octavia IV (Combi), Front-Seitenansicht
 *   Quelle: Wikimedia Commons
 *           https://commons.wikimedia.org/wiki/File:2024_%C5%A0koda_Octavia_Estate_1.5_TSI_SE_in_Listers_%C5%A0koda_Coventry_01.jpg
 *   Lizenz: CC0 1.0 (Public Domain Dedication) - keine Auflagen
 *   Aenderung: auf 480x320 beschnitten/skaliert, als JPEG (Qualitaet 80)
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "config.h"
#include "display.h"
#include "splash.h"

static const char *TAG = "s3lcd_splash";

/* Von CMake EMBED_FILES erzeugte Symbole */
extern const uint8_t splash_jpg_start[] asm("_binary_splash_jpg_start");
extern const uint8_t splash_jpg_end[]   asm("_binary_splash_jpg_end");

void splash_show(void)
{
    const uint8_t *jpg = splash_jpg_start;
    size_t len = (size_t)(splash_jpg_end - splash_jpg_start);
    if (len < 16) {
        ESP_LOGW(TAG, "Splash-Bild fehlt");
        return;
    }

    const uint32_t cap = (uint32_t)TFT_WIDTH * TFT_HEIGHT * 2;
    uint16_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "Kein PSRAM fuer Splash - uebersprungen");
        return;
    }

    /* swap_color_bytes = 0: wie im Stream-Client (display_blit liest die Pixel
     * als uint16_t little-endian und sendet high-byte-first). */
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = (uint32_t)len,
        .outbuf = (uint8_t *)buf,
        .outbuf_size = cap,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t out = { 0 };
    esp_err_t err = esp_jpeg_decode(&cfg, &out);
    if (err != ESP_OK || out.width == 0 || out.height == 0 ||
        out.width > TFT_WIDTH || out.height > TFT_HEIGHT) {
        ESP_LOGW(TAG, "Splash-Dekodierung fehlgeschlagen (%s)", esp_err_to_name(err));
        heap_caps_free(buf);
        return;
    }

    display_lock();
    display_fill(0x0000);
    display_blit(buf, (TFT_WIDTH - (int)out.width) / 2,
                      (TFT_HEIGHT - (int)out.height) / 2,
                      (int)out.width, (int)out.height);
    display_commit();
    display_unlock();

    ESP_LOGI(TAG, "Splash angezeigt: %ux%u (%u B JPEG)",
             (unsigned)out.width, (unsigned)out.height, (unsigned)len);
    heap_caps_free(buf);
}
