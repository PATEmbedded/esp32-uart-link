/*
 * Sends every message back: reliable messages reliably, datagrams as datagrams. Connect a USB
 * serial adapter to the TX and RX pins and run, on the PC:
 *
 *     python tools/uart_link.py --port COM5 send "hello" --count 10
 */
#include <inttypes.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "uart_link.h"

static const char *TAG = "echo";

void app_main(void)
{
    uart_link_config_t config =
        UART_LINK_CONFIG_DEFAULT((uart_port_t)CONFIG_EXAMPLE_UART_PORT, CONFIG_EXAMPLE_TX_GPIO, CONFIG_EXAMPLE_RX_GPIO);
    config.baud_rate = CONFIG_EXAMPLE_BAUD_RATE;
    uart_link_handle_t link = NULL;
    ESP_ERROR_CHECK(uart_link_open(&config, &link));
    ESP_LOGI(TAG, "echoing on UART%d, TX GPIO %d, RX GPIO %d", CONFIG_EXAMPLE_UART_PORT, CONFIG_EXAMPLE_TX_GPIO,
             CONFIG_EXAMPLE_RX_GPIO);

    static uart_link_message_t message;
    for (;;) {
        if (uart_link_receive(link, &message, pdMS_TO_TICKS(10000)) != ESP_OK) {
            uart_link_stats_t stats;
            uart_link_get_stats(link, &stats);
            ESP_LOGI(TAG, "received %" PRIu32 ", acknowledged %" PRIu32 ", sent again %" PRIu32 ", CRC errors %" PRIu32,
                     stats.link.received, stats.link.sent, stats.link.retransmits, stats.link.wire.crc_errors);
            continue;
        }
        const esp_err_t err = message.reliable ? uart_link_send(link, message.data, message.len, portMAX_DELAY)
                                               : uart_link_send_datagram(link, message.data, message.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "echo of %u bytes not acknowledged", (unsigned)message.len);
        }
    }
}
