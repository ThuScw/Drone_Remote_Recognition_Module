#pragma once
// Stub: no real BT driver needed for tests

// These enum values are referenced by config.h via BLE_TX_POWER_LEVEL
typedef enum {
    ESP_PWR_LVL_P3 = 3,
    ESP_PWR_LVL_P6 = 6,
    ESP_PWR_LVL_P9 = 9,
} esp_ble_tx_power_level_t;

#define ESP_PWR_TYPE_ADV 0
