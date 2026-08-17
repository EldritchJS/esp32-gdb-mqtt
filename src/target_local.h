#pragma once

#include "target_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const target_ops_t target_local_ops;

void target_local_set_excluded_task(TaskHandle_t handle);
