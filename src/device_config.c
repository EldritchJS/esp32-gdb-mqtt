#include "device_config.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "dev_cfg"
#define PROMPT_TIMEOUT_MS 3000

static device_config_t s_config;

static void load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return;

    size_t len;

    len = sizeof(s_config.wifi_ssid);
    nvs_get_str(h, "ssid", s_config.wifi_ssid, &len);

    len = sizeof(s_config.wifi_password);
    nvs_get_str(h, "password", s_config.wifi_password, &len);

    len = sizeof(s_config.mqtt_uri);
    nvs_get_str(h, "mqtt_uri", s_config.mqtt_uri, &len);

    len = sizeof(s_config.device_id);
    nvs_get_str(h, "device_id", s_config.device_id, &len);

    nvs_close(h);
}

static void save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS for write");
        return;
    }

    nvs_set_str(h, "ssid", s_config.wifi_ssid);
    nvs_set_str(h, "password", s_config.wifi_password);
    nvs_set_str(h, "mqtt_uri", s_config.mqtt_uri);
    nvs_set_str(h, "device_id", s_config.device_id);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved to NVS");
}

static int try_getchar(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    int c = fgetc(stdin);
    fcntl(STDIN_FILENO, F_SETFL, flags);
    return c;
}

static int wait_for_enter(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int c = try_getchar();
        if (c == '\r' || c == '\n') {
            vTaskDelay(pdMS_TO_TICKS(50));
            while (try_getchar() != EOF) {}
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return -1;
}

static int read_line(char *buf, size_t max, int timeout_ms)
{
    size_t pos = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        int c = try_getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\n' || c == '\r') {
            buf[pos] = '\0';
            putchar('\n');
            vTaskDelay(pdMS_TO_TICKS(50));
            while (try_getchar() != EOF) {}
            return pos;
        }

        if (c == 0x08 || c == 0x7f) {
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (pos < max - 1) {
            buf[pos++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
    buf[pos] = '\0';
    return 0;
}

static void print_config(void)
{
    printf("\n--- Current config ---\n");
    printf("  ssid:      %s\n", s_config.wifi_ssid);
    printf("  password:  %s\n", s_config.wifi_password[0] ? "****" : "(empty)");
    printf("  mqtt_uri:  %s\n", s_config.mqtt_uri);
    printf("  device_id: %s\n", s_config.device_id);
    printf("----------------------\n");
}

static void run_config_prompt(void)
{
    char line[CFG_MAX_URI];

    print_config();
    printf("Press ENTER within 3s to configure, or wait to boot...\n");

    if (wait_for_enter(PROMPT_TIMEOUT_MS) != 0)
        return;

    bool changed = false;

    printf("\nWiFi SSID [%s]: ", s_config.wifi_ssid);
    fflush(stdout);
    if (read_line(line, sizeof(line), 30000) > 0) {
        strlcpy(s_config.wifi_ssid, line, sizeof(s_config.wifi_ssid));
        changed = true;
    }

    printf("\nWiFi Password: ");
    fflush(stdout);
    if (read_line(line, sizeof(line), 30000) > 0) {
        strlcpy(s_config.wifi_password, line, sizeof(s_config.wifi_password));
        changed = true;
    }

    printf("\nMQTT URI [%s]: ", s_config.mqtt_uri);
    fflush(stdout);
    if (read_line(line, sizeof(line), 30000) > 0) {
        strlcpy(s_config.mqtt_uri, line, sizeof(s_config.mqtt_uri));
        changed = true;
    }

    printf("\nDevice ID [%s]: ", s_config.device_id);
    fflush(stdout);
    if (read_line(line, sizeof(line), 30000) > 0) {
        strlcpy(s_config.device_id, line, sizeof(s_config.device_id));
        changed = true;
    }

    if (changed) {
        save_to_nvs();
        print_config();
        printf("Rebooting with new config...\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    printf("\nNo changes, continuing boot.\n");
}

esp_err_t device_config_init(void)
{
    strlcpy(s_config.wifi_ssid, CONFIG_GDB_MQTT_WIFI_SSID, sizeof(s_config.wifi_ssid));
    strlcpy(s_config.wifi_password, CONFIG_GDB_MQTT_WIFI_PASSWORD, sizeof(s_config.wifi_password));
    strlcpy(s_config.mqtt_uri, CONFIG_GDB_MQTT_BROKER_URI, sizeof(s_config.mqtt_uri));
    strlcpy(s_config.device_id, CONFIG_GDB_MQTT_DEVICE_ID, sizeof(s_config.device_id));

    load_from_nvs();
    run_config_prompt();

    return ESP_OK;
}

const device_config_t *device_config_get(void)
{
    return &s_config;
}
