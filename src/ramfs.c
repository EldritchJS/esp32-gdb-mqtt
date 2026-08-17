#include "ramfs.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "ramfs";

static ramfs_file_t s_files[RAMFS_MAX_FILES];
static SemaphoreHandle_t s_mutex;

void ramfs_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(s_files, 0, sizeof(s_files));
}

esp_err_t ramfs_store(const char *name, const uint8_t *data, size_t size)
{
    if (!name || !name[0] || strlen(name) >= RAMFS_MAX_FILENAME)
        return ESP_ERR_INVALID_ARG;

    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for %zu bytes", size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, data, size);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (s_files[i].name[0] && strcmp(s_files[i].name, name) == 0) {
            free(s_files[i].data);
            s_files[i].data = buf;
            s_files[i].size = size;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "updated %s (%zu bytes)", name, size);
            return ESP_OK;
        }
    }

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!s_files[i].name[0]) {
            strlcpy(s_files[i].name, name, RAMFS_MAX_FILENAME);
            s_files[i].data = buf;
            s_files[i].size = size;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "stored %s (%zu bytes)", name, size);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    free(buf);
    ESP_LOGE(TAG, "no free slots");
    return ESP_ERR_NO_MEM;
}

const ramfs_file_t *ramfs_find(const char *name)
{
    if (!name)
        return NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (s_files[i].name[0] && strcmp(s_files[i].name, name) == 0) {
            xSemaphoreGive(s_mutex);
            return &s_files[i];
        }
    }
    xSemaphoreGive(s_mutex);
    return NULL;
}

esp_err_t ramfs_delete(const char *name)
{
    if (!name)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (s_files[i].name[0] && strcmp(s_files[i].name, name) == 0) {
            free(s_files[i].data);
            s_files[i].data = NULL;
            s_files[i].size = 0;
            s_files[i].name[0] = '\0';
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "deleted %s", name);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

int ramfs_list(const ramfs_file_t **entries, int max_entries)
{
    int count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < RAMFS_MAX_FILES && count < max_entries; i++) {
        if (s_files[i].name[0]) {
            entries[count++] = &s_files[i];
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}
