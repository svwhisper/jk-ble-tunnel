#include "supervisor.h"
#include "config.h"
#include "nb_state.h"
#include "tunnel_cli.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "supervisor";

static void supervisor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "heap=%u conns=%d tunnel=%d",
                 (unsigned)heap, nb_active_conn_count(), tunnel_cli_up());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void supervisor_start(void)
{
    xTaskCreatePinnedToCore(supervisor_task, "supervisor", 4096, NULL, 3, NULL, 0);
}
