/*
 * Two producer tasks, one on each core, put readings into a FreeRTOS queue. A sender task takes
 * them out and sends each one as a reliable message, so every reading is either acknowledged by
 * the PC or counted as lost. On single-core chips both producers share the core.
 *
 * On the PC:  python tools/uart_link.py --port COM5 listen
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "uart_link.h"

static const char *TAG = "sensor_stream";

typedef struct {
    char producer;
    int core;
    uint32_t sequence;
    int64_t time_us;
} reading_t;

typedef struct {
    char name;
    uint32_t period_ms;
} producer_t;

static QueueHandle_t s_readings;

static void produce(void *arg)
{
    const producer_t *self = arg;
    reading_t reading = {.producer = self->name};
    for (;;) {
        reading.sequence++;
        reading.core = esp_cpu_get_core_id();
        reading.time_us = esp_timer_get_time();
        if (xQueueSend(s_readings, &reading, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, reading %c%" PRIu32 " dropped", self->name, reading.sequence);
        }
        vTaskDelay(pdMS_TO_TICKS(self->period_ms));
    }
}

static void send_readings(void *arg)
{
    uart_link_handle_t link = arg;
    uint32_t lost = 0;
    reading_t reading;
    char text[64];
    for (;;) {
        xQueueReceive(s_readings, &reading, portMAX_DELAY);
        const int n = snprintf(text, sizeof(text), "%c %" PRIu32 " core %d at %" PRId64 " ms", reading.producer,
                               reading.sequence, reading.core, reading.time_us / 1000);
        if (uart_link_send(link, text, (size_t)n, portMAX_DELAY) != ESP_OK) {
            ESP_LOGW(TAG, "%s: not acknowledged (%" PRIu32 " lost so far)", text, ++lost);
        }
    }
}

static BaseType_t core_for(int core)
{
#if SOC_CPU_CORES_NUM > 1 && !defined(CONFIG_FREERTOS_UNICORE)
    return core;
#else
    (void)core;
    return tskNO_AFFINITY;
#endif
}

void app_main(void)
{
    uart_link_config_t config =
        UART_LINK_CONFIG_DEFAULT((uart_port_t)CONFIG_EXAMPLE_UART_PORT, CONFIG_EXAMPLE_TX_GPIO, CONFIG_EXAMPLE_RX_GPIO);
    config.baud_rate = CONFIG_EXAMPLE_BAUD_RATE;
    uart_link_handle_t link = NULL;
    ESP_ERROR_CHECK(uart_link_open(&config, &link));

    s_readings = xQueueCreate(32, sizeof(reading_t));
    static producer_t fast = {'A', 200};
    static producer_t slow = {'B', 1000};
    xTaskCreatePinnedToCore(produce, "producer A", 3072, &fast, 5, NULL, core_for(0));
    xTaskCreatePinnedToCore(produce, "producer B", 3072, &slow, 5, NULL, core_for(1));
    xTaskCreatePinnedToCore(send_readings, "sender", 4096, link, 6, NULL, tskNO_AFFINITY);
    ESP_LOGI(TAG, "streaming on UART%d, TX GPIO %d, RX GPIO %d", CONFIG_EXAMPLE_UART_PORT, CONFIG_EXAMPLE_TX_GPIO,
             CONFIG_EXAMPLE_RX_GPIO);
}
