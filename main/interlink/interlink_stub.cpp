#include <esp_log.h>
#include "interlink_stub.h"
#include "fault_log.h"

static const char* TAG = "INTERLINK";

// 测试阶段: 只记录, 不发送。量产真实后端实现后, 该通知应经
// UART MAVLink / DroneCAN 或飞控内部调用实际触达飞控。
void StubFcInterlink::notifyFault(InterlinkReason reason, bool airborne, uint64_t nowMs) {
    ESP_LOGE(TAG, "[STUB] FC interlink fault → reason=%d, airborne=%s, t=%llu ms (no real link yet)",
             (int)reason, airborne ? "yes" : "no", (unsigned long long)nowMs);
    faultLogRecord(FAULT_INTERLINK_TRIGGERED, nowMs);
}

void StubFcInterlink::notifyRecovered(uint64_t nowMs) {
    ESP_LOGI(TAG, "[STUB] FC interlink recovered, t=%llu ms (no real link yet)",
             (unsigned long long)nowMs);
}
