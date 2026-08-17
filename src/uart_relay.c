#include "uart_relay.h"

#ifdef CONFIG_UART_RELAY_ENABLE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "mqtt_transport.h"
#include "device_config.h"

static const char *TAG = "uart-relay";

#define RELAY_UART      UART_NUM_1
#define RELAY_TX_PIN    CONFIG_UART_RELAY_TX_PIN
#define RELAY_RX_PIN    CONFIG_UART_RELAY_RX_PIN
#define RELAY_BAUD      CONFIG_UART_RELAY_BAUD
#define RELAY_BUF_SIZE  256
#define RELAY_STACK     3072

static char s_tx_topic[64];
static char s_rx_topic[64];

static void mqtt_to_uart_handler(const char *topic, const uint8_t *data, size_t len)
{
    uart_write_bytes(RELAY_UART, data, len);
}

static void uart_to_mqtt_task(void *arg)
{
    uint8_t buf[RELAY_BUF_SIZE];

    while (1) {
        int n = uart_read_bytes(RELAY_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            mqtt_transport_publish(s_tx_topic, (const char *)buf, n);
        }
    }
}

esp_err_t uart_relay_init(void)
{
    const device_config_t *cfg = device_config_get();
    snprintf(s_tx_topic, sizeof(s_tx_topic), "device/%s/console/out", cfg->device_id);
    snprintf(s_rx_topic, sizeof(s_rx_topic), "device/%s/console/in", cfg->device_id);

    uart_config_t uart_cfg = {
        .baud_rate  = RELAY_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RELAY_UART, RELAY_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RELAY_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(RELAY_UART, RELAY_TX_PIN, RELAY_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(mqtt_transport_register_handler(s_rx_topic, mqtt_to_uart_handler));

    xTaskCreate(uart_to_mqtt_task, "uart_relay", RELAY_STACK, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    ESP_LOGI(TAG, "UART%d relay on GPIO%d(TX)/GPIO%d(RX) @ %d baud",
             RELAY_UART, RELAY_TX_PIN, RELAY_RX_PIN, RELAY_BAUD);
    ESP_LOGI(TAG, "  out: %s  in: %s", s_tx_topic, s_rx_topic);
    return ESP_OK;
}

#endif /* CONFIG_UART_RELAY_ENABLE */
