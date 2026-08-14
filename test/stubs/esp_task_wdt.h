#pragma once
// Stub: no task watchdog on host — always succeed.
#include "esp_err.h"

inline esp_err_t esp_task_wdt_add(void* task) {
    (void)task;
    return ESP_OK;
}

inline esp_err_t esp_task_wdt_reset() {
    return ESP_OK;
}
