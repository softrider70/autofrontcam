/*
 * sleep.c - Sleep-/Wake-up-Steuerung fuer Autofrontcam
 *
 * Deep-Sleep mit RTC-Timer-Wakeup (SLEEP_CHECK_INTERVAL_MS):
 *   - Early-Check nach dem Boot: Spannung < Grenzwert -> sofort wieder schlafen
 *   - Spannungs-Monitor-Task im Betrieb: misst alle VOLT_MONITOR_INTERVAL_MS,
 *     bei Unterschreitung -> Deep-Sleep
 *   - Wake-up: RTC-Timer weckt das System; beim Boot wird erneut gemessen.
 *     Liegt die Spannung ueber dem Grenzwert, bootet das System normal durch
 *     (AP + Kamera + Stream), andernfalls folgt der naechste Deep-Sleep.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "config.h"
#include "sleep.h"
#include "voltage.h"

static const char *TAG = "SLEEP";

/* In den Deep-Sleep gehen (kehrt nicht zurueck) */
static void sleep_enter(void)
{
    ESP_LOGI(TAG, "Deep-Sleep fuer %u s (Timer-Wakeup)",
             SLEEP_CHECK_INTERVAL_MS / 1000);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_CHECK_INTERVAL_MS * 1000);
    esp_deep_sleep_start();
    /* wird nicht erreicht */
}

void sleep_check_early(void)
{
    if (!voltage_is_regulated()) {
        return; /* Dauerbetrieb: nie schlafen */
    }

    float v = voltage_read_batt();
    if (v <= 0.0f) {
        ESP_LOGW(TAG, "Spannungsmessung ungueltig - fahre normal hoch");
        return;
    }

    if (v < voltage_get_threshold()) {
        ESP_LOGI(TAG, "Early-Check: %.1fV < Grenzwert %.1fV - sofort Deep-Sleep",
                 v, voltage_get_threshold());
        sleep_enter();
    }
}

static void voltage_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Spannungs-Monitor gestartet (Intervall %u s)",
             VOLT_MONITOR_INTERVAL_MS / 1000);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(VOLT_MONITOR_INTERVAL_MS));
        voltage_read_batt();
        if (voltage_sleep_required()) {
            ESP_LOGI(TAG, "Spannung %.1fV < Grenzwert %.1fV - gehe in Deep-Sleep",
                     voltage_get_last(), voltage_get_threshold());
            sleep_enter();
        }
    }
}

void sleep_start_monitor(void)
{
    if (!voltage_is_regulated()) {
        ESP_LOGI(TAG, "Modus Dauerbetrieb - kein Sleep-Monitor aktiv");
        return;
    }

    BaseType_t ret = xTaskCreate(voltage_monitor_task, "volt_mon",
                                 TASK_STACK_MONITOR, NULL,
                                 TASK_PRIORITY_MONITOR, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Spannungs-Monitor-Task-Erstellung fehlgeschlagen");
    }
}
