#pragma once

#include <stdint.h>

// Controllable test clock — set before each test
extern uint64_t g_test_now_us;

inline uint64_t esp_timer_get_time() {
    return g_test_now_us;
}
