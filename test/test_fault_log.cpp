// Fault log: counters + event ring buffer
#include "test_common.h"
#include "fault_log.h"
#include <cstring>

void test_fault_log() {
    printf("--- Fault Log ---\n");

    // ==== 1. Fresh state: no counts, no ring ====
    {
        char buf[384];
        faultLogFormat(buf, sizeof(buf));
        CHECK(strstr(buf, "faults:") != nullptr);
        CHECK(strstr(buf, "last:") == nullptr);
        CHECK(strstr(buf, "CRC_STORM") == nullptr);
    }

    // ==== 2. Counters accumulate; ring records recent events ====
    {
        faultLogRecord(FAULT_CRC_STORM, 1000);
        faultLogRecord(FAULT_CRC_STORM, 2000);
        faultLogRecord(FAULT_BLE_HEAL_OK, 3000);

        char buf[384];
        faultLogFormat(buf, sizeof(buf));
        CHECK(strstr(buf, "faults:") != nullptr);
        CHECK(strstr(buf, "CRC_STORM=2") != nullptr);   // counter aggregated
        CHECK(strstr(buf, "BLE_OK=1") != nullptr);
        CHECK(strstr(buf, "last:") != nullptr);

        // 顺序: 最旧在前 (t1000:CRC_STORM 在 t3000:BLE_OK 之前)
        const char* last = strstr(buf, "last:");
        const char* t1000 = strstr(buf, "t1000:CRC_STORM");
        const char* t3000 = strstr(buf, "t3000:BLE_OK");
        CHECK(t1000 != nullptr);
        CHECK(t3000 != nullptr);
        CHECK(last != nullptr && t1000 > last && t3000 > t1000);
    }

    // ==== 3. Ring overflow: 满 16 后最旧事件被挤出, 计数器保留 ====
    {
        for (int i = 0; i < FAULT_RING_MAX + 5; i++) {
            faultLogRecord(FAULT_VALIDATION, (uint64_t)(10000 + i));
        }

        char buf[512];
        faultLogFormat(buf, sizeof(buf));

        // CRC_STORM/BLE_OK 事件已被挤出环形缓冲 (被 VAL_FAIL 淹没), 但计数器保留
        const char* last = strstr(buf, "last:");
        CHECK(last != nullptr);
        CHECK(strstr(last, "CRC_STORM") == nullptr);
        CHECK(strstr(last, "BLE_OK") == nullptr);
        CHECK(strstr(last, "VAL_FAIL") != nullptr);
        CHECK(strstr(buf, "CRC_STORM=2") != nullptr);
        CHECK(strstr(buf, "VAL_FAIL=") != nullptr);
    }

    // ==== 4. 非法事件忽略, 不崩溃 ====
    {
        faultLogRecord((FaultEvent)255, 9999);
        char buf[512];
        faultLogFormat(buf, sizeof(buf));
        CHECK(strstr(buf, "faults:") != nullptr);
    }

    // ==== 5. 小缓冲区不越界 (截断 + NUL 结尾) ====
    {
        char tiny[4];
        faultLogFormat(tiny, sizeof(tiny));
        CHECK(tiny[sizeof(tiny) - 1] == '\0');
    }

    // ==== 6. INTERLINK 事件 (GB 46750 5.1.7 飞控交联失效通知) ====
    {
        faultLogRecord(FAULT_INTERLINK_TRIGGERED, 5000);
        char buf[384];
        faultLogFormat(buf, sizeof(buf));
        CHECK(strstr(buf, "INTERLINK=1") != nullptr);
        CHECK(strstr(buf, "t5000:INTERLINK") != nullptr);
    }
}
