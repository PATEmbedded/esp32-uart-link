#ifndef UART_LINK_H
#define UART_LINK_H

/*
 * Framed, CRC-checked messages over an ESP32 UART, with acknowledged delivery.
 *
 * uart_link_open() installs the UART driver and starts one task per link. The task reads the
 * UART events, runs the link and repeats unacknowledged messages. Other tasks send with
 * uart_link_send(), which returns once the peer has acknowledged the message or the retries have
 * run out, and take received messages from a queue with uart_link_receive(). A full queue makes
 * the peer wait and repeat the message instead of losing it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "ul/link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uart_link *uart_link_handle_t;

typedef struct {
    uart_port_t port;
    int tx_pin; /**< UART_PIN_NO_CHANGE keeps the current pin */
    int rx_pin;
    int baud_rate;
    uint32_t ack_timeout_ms;
    uint8_t max_retries;
    size_t rx_queue_length; /**< received messages waiting for uart_link_receive() */
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    BaseType_t task_core; /**< tskNO_AFFINITY for any core */
} uart_link_config_t;

#define UART_LINK_CONFIG_DEFAULT(port_, tx_pin_, rx_pin_)                                                  \
    {                                                                                                      \
        .port = (port_), .tx_pin = (tx_pin_), .rx_pin = (rx_pin_), .baud_rate = 115200,                    \
        .ack_timeout_ms = 100, .max_retries = 5, .rx_queue_length = 8, .task_priority = 10,                \
        .task_stack_size = 4096, .task_core = tskNO_AFFINITY,                                              \
    }

typedef struct {
    size_t len;
    bool reliable; /**< sent with uart_link_send(), not as a datagram */
    uint8_t data[UL_MAX_PAYLOAD];
} uart_link_message_t;

typedef struct {
    ul_link_stats_t link;       /**< delivery and framing counters */
    uint32_t datagrams_dropped; /**< datagrams lost because the receive queue was full */
    uint32_t uart_overflows;    /**< the UART FIFO or the driver's buffer overflowed */
} uart_link_stats_t;

esp_err_t uart_link_open(const uart_link_config_t *config, uart_link_handle_t *out_link);

/** Stops the task and removes the UART driver. No other call may be running on this link. */
void uart_link_close(uart_link_handle_t link);

/**
 * Sends a message and waits until the peer acknowledges it. Several tasks may call this; they
 * take turns. wait limits only the time to get a turn.
 *
 * @return ESP_OK when acknowledged, ESP_FAIL when the retries ran out, ESP_ERR_TIMEOUT when no
 *         turn came within wait, ESP_ERR_INVALID_SIZE for a payload longer than UL_MAX_PAYLOAD.
 */
esp_err_t uart_link_send(uart_link_handle_t link, const void *data, size_t len, TickType_t wait);

/** Sends a message once, without acknowledgement, and returns at once. */
esp_err_t uart_link_send_datagram(uart_link_handle_t link, const void *data, size_t len);

/** Takes the next received message. ESP_ERR_TIMEOUT if none came within wait. */
esp_err_t uart_link_receive(uart_link_handle_t link, uart_link_message_t *message, TickType_t wait);

void uart_link_get_stats(uart_link_handle_t link, uart_link_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
