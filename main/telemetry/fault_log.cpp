#include "fault_log.h"
#include <cstdio>

// 单任务访问 (主循环上下文), 无需锁 — 见 fault_log.h 注释。

static uint32_t s_counts[FAULT_EVENT_COUNT] = {0};
static uint16_t s_ring[FAULT_RING_MAX];
static uint64_t s_ringTs[FAULT_RING_MAX];
static uint16_t s_ringHead = 0;   // 下一个写入位置
static uint16_t s_ringLen = 0;    // 已记录事件数

static const char* kNames[FAULT_EVENT_COUNT] = {
    "CRC_STORM", "BLE_OK", "BLE_DEG", "BLE_FAIL",
    "VAL_FAIL", "STALE_TX", "LOW_HEAP", "INTERLINK"
};

void faultLogRecord(FaultEvent ev, uint64_t nowMs) {
    if (ev >= FAULT_EVENT_COUNT) return;
    s_counts[ev]++;
    s_ring[s_ringHead] = (uint16_t)ev;
    s_ringTs[s_ringHead] = nowMs;
    s_ringHead = (uint16_t)((s_ringHead + 1) % FAULT_RING_MAX);
    if (s_ringLen < FAULT_RING_MAX) s_ringLen++;
}

void faultLogFormat(char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) return;
    size_t n = 0;
    int w = snprintf(buf + n, bufSize - n, "faults:");
    if (w > 0) n += (size_t)w;

    for (int i = 0; i < FAULT_EVENT_COUNT; i++) {
        if (s_counts[i] == 0) continue;
        w = snprintf(buf + n, bufSize - n, " %s=%lu", kNames[i], (unsigned long)s_counts[i]);
        if (w <= 0) return;
        n += (size_t)w;
        if (n >= bufSize) return;
    }

    if (s_ringLen > 0) {
        w = snprintf(buf + n, bufSize - n, " last:");
        if (w <= 0) return;
        n += (size_t)w;

        // 环形遍历: 满时从 s_ringHead (最旧) 起, 未满时从 0 起
        uint16_t start = (s_ringLen == FAULT_RING_MAX) ? s_ringHead : 0;
        for (uint16_t k = 0; k < s_ringLen; k++) {
            uint16_t idx = (uint16_t)((start + k) % FAULT_RING_MAX);
            w = snprintf(buf + n, bufSize - n, " t%llu:%s",
                         (unsigned long long)s_ringTs[idx], kNames[s_ring[idx]]);
            if (w <= 0) return;
            n += (size_t)w;
            if (n >= bufSize) return;
        }
    }
}
