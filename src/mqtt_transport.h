#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef void (*mqtt_msg_handler_t)(const char *topic, const uint8_t *data, size_t len);

esp_err_t mqtt_transport_init(const char *broker_uri, const char *device_id);
void mqtt_transport_send(const uint8_t *data, size_t len);
int mqtt_transport_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms);
esp_err_t mqtt_transport_register_handler(const char *topic, mqtt_msg_handler_t handler);
esp_err_t mqtt_transport_publish(const char *topic, const char *data, int len);
