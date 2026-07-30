#pragma once

#include <stdint.h>

// Test-only config — provides constants referenced by the modules under test
#define CONFIG_RID_VERBOSE_LOG 0

// For mavlink_parser stub usage
#define MAVLINK_CONSECUTIVE_CRC_LIMIT 200
#define FC_DATA_TIMEOUT_MS            2000
#define USB_RECOVERY_COOLDOWN_MS      5000

// For flight_data default values (used by mavlink_fillFlightData paths)
#define MOCK_OP_LAT   31.230500f
#define MOCK_OP_LON   121.473800f
#define MOCK_OP_ALT   10.0f

// Unused stubs — needed only because the real config.h includes these
// These are defined empty so the compiler doesn't complain
typedef int gpio_num_t;
#define GPIO_NUM_48  48
#define GPIO_NUM_6   6
