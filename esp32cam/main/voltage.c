/*
 * voltage.c - Spannungsmessung (Fahrzeugbatterie) fuer Autofrontcam
 *
 * ADC1_CH7 (GPIO35) mit Spannungsteiler 100k/22k:
 *   - Teilerverhaeltnis 0,1803 -> Umrechnungsfaktor 5,55 (Batterie = ADC_V * 5,55)
 *   - Max. 14,5V -> ~2,62V am ADC (unter der 3,1V-Grenze bei 11dB)
 *   - Laststrom ~0,12mA (ideal fuer Dauerbetrieb am Fahrzeug)
 *
 * Betriebsmodi:
 *   - "Dauerbetrieb" (default): immer aktiv, kein Sleep
 *   - "Geregelt": Sleep wenn Spannung < Schwellwert, Wake-up wenn darueber
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "config.h"
#include "voltage.h"
#include "nvs_config.h"

static const char *TAG = "VOLT";

/* GPIO35 = ADC1_CH7 (klassischer ESP32, reiner Input-Pin) */
#define VOLT_ADC_CHANNEL    ADC_CHANNEL_7
#define VOLT_ADC_ATTEN      ADC_ATTEN_DB_12   /* ~3,1V Vollausschlag (v6.x: DB_12) */
#define VOLT_SAMPLES        16

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool adc_initialized = false;
static bool cali_valid = false;

static float last_batt_v = 0.0f;
static float threshold_v = VOLT_THRESHOLD_DEFAULT / 10.0f;
static bool regulated = (VOLT_MODE_DEFAULT == 1);

/* ADC-Kalibrierung (Line-Fitting fuer klassischen ESP32, Curve-Fitting als Fallback) */
static bool voltage_calibration_init(adc_channel_t channel, adc_atten_t atten,
                                     adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif

    if (!calibrated) {
        ESP_LOGW(TAG, "ADC-Kalibrierung nicht verfuegbar - nutze ungefaehre Werte");
    }
    *out_handle = handle;
    return calibrated;
}

esp_err_t voltage_init(void)
{
    /* Modus + Grenzwert aus NVS laden */
    uint8_t mode = nvs_config_get_u8("v_mode", VOLT_MODE_DEFAULT);
    regulated = (mode == 1);
    uint8_t thresh = nvs_config_get_u8("v_thresh", VOLT_THRESHOLD_DEFAULT);
    threshold_v = thresh / 10.0f;

    /* ADC-Oneshot-Unit initialisieren */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC-Unit-Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = VOLT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(adc_handle, VOLT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC-Channel-Config fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    cali_valid = voltage_calibration_init(VOLT_ADC_CHANNEL, VOLT_ADC_ATTEN, &cali_handle);

    adc_initialized = true;
    ESP_LOGI(TAG, "Spannungsmessung initialisiert (GPIO%d, Modus: %s, Grenzwert: %.1fV)",
             VOLT_PIN, regulated ? "Geregelt" : "Dauerbetrieb", threshold_v);
    return ESP_OK;
}

/* Einzelne Rohmessung (V in Volt an der Batterie) */
static float voltage_read_raw(void)
{
    int raw = 0;
    if (adc_oneshot_read(adc_handle, VOLT_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0.0f;
    }

    if (cali_valid && cali_handle) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) == ESP_OK) {
            return (float)mv / 1000.0f * VOLT_DIVIDER_FACTOR;
        }
    }

    /* Fallback ohne Kalibrierung: ~3,1V Vollausschlag bei 11dB */
    return (float)raw / 4095.0f * 3.1f * VOLT_DIVIDER_FACTOR;
}

float voltage_read_batt(void)
{
    if (!adc_initialized) return 0.0f;

    /* Mittelwertfilter ueber 16 Samples */
    float sum = 0.0f;
    for (int i = 0; i < VOLT_SAMPLES; i++) {
        sum += voltage_read_raw();
    }
    last_batt_v = sum / VOLT_SAMPLES;
    return last_batt_v;
}

float voltage_get_last(void)
{
    return last_batt_v;
}

float voltage_get_threshold(void)
{
    return threshold_v;
}

void voltage_set_threshold(float volts)
{
    if (volts < 0.0f) volts = 0.0f;
    if (volts > 14.5f) volts = 14.5f;
    threshold_v = volts;
    nvs_config_set_u8("v_thresh", (uint8_t)(volts * 10.0f + 0.5f));
}

bool voltage_is_regulated(void)
{
    return regulated;
}

void voltage_set_mode(bool reg)
{
    regulated = reg;
    nvs_config_set_u8("v_mode", reg ? 1 : 0);
}

bool voltage_sleep_required(void)
{
    if (!regulated) return false;              /* Dauerbetrieb: nie schlafen */
    if (last_batt_v <= 0.0f) return false;     /* Messung ungueltig (Teiler fehlt?) */
    return last_batt_v < threshold_v;
}
