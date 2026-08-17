#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "demo_task.h"

static const char *TAG = "demo";

static volatile uint32_t counter_a;
static volatile uint32_t counter_b;

static void demo_counter_task(void *arg)
{
    const char *name = pcTaskGetName(NULL);
    volatile uint32_t *counter = (volatile uint32_t *)arg;

    ESP_LOGI(TAG, "Task '%s' started", name);

    while (1) {
        (*counter)++;
        if ((*counter % 1000) == 0) {
            ESP_LOGI(TAG, "%s = %lu", name, (unsigned long)*counter);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void demo_task_start(void)
{
    counter_a = 0;
    counter_b = 0;

    xTaskCreate(demo_counter_task, "counter_a", 2048,
                (void *)&counter_a, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(demo_counter_task, "counter_b", 2048,
                (void *)&counter_b, tskIDLE_PRIORITY + 1, NULL);

    ESP_LOGI(TAG, "Demo tasks started — attach GDB and inspect counter_a/counter_b");
}
