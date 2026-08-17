#include "file_manager.h"
#include "ramfs.h"
#include "mqtt_transport.h"
#include "device_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "file-mgr";

#define CMD_QUEUE_DEPTH 4
#define CMD_MSG_MAX 512
#define DOWNLOAD_TASK_STACK 8192

#ifndef CONFIG_FILE_MANAGER_MAX_FILE_SIZE
#define CONFIG_FILE_MANAGER_MAX_FILE_SIZE 65536
#endif

typedef struct {
    uint8_t data[CMD_MSG_MAX];
    size_t len;
} cmd_msg_t;

static char s_cmd_topic[64];
static char s_resp_topic[64];
static QueueHandle_t s_cmd_queue;

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static void publish_response(const char *json_str)
{
    mqtt_transport_publish(s_resp_topic, json_str, strlen(json_str));
}

static void publish_error(const char *action, const char *message)
{
    char resp[256];
    if (action) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"action\":\"%s\",\"message\":\"%s\"}",
                 action, message);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"%s\"}", message);
    }
    publish_response(resp);
}

static void handle_download(const char *json)
{
    char url[256];
    char name[RAMFS_MAX_FILENAME];

    if (!extract_json_string(json, "url", url, sizeof(url)) ||
        !extract_json_string(json, "name", name, sizeof(name))) {
        publish_error("download", "missing url or name");
        return;
    }

    ESP_LOGI(TAG, "downloading %s -> %s", url, name);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        publish_error("download", "http client init failed");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        publish_error("download", "http connect failed");
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        char msg[48];
        snprintf(msg, sizeof(msg), "HTTP %d", status);
        publish_error("download", msg);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    size_t max_size = CONFIG_FILE_MANAGER_MAX_FILE_SIZE;
    if (content_length > (int)max_size) {
        publish_error("download", "file too large");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    size_t buf_size = (content_length > 0) ? (size_t)content_length : 4096;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        publish_error("download", "out of memory");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    size_t total = 0;
    while (1) {
        int rd = esp_http_client_read(client, (char *)(buf + total), buf_size - total);
        if (rd < 0) {
            ESP_LOGE(TAG, "http read error");
            free(buf);
            publish_error("download", "http read error");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return;
        }
        if (rd == 0)
            break;

        total += rd;

        if (total >= buf_size && content_length <= 0) {
            if (buf_size >= max_size) {
                free(buf);
                publish_error("download", "file too large");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return;
            }
            size_t new_size = buf_size * 2;
            if (new_size > max_size)
                new_size = max_size;
            uint8_t *new_buf = realloc(buf, new_size);
            if (!new_buf) {
                free(buf);
                publish_error("download", "out of memory");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return;
            }
            buf = new_buf;
            buf_size = new_size;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    err = ramfs_store(name, buf, total);
    free(buf);

    if (err != ESP_OK) {
        publish_error("download", "ramfs store failed");
        return;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"action\":\"download\",\"name\":\"%s\",\"size\":%zu}",
             name, total);
    publish_response(resp);

    ESP_LOGI(TAG, "downloaded %s (%zu bytes)", name, total);
}

static void handle_delete(const char *json)
{
    char name[RAMFS_MAX_FILENAME];
    if (!extract_json_string(json, "name", name, sizeof(name))) {
        publish_error("delete", "missing name");
        return;
    }

    esp_err_t err = ramfs_delete(name);
    if (err == ESP_ERR_NOT_FOUND) {
        publish_error("delete", "file not found");
        return;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"action\":\"delete\",\"name\":\"%s\"}", name);
    publish_response(resp);
}

static void handle_list(void)
{
    const ramfs_file_t *entries[RAMFS_MAX_FILES];
    int count = ramfs_list(entries, RAMFS_MAX_FILES);

    char resp[512];
    int pos = snprintf(resp, sizeof(resp),
                       "{\"status\":\"ok\",\"action\":\"list\",\"files\":[");
    for (int i = 0; i < count && pos < (int)sizeof(resp) - 64; i++) {
        if (i > 0)
            resp[pos++] = ',';
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "{\"name\":\"%s\",\"size\":%zu}",
                        entries[i]->name, entries[i]->size);
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
    publish_response(resp);
}

static void file_manager_task(void *arg)
{
    cmd_msg_t msg;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        msg.data[msg.len < CMD_MSG_MAX ? msg.len : CMD_MSG_MAX - 1] = '\0';
        const char *json = (const char *)msg.data;

        char action[16];
        if (!extract_json_string(json, "action", action, sizeof(action))) {
            publish_error(NULL, "missing action");
            continue;
        }

        if (strcmp(action, "download") == 0) {
            handle_download(json);
        } else if (strcmp(action, "delete") == 0) {
            handle_delete(json);
        } else if (strcmp(action, "list") == 0) {
            handle_list();
        } else {
            publish_error(action, "unknown action");
        }
    }
}

static void file_cmd_handler(const char *topic, const uint8_t *data, size_t len)
{
    if (len >= CMD_MSG_MAX) {
        ESP_LOGW(TAG, "command too large (%zu bytes), dropping", len);
        return;
    }
    cmd_msg_t msg;
    memcpy(msg.data, data, len);
    msg.len = len;
    xQueueSend(s_cmd_queue, &msg, 0);
}

esp_err_t file_manager_init(void)
{
    const device_config_t *cfg = device_config_get();
    snprintf(s_cmd_topic, sizeof(s_cmd_topic),
             "device/%s/file/cmd", cfg->device_id);
    snprintf(s_resp_topic, sizeof(s_resp_topic),
             "device/%s/file/resp", cfg->device_id);

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(cmd_msg_t));
    if (!s_cmd_queue)
        return ESP_ERR_NO_MEM;

    esp_err_t err = mqtt_transport_register_handler(s_cmd_topic, file_cmd_handler);
    if (err != ESP_OK)
        return err;

    xTaskCreate(file_manager_task, "file_mgr", DOWNLOAD_TASK_STACK,
                NULL, tskIDLE_PRIORITY + 1, NULL);

    ESP_LOGI(TAG, "File manager initialized (cmd: %s, resp: %s)",
             s_cmd_topic, s_resp_topic);
    return ESP_OK;
}
