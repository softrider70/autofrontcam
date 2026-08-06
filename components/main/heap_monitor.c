/*
 * heap_monitor.c - Heap-Ueberwachung fuer Heltec WiFi LoRa 32 V3
 *
 * Gibt periodisch Heap-Statistiken per ESP_LOG aus.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "heap_monitor.h"

static const char *TAG = "HEAP_MON";
static TaskHandle_t monitor_task = NULL;
static bool running = false;

/* Monitor-Task: alle 10s Heap-Statistiken ausgeben */
static void heap_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Heap-Monitor gestartet");

    while (running) {
        /* DRAM (interner Speicher) */
        uint32_t free_dram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        uint32_t total_dram = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        uint32_t min_free_dram = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

        /* PSRAM (externer Speicher, falls vorhanden) */
        uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        uint32_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "=== Heap-Status ===");
        ESP_LOGI(TAG, "DRAM:  frei=%lu / %lu Bytes, min=frei=%lu Bytes",
                 free_dram, total_dram, min_free_dram);
        ESP_LOGI(TAG, "DRAM:  %.1f%% belegt",
                 100.0f * (1.0f - (float)free_dram / (float)total_dram));

        if (total_psram > 0) {
            ESP_LOGI(TAG, "PSRAM: frei=%lu / %lu Bytes, min=frei=%lu Bytes",
                     free_psram, total_psram, min_free_psram);
            ESP_LOGI(TAG, "PSRAM: %.1f%% belegt",
                     100.0f * (1.0f - (float)free_psram / (float)total_psram));
        }

        /* Groesster freier Block */
        uint32_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "Groesster freier DRAM-Block: %lu Bytes", largest_free);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    vTaskDelete(NULL);
}

void heap_monitor_init(void)
{
    if (running) {
        return;
    }

    running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        heap_monitor_task,
        "heap_mon",
        configMINIMAL_STACK_SIZE + 512,
        NULL,
        tskIDLE_PRIORITY,
        &monitor_task,
        0  /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Monitor-Task-Erstellung fehlgeschlagen");
        running = false;
    }
}