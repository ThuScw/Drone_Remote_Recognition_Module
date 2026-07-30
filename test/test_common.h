#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Controllable test clock for esp_timer_get_time()
extern uint64_t g_test_now_us;

// Minimal test framework
extern int g_passed;
extern int g_failed;

#define CHECK(expr) do { \
    if (expr) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (_va == _vb) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "  FAIL [%s:%d] %s == %s  (%d vs %d)\n", \
            __FILE__, __LINE__, #a, #b, (int)_va, (int)_vb); } \
} while(0)

#define CHECK_CLOSE(a, b, eps) do { \
    double _va = (double)(a); double _vb = (double)(b); \
    if (fabs(_va - _vb) <= (eps)) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "  FAIL [%s:%d] |%s - %s| <= %s  (%.6f vs %.6f)\n", \
            __FILE__, __LINE__, #a, #b, #eps, _va, _vb); } \
} while(0)
