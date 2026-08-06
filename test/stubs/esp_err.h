#pragma once
// Minimal esp_err.h stub — production error codes reduced to what tests need.
typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
