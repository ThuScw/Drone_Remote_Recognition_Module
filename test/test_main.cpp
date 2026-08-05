// Minimal test runner
#include "test_common.h"

int g_passed = 0;
int g_failed = 0;
uint64_t g_test_now_us = 0;

// Module entry points
void test_mavlink_crc();
void test_mavlink_parser();
void test_rid_messages();
void test_fault_log();
void test_status_machine();

int main() {
    printf("ESP32-S3 RID -- Host Test Suite\n\n");

    test_mavlink_crc();
    test_mavlink_parser();
    test_rid_messages();
    test_fault_log();
    test_status_machine();

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
