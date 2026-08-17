#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define RAMFS_MAX_FILES    8
#define RAMFS_MAX_FILENAME 32

typedef struct {
    char name[RAMFS_MAX_FILENAME];
    uint8_t *data;
    size_t size;
} ramfs_file_t;

void ramfs_init(void);
esp_err_t ramfs_store(const char *name, const uint8_t *data, size_t size);
const ramfs_file_t *ramfs_find(const char *name);
esp_err_t ramfs_delete(const char *name);
int ramfs_list(const ramfs_file_t **entries, int max_entries);
