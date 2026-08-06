#pragma once
// Minimal esp_partition.h stub — a single data partition sized to the fake flash.
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
} esp_partition_type_t;

typedef enum {
    ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00,
    ESP_PARTITION_SUBTYPE_ANY = 0xff,
} esp_partition_subtype_t;

typedef struct {
    esp_partition_type_t   type;
    esp_partition_subtype_t subtype;
    uint32_t               address;
    uint32_t               size;
    uint32_t               erase_size;
    char                   label[17];
} esp_partition_t;

// Defined in test_flight_log.cpp — returns a partition whose size matches g_flash.
const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char* label);
