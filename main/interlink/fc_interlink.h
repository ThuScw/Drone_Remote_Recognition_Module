#ifndef FC_INTERLINK_H
#define FC_INTERLINK_H

#include <stdint.h>

// ======================== 飞控交联接口 (GB 46750-2025 5.1.7) ========================
//
// 5.1.7 要求运行识别发送模块与飞行控制功能模块互联:
//   a) 起飞前失效 → 应能阻止起飞
//   b) 飞行中失效 → 应能触发悬停/返航/降落
//
// 量产形态待定, 具体后端通过构造函数注入, 与状态机 / 广播逻辑解耦:
//   A. 独立模块嵌入机体 → 经 UART (MAVLink 指令 / DroneCAN) 向飞控发送
//   B. 合并进飞控固件     → 直接调用飞控内部安全接口
// 当前测试阶段注入 StubFcInterlink (见 interlink_stub.h), 仅记录不发送。

// 模块识别发送功能失效的触发源
enum class InterlinkReason : uint8_t {
    SELF_TEST_FAILED = 0,  // 运行时自检失败 (GB 42590-2023 A.2.3.5.5)
    BLE_HEAL_FAILED  = 1,  // BLE 三级自愈全部失败 → 识别发送功能失效
};

// 飞控交联抽象接口 — 模块 → 飞控的失效/恢复通知语义
class IFcInterlink {
public:
    virtual ~IFcInterlink() = default;

    // 模块识别发送功能失效 → 通知飞控安全处置。
    // airborne: true = 飞行中 (应悬停/返航/降落); false = 地面 (应禁止起飞)
    // nowMs: 触发时刻 (ms)
    virtual void notifyFault(InterlinkReason reason, bool airborne, uint64_t nowMs) = 0;

    // 模块识别功能恢复 → 通知飞控解除限制 (是否解除由飞控策略决定)
    virtual void notifyRecovered(uint64_t nowMs) = 0;
};

#endif // FC_INTERLINK_H
