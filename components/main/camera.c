/*
 * camera.c - OV2640 Kamera-Treiber fuer ESP32-CAM (AI-Thinker)
 *
 * Verwendet die esp32-camera Komponente aus der Component Registry
 * (espressif/esp32-camera) - OV2640 DVP parallel, JPEG.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "config.h"
#include "camera.h"

static const char *TAG = "CAM";

static bool camera_ready = false;
static camera_fb_t *fb = NULL;

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
    esp_err_t ret = esp_camera_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init fehlgeschlagen: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        camera_ready = false;
        return ret;
    }

    camera_ready = true;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
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
    } else {
        ESP_LOGW(TAG, "Sensor nicht gefunden, fahre mit Standard fort");
    }

    ESP_LOGI(TAG, "Kamera OV2640 initialisiert (JPEG)");
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(uint8_t **buf, size_t *len)
{
    if (!camera_ready || !buf || !len) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Altes Frame freigeben, falls vorhanden */
    camera_fb_return();

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Frame-Capture fehlgeschlagen");
        return ESP_ERR_TIMEOUT;
    }

    *buf = fb->buf;
    *len = fb->len;
    return ESP_OK;
}

void camera_fb_return(void)
{
    if (fb) {
        esp_camera_fb_return(fb);
        fb = NULL;
    }
}

bool camera_is_ready(void)
{
    return camera_ready;
}

void camera_get_info(char *out, size_t len)
{
    if (!out || len == 0) return;
    snprintf(out, len, "OV2640, %s, Qualitaet %d",
             cam_config.frame_size == FRAMESIZE_SVGA ? "800x600" : "unknown",
             cam_config.jpeg_quality);
}
