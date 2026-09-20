/*
 * camera.c - OV2640 Kamera-Treiber fuer ESP32-CAM (AI-Thinker)
 *
 * Verwendet die esp32-camera Komponente aus der Component Registry
 * (espressif/esp32-camera) - OV2640 DVP parallel, JPEG.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "config.h"
#include "camera.h"

static const char *TAG = "CAM";

static bool camera_ready = false;static uint32_t s_fail_streak = 0;   /* aufeinanderfolgende Capture-Fehler */

/* Grundeinstellungen des Sensors (auch nach einer Recovery anzuwenden) */
static void sensor_apply_defaults(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGW(TAG, "Sensor nicht gefunden, fahre mit Standard fort");
        return;
    }
    /* Grundeinstellungen fuer Tag-Aufnahmen */
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, 0);
    s->set_aec_value(s, 300);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_lenc(s, 1);
    ESP_LOGI(TAG, "Kamera-Sensor konfiguriert");
}static camera_fb_t *fb = NULL;
static SemaphoreHandle_t cam_mutex = NULL;   /* schuetzt Frame-Zugriff (Stream/Capture) */

/* ESP32-CAM (AI-Thinker) Sensor-Pin-Konfiguration */
static camera_config_t cam_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk  = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_Y9,
    .pin_d6 = CAM_PIN_Y8,
    .pin_d5 = CAM_PIN_Y7,
    .pin_d4 = CAM_PIN_Y6,
    .pin_d3 = CAM_PIN_Y5,
    .pin_d2 = CAM_PIN_Y4,
    .pin_d1 = CAM_PIN_Y3,
    .pin_d0 = CAM_PIN_Y2,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href  = CAM_PIN_HREF,
    .pin_pclk  = CAM_PIN_PCLK,

    .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = CAM_LEDC_CHANNEL,

    .pixel_format = CAM_PIXEL_FORMAT,
    .frame_size   = CAM_FRAME_SIZE,
    .jpeg_quality = CAM_JPEG_QUALITY,
    .fb_count     = CAM_FB_COUNT,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_LATEST,   /* neuestes Frame -> minimale Latenz */
};

esp_err_t camera_init(void)
{
    cam_mutex = xSemaphoreCreateMutex();
    if (!cam_mutex) {
        ESP_LOGE(TAG, "Kamera-Mutex-Erstellung fehlgeschlagen");
    }

    esp_err_t ret = esp_camera_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init fehlgeschlagen: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        camera_ready = false;
        return ret;
    }

    camera_ready = true;
    s_fail_streak = 0;
    sensor_apply_defaults();

    ESP_LOGI(TAG, "Kamera OV2640 initialisiert (JPEG)");
    return ESP_OK;
}

/* Nur das Frame-Objekt freigeben (OHNE Mutex-Freigabe) - intern fuer die
 * Neuaufnahme unter gehaltenem Mutex. */
static void camera_release_fb(void)
{
    if (fb) {
        esp_camera_fb_return(fb);
        fb = NULL;
    }
}

esp_err_t camera_capture_jpeg(uint8_t **buf, size_t *len)
{
    if (!camera_ready || !buf || !len) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cam_mutex && xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Altes Frame freigeben, falls vorhanden (Mutex bleibt gehalten) */
    camera_release_fb();

    fb = esp_camera_fb_get();
    if (!fb) {
        s_fail_streak++;
        ESP_LOGE(TAG, "Frame-Capture fehlgeschlagen");
        if (cam_mutex) xSemaphoreGive(cam_mutex);
        return ESP_ERR_TIMEOUT;
    }
    s_fail_streak = 0;

    *buf = fb->buf;
    *len = fb->len;
    return ESP_OK;
}

void camera_fb_return(void)
{
    camera_release_fb();
    if (cam_mutex) xSemaphoreGive(cam_mutex);
}

bool camera_is_ready(void)
{
    return camera_ready;
}

/* Aufeinanderfolgende Capture-Fehler (fuer den Selbstheilungs-Watchdog) */
uint32_t camera_fail_streak(void)
{
    return s_fail_streak;
}

/* Kamera-Recovery: Treiber/Sensor neu initialisieren. Wird vom Watchdog
 * aufgerufen, wenn dauerhaft keine Frames mehr kommen (Sensor haengt).
 * Die Bildparameter (Helligkeit/Kontrast/Nacht) setzt der Aufrufer danach. */
esp_err_t camera_recover(void)
{
    ESP_LOGW(TAG, "Kamera-Recovery: Treiber neu initialisieren");
    if (cam_mutex && xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Kamera-Recovery: Mutex belegt");
        return ESP_ERR_TIMEOUT;
    }

    camera_release_fb();
    esp_err_t ret = esp_camera_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_deinit: %s", esp_err_to_name(ret));
    }

    /* Sensor wirklich zuruecksetzen: ohne Power-Cycle bleibt der OV2640 nach
     * einer Software-Rueckkehr gern in einem undefinierten Zustand (dann kommen
     * kaputte/leere Frames). PWDN ist nach dem Deinit frei -> selbst treiben. */
#if CAM_PIN_PWDN >= 0
    gpio_config_t pwdn = {
        .pin_bit_mask = (1ULL << CAM_PIN_PWDN),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&pwdn) == ESP_OK) {
        gpio_set_level(CAM_PIN_PWDN, 1);   /* Sensor aus */
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(CAM_PIN_PWDN, 0);   /* Sensor an  */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
#endif

    ret = esp_camera_init(&cam_config);
    if (ret != ESP_OK) {
        camera_ready = false;
        ESP_LOGE(TAG, "Kamera-Recovery fehlgeschlagen: %s", esp_err_to_name(ret));
    } else {
        camera_ready = true;
        sensor_apply_defaults();
    }
    s_fail_streak = 0;

    if (cam_mutex) xSemaphoreGive(cam_mutex);
    ESP_LOGW(TAG, "Kamera-Recovery %s", ret == ESP_OK ? "ok" : "fehlgeschlagen");
    return ret;
}

/* Bildparameter anwenden (Werte werden auf -2..2 begrenzt).
 * WICHTIG (OV2640): Bei aktiver Auto-Exposure (AEC)/Auto-Gain (AGC) werden
 * die DSP-Register fuer Helligkeit (0x09) und Kontrast (0x07) automatisch
 * ausgeregelt und sind kaum sichtbar. Zuverlaessig wirkt dagegen das
 * Belichtungsziel set_ae_level() - das verschiebt die Helligkeit trotz
 * Auto-Exposure sichtbar. Wir setzen daher BEIDES: DSP-Register + AE-Level. */
esp_err_t camera_set_picture(int brightness, int contrast, int saturation)
{
    if (!camera_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_ERR_INVALID_STATE;

    if (brightness < -2) brightness = -2;
    if (brightness > 2)  brightness = 2;
    if (contrast < -2) contrast = -2;
    if (contrast > 2)  contrast = 2;
    if (saturation < -2) saturation = -2;
    if (saturation > 2)  saturation = 2;

    s->set_brightness(s, brightness);
    s->set_contrast(s, contrast);
    s->set_saturation(s, saturation);
    /* Helligkeit sichtbar machen: Belichtungsziel der Auto-Exposure
     * verschieben (-2 = dunkler, +2 = heller). */
    s->set_ae_level(s, brightness);
    ESP_LOGI(TAG, "Bild-Parameter: Helligkeit %d, Kontrast %d, Saettigung %d, AE-Level %d",
             brightness, contrast, saturation, brightness);
    return ESP_OK;
}

/* Nachtsicht-Modus: im Normalmodus ist der Gainceiling auf 2x begrenzt
 * (wenig Rauschen). Fuer wenig Licht wird er deutlich angehoben und die
 * Belichtungszeit verlaengert, damit dunkle Szenen sichtbar werden. */
esp_err_t camera_set_night_mode(bool enable)
{
    if (!camera_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_ERR_INVALID_STATE;

    if (enable) {
        s->set_gainceiling(s, GAINCEILING_16X);  /* mehr Verstaerkung bei wenig Licht */
        s->set_aec_value(s, 800);                /* laengere Belichtung (dunkler) */
        s->set_ae_level(s, 2);
        s->set_dcw(s, 1);
        ESP_LOGI(TAG, "Nachtsicht-Modus AN (Gain 16x, Belichtung 800)");
    } else {
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_aec_value(s, 300);
        s->set_ae_level(s, 0);
        s->set_dcw(s, 0);
        ESP_LOGI(TAG, "Nachtsicht-Modus AUS (Gain 2x, Belichtung 300)");
    }
    return ESP_OK;
}

void camera_get_info(char *out, size_t len)
{
    if (!out || len == 0) return;
    snprintf(out, len, "OV2640, %s, Qualitaet %d",
             cam_config.frame_size == FRAMESIZE_SVGA ? "800x600" : "unknown",
             cam_config.jpeg_quality);
}
