#pragma once

#include <stdint.h>

// Test-only config — provides constants referenced by the modules under test
#define CONFIG_RID_VERBOSE_LOG 0

// For mavlink_parser stub usage
#define MAVLINK_CONSECUTIVE_CRC_LIMIT 200
#define FC_DATA_TIMEOUT_MS            2000
#define USB_RECOVERY_COOLDOWN_MS      5000

// For flight_log stub usage
#define FLIGHT_LOG_INTERVAL_S       10
#define FLIGHT_LOG_PARTITION        "flight_log"
#define FLIGHT_LOG_TASK_STACK       3072
#define FLIGHT_LOG_TASK_PRIO        1
#define FLIGHT_LOG_QUEUE_DEPTH      16
#define FLIGHT_LOG_WRITE_TIMEOUT_MS 100
#define FLIGHT_LOG_MAGIC            0x5249444C

// Unused stubs — needed only because the real config.h includes these
// These are defined empty so the compiler doesn't complain
typedef int gpio_num_t;
#define GPIO_NUM_48  48
#define GPIO_NUM_6   6
