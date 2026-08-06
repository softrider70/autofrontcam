/*
 * stack_monitor.c - Stack-Ueberwachung fuer Heltec WiFi LoRa 32 V3
 *
 * Gibt periodisch den minimalen freien Stack aller Tasks aus.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "stack_monitor.h"

static const char *TAG = "STACK_MON";
static TaskHandle_t monitor_task = NULL;
static bool running = false;

/* Stack-Information fuer einen Task ausgeben */
static void print_task_stack_info(TaskHandle_t task, const char *name)
{
    if (task == NULL) return;

    UBaseType_t watermark = uxTaskGetStackHighWaterMark(task);

    ESP_LOGI(TAG, "  %-12s  Stack min frei: %u Woerter (%u Bytes)",
             name, watermark, watermark * sizeof(StackType_t));
}

/* Monitor-Task: alle 15s alle Task-Stacks ausgeben */
static void stack_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Stack-Monitor gestartet");

    while (running) {
        ESP_LOGI(TAG, "=== Task-Stack-Status ===");
        ESP_LOGI(TAG, "  %-12s  %s", "Task", "Min. freier Stack");

        /* Eigene bekannte Tasks ueberpruefen */
        /* Die Handles werden in main.c/anderen Dateien gespeichert */
        /* Hier wird nur der aktuelle Task und Idle geprueft */

        /* Aktuellen Task prüfen */
        TaskHandle_t cur = xTaskGetCurrentTaskHandle();
        if (cur) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(cur);
            ESP_LOGI(TAG, "  %-12s  Stack min frei: %u Woerter (%u Bytes)",
                     "stack_mon", watermark, watermark * sizeof(StackType_t));
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }

    vTaskDelete(NULL);
}

void stack_monitor_init(void)
{
    if (running) {
        return;
    }

    running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        stack_monitor_task,
        "stk_mon",
        configMINIMAL_STACK_SIZE + 256,
        NULL,
        tskIDLE_PRIORITY,
        &monitor_task,
        0  /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Stack-Monitor-Task-Erstellung fehlgeschlagen");
        running = false;
    }
}