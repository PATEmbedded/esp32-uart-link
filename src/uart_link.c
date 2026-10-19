#include "uart_link.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_link";

/* Driver buffers: room for several of the largest frames, and at least what the driver needs. */
#define RX_BUFFER_SIZE (4u * UL_MAX_WIRE < 1024u ? 1024u : 4u * UL_MAX_WIRE)
#define TX_BUFFER_SIZE (2u * UL_MAX_WIRE < 512u ? 512u : 2u * UL_MAX_WIRE)
#define EVENT_QUEUE_LENGTH 16

/* Not a driver event: sent to the task so it looks at the link again. */
#define WAKE_EVENT UART_EVENT_MAX

struct uart_link {
    uart_link_config_t cfg;
    ul_link_t core;
    SemaphoreHandle_t lock;      /* guards core */
    SemaphoreHandle_t send_turn; /* one reliable message at a time */
    SemaphoreHandle_t sent;      /* given when the message in flight is done */
    bool delivered;
    QueueHandle_t events; /* created by the UART driver */
    QueueHandle_t received;
    TaskHandle_t task;
    TaskHandle_t closer;
    volatile bool stopping;
    uint32_t datagrams_dropped;
    uint32_t uart_overflows;
    uart_link_message_t incoming; /* used by the link task only */
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void wake(struct uart_link *link)
{
    const uart_event_t event = {.type = WAKE_EVENT};
    /* If the queue is full the task is busy anyway and looks at the link afterwards. */
    (void)xQueueSend(link->events, &event, 0);
}

static void on_write(void *ctx, const uint8_t *data, size_t len)
{
    struct uart_link *link = ctx;
    uart_write_bytes(link->cfg.port, data, len);
}

static bool on_message(void *ctx, const uint8_t *payload, size_t len, bool reliable)
{
    struct uart_link *link = ctx;
    link->incoming.len = len;
    link->incoming.reliable = reliable;
    memcpy(link->incoming.data, payload, len);
    if (xQueueSend(link->received, &link->incoming, 0) == pdTRUE) {
        return true;
    }
    if (!reliable) {
        link->datagrams_dropped++;
    }
    return false; /* a reliable message is not acknowledged, so the peer repeats it */
}

static void on_sent(void *ctx, bool delivered)
{
    struct uart_link *link = ctx;
    link->delivered = delivered;
    xSemaphoreGive(link->sent);
}

static void read_pending(struct uart_link *link)
{
    uint8_t chunk[128];
    size_t pending = 0;
    uart_get_buffered_data_len(link->cfg.port, &pending);
    while (pending > 0u) {
        const int n = uart_read_bytes(link->cfg.port, chunk, pending < sizeof(chunk) ? pending : sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        xSemaphoreTake(link->lock, portMAX_DELAY);
        ul_link_receive(&link->core, chunk, (size_t)n, now_ms());
        xSemaphoreGive(link->lock);
        pending -= (size_t)n;
    }
}

static void link_task(void *arg)
{
    struct uart_link *link = arg;

    while (!link->stopping) {
        xSemaphoreTake(link->lock, portMAX_DELAY);
        const int32_t wait_ms = ul_link_time_to_deadline(&link->core, now_ms());
        xSemaphoreGive(link->lock);
        /* Round up, so the task does not wake before the deadline and spin. */
        const TickType_t wait =
            wait_ms < 0 ? portMAX_DELAY : (TickType_t)(((uint32_t)wait_ms + portTICK_PERIOD_MS - 1u) / portTICK_PERIOD_MS);

        uart_event_t event;
        if (xQueueReceive(link->events, &event, wait) == pdTRUE) {
            switch (event.type) {
            case UART_DATA:
                read_pending(link);
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                /* Bytes were lost. Their frames fail the CRC check; reliable ones are sent again. */
                link->uart_overflows++;
                uart_flush_input(link->cfg.port);
                xQueueReset(link->events);
                break;
            default:
                break; /* breaks, frame and parity errors: the CRC catches what they damage */
            }
        }

        xSemaphoreTake(link->lock, portMAX_DELAY);
        ul_link_poll(&link->core, now_ms());
        xSemaphoreGive(link->lock);
    }

    xTaskNotifyGive(link->closer);
    vTaskDelete(NULL);
}

static void free_link(struct uart_link *link)
{
    if (link->lock != NULL) {
        vSemaphoreDelete(link->lock);
    }
    if (link->send_turn != NULL) {
        vSemaphoreDelete(link->send_turn);
    }
    if (link->sent != NULL) {
        vSemaphoreDelete(link->sent);
    }
    if (link->received != NULL) {
        vQueueDelete(link->received);
    }
    free(link);
}

esp_err_t uart_link_open(const uart_link_config_t *config, uart_link_handle_t *out_link)
{
    ESP_RETURN_ON_FALSE(config != NULL && out_link != NULL, ESP_ERR_INVALID_ARG, TAG, "null argument");
    ESP_RETURN_ON_FALSE(config->rx_queue_length > 0u, ESP_ERR_INVALID_ARG, TAG, "rx_queue_length is 0");

    struct uart_link *link = calloc(1, sizeof(*link));
    ESP_RETURN_ON_FALSE(link != NULL, ESP_ERR_NO_MEM, TAG, "no memory");
    link->cfg = *config;
    link->lock = xSemaphoreCreateMutex();
    link->send_turn = xSemaphoreCreateMutex();
    link->sent = xSemaphoreCreateBinary();
    link->received = xQueueCreate(config->rx_queue_length, sizeof(uart_link_message_t));
    if (link->lock == NULL || link->send_turn == NULL || link->sent == NULL || link->received == NULL) {
        free_link(link);
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(config->port, (int)RX_BUFFER_SIZE, (int)TX_BUFFER_SIZE, EVENT_QUEUE_LENGTH,
                                        &link->events, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: driver install failed (%s)", (int)config->port, esp_err_to_name(err));
        free_link(link);
        return err;
    }
    err = uart_param_config(config->port, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(config->port, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: configuration failed (%s)", (int)config->port, esp_err_to_name(err));
        uart_driver_delete(config->port);
        free_link(link);
        return err;
    }

    const ul_link_config_t link_config = {
        .write = on_write,
        .on_message = on_message,
        .on_sent = on_sent,
        .ctx = link,
        .session = (uint16_t)esp_random(), /* new on every start, see ul/link.h */
        .ack_timeout_ms = config->ack_timeout_ms,
        .max_retries = config->max_retries,
    };
    ul_link_init(&link->core, &link_config);

    if (xTaskCreatePinnedToCore(link_task, "uart_link", config->task_stack_size, link, config->task_priority,
                                &link->task, config->task_core) != pdPASS) {
        uart_driver_delete(config->port);
        free_link(link);
        return ESP_ERR_NO_MEM;
    }
    *out_link = link;
    return ESP_OK;
}

void uart_link_close(uart_link_handle_t link)
{
    if (link == NULL) {
        return;
    }
    link->closer = xTaskGetCurrentTaskHandle();
    link->stopping = true;
    wake(link);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uart_driver_delete(link->cfg.port);
    free_link(link);
}

esp_err_t uart_link_send(uart_link_handle_t link, const void *data, size_t len, TickType_t wait)
{
    ESP_RETURN_ON_FALSE(len <= UL_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE, TAG, "payload of %u bytes is too long",
                        (unsigned)len);
    if (xSemaphoreTake(link->send_turn, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(link->sent, 0); /* nothing should be left over; make sure */

    xSemaphoreTake(link->lock, portMAX_DELAY);
    const ul_result_t result = ul_link_send(&link->core, data, len, now_ms());
    xSemaphoreGive(link->lock);

    esp_err_t err = ESP_FAIL;
    if (result == UL_OK) {
        wake(link); /* the task has to watch the new deadline */
        /* Bounded: the link gives up after max_retries repeats. */
        xSemaphoreTake(link->sent, portMAX_DELAY);
        err = link->delivered ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(link->send_turn);
    return err;
}

esp_err_t uart_link_send_datagram(uart_link_handle_t link, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= UL_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE, TAG, "payload of %u bytes is too long",
                        (unsigned)len);
    xSemaphoreTake(link->lock, portMAX_DELAY);
    (void)ul_link_send_datagram(&link->core, data, len);
    xSemaphoreGive(link->lock);
    return ESP_OK;
}

esp_err_t uart_link_receive(uart_link_handle_t link, uart_link_message_t *message, TickType_t wait)
{
    return xQueueReceive(link->received, message, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void uart_link_get_stats(uart_link_handle_t link, uart_link_stats_t *stats)
{
    xSemaphoreTake(link->lock, portMAX_DELAY);
    stats->link = ul_link_stats(&link->core);
    stats->datagrams_dropped = link->datagrams_dropped;
    stats->uart_overflows = link->uart_overflows;
    xSemaphoreGive(link->lock);
}
