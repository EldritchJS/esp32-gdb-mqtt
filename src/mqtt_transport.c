#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_transport.h"

static const char *TAG = "mqtt-transport";

#define RX_QUEUE_DEPTH 512
#define TX_QUEUE_DEPTH 8
#define TX_MSG_MAX 512
#define TX_TASK_STACK 4096
#define MAX_HANDLERS 4

typedef struct {
    uint8_t data[TX_MSG_MAX];
    size_t len;
} tx_msg_t;

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_tx_queue;

static char s_cmd_topic[64];
static char s_resp_topic[64];

typedef struct {
    char topic[64];
    mqtt_msg_handler_t handler;
} topic_handler_t;

static topic_handler_t s_handlers[MAX_HANDLERS];
static int s_handler_count;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        ESP_LOGI(TAG, "Subscribed to %s", s_cmd_topic);
        for (int i = 0; i < s_handler_count; i++) {
            esp_mqtt_client_subscribe(s_client, s_handlers[i].topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s", s_handlers[i].topic);
        }
        break;

    case MQTT_EVENT_DATA: {
        bool handled = false;
        for (int i = 0; i < s_handler_count; i++) {
            if (event->topic_len == strlen(s_handlers[i].topic) &&
                memcmp(event->topic, s_handlers[i].topic, event->topic_len) == 0) {
                s_handlers[i].handler(s_handlers[i].topic,
                                      (const uint8_t *)event->data,
                                      event->data_len);
                handled = true;
                break;
            }
        }
        if (!handled) {
            ESP_LOGI(TAG, "RX %d bytes on gdb/cmd", event->data_len);
            for (int i = 0; i < event->data_len; i++) {
                uint8_t byte = event->data[i];
                if (xQueueSend(s_rx_queue, &byte, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "RX queue full, dropped byte");
                }
            }
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

static void mqtt_tx_task(void *arg)
{
    tx_msg_t msg;

    while (1) {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            int mid = esp_mqtt_client_publish(s_client, s_resp_topic,
                                    (const char *)msg.data, msg.len, 1, 0);
            ESP_LOGI(TAG, "TX %d bytes on resp (mid=%d)", (int)msg.len, mid);
        }
    }
}

esp_err_t mqtt_transport_init(const char *broker_uri, const char *device_id)
{
    snprintf(s_cmd_topic, sizeof(s_cmd_topic),
             "device/%s/gdb/cmd", device_id);
    snprintf(s_resp_topic, sizeof(s_resp_topic),
             "device/%s/gdb/resp", device_id);

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(uint8_t));
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_msg_t));
    if (!s_rx_queue || !s_tx_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    xTaskCreate(mqtt_tx_task, "mqtt_tx", TX_TASK_STACK,
                NULL, tskIDLE_PRIORITY + 2, NULL);

    ESP_LOGI(TAG, "MQTT transport initialized (device: %s)", device_id);
    return ESP_OK;
}

void mqtt_transport_send(const uint8_t *data, size_t len)
{
    while (len > 0) {
        tx_msg_t msg;
        msg.len = (len > TX_MSG_MAX) ? TX_MSG_MAX : len;
        memcpy(msg.data, data, msg.len);
        xQueueSend(s_tx_queue, &msg, portMAX_DELAY);
        data += msg.len;
        len -= msg.len;
    }
}

int mqtt_transport_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    uint8_t byte;

    if (xQueueReceive(s_rx_queue, &byte, ticks) != pdTRUE) {
        return 0;
    }

    buf[0] = byte;
    size_t count = 1;

    while (count < max_len) {
        if (xQueueReceive(s_rx_queue, &byte, 0) != pdTRUE) {
            break;
        }
        buf[count++] = byte;
    }
    return count;
}

esp_err_t mqtt_transport_register_handler(const char *topic, mqtt_msg_handler_t handler)
{
    if (s_handler_count >= MAX_HANDLERS)
        return ESP_ERR_NO_MEM;

    strlcpy(s_handlers[s_handler_count].topic, topic, sizeof(s_handlers[0].topic));
    s_handlers[s_handler_count].handler = handler;
    s_handler_count++;

    esp_mqtt_client_subscribe(s_client, topic, 1);
    ESP_LOGI(TAG, "Registered handler for %s", topic);
    return ESP_OK;
}

esp_err_t mqtt_transport_publish(const char *topic, const char *data, int len)
{
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, len, 1, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
