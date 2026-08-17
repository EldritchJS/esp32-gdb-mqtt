#pragma once

#include "esp_err.h"

#define CFG_MAX_SSID     32
#define CFG_MAX_PASSWORD 64
#define CFG_MAX_URI      128
#define CFG_MAX_DEVICE_ID 32

typedef struct {
    char wifi_ssid[CFG_MAX_SSID];
    char wifi_password[CFG_MAX_PASSWORD];
    char mqtt_uri[CFG_MAX_URI];
    char device_id[CFG_MAX_DEVICE_ID];
} device_config_t;

esp_err_t device_config_init(void);
const device_config_t *device_config_get(void);
