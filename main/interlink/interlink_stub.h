#ifndef INTERLINK_STUB_H
#define INTERLINK_STUB_H

#include "fc_interlink.h"

// 测试阶段飞控交联后端 — 不实际向飞控发送任何指令。
// 仅记录日志 + 故障事件, 便于验证"失效通知"逻辑是否被正确触发。
// 量产形态确定后替换为真实后端:
//   A. 嵌入芯片 → UART (MAVLink 指令 / DroneCAN) 后端
//   B. 合并进飞控固件 → 直接调用飞控内部安全接口
class StubFcInterlink : public IFcInterlink {
public:
    void notifyFault(InterlinkReason reason, bool airborne, uint64_t nowMs) override;
    void notifyRecovered(uint64_t nowMs) override;
};

#endif // INTERLINK_STUB_H
