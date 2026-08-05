#ifndef STATUS_MACHINE_H
#define STATUS_MACHINE_H

#include <stdint.h>
#include "rid_messages.h"

// ======================== 状态机 (纯函数) ========================
//
// 运行状态转换决策逻辑与 I/O 副作用分离，可独立宿主单元测试。
// 只有 statusStep() 是决策核心 (无 I/O、无日志、无 FreeRTOS 依赖)，
// 副作用 (广播启停、LED、日志) 由调用方依据返回结果执行。
//
// 消抖规则 (与历史行为严格一致):
//   1. 候选 == 已确认 → 重置消抖 (状态一致, 无需切换)
//   2. 紧急/失效 (3/4/5) → 绕过消抖立即切换
//   3. 候选首次出现 → 记录消抖起点
//   4. 持续确认达到阈值 → 切换
// 阈值: 地面→空中 300ms; 空中→地面 500ms (更严格, 防飞行中误停广播)

// 消抖时长 (ms) — 时间戳驱动, 与主循环负载解耦
static constexpr uint32_t DEBOUNCE_MS_GND_AIR = 300;  // 地面→空中
static constexpr uint32_t DEBOUNCE_MS_AIR_GND = 500;  // 空中→地面 (更严格)

// 消抖状态 (调用方持久持有, 每周期传入)
struct DebounceState {
    uint8_t  confirmed = 0xFF;  // 当前确认的运行状态
    uint8_t  target    = 0xFF;  // 候选目标状态 (消抖中)
    uint64_t startMs   = 0;     // 候选目标状态首次出现时刻 (消抖计时起点)
};

// statusStep() 返回结果 — 调用方据此执行副作用
enum class StatusStepResult {
    UNCHANGED,  // 状态一致 → 消抖已重置, 无需动作
    EMERGENCY,  // 紧急/失效 → 立即切换, 调用方应 applyStatusChange(st.confirmed)
    PENDING,    // 消抖进行中, 继续等待
    DEBOUNCED,  // 消抖完成 → 调用方应 applyStatusChange(st.confirmed)
};

inline bool statusIsEmergency(uint8_t s) {
    return (s == STATUS_EMERGENCY || s == STATUS_FAIL_SAFE || s == STATUS_FAIL_EMERG);
}

// 状态机单步决策。入参: 当前消抖状态 st, 本周期候选状态 candidate, 墙钟时间 nowMs。
// 出参: 更新后的 st 以及决策结果。
inline StatusStepResult statusStep(DebounceState& st, uint8_t candidate, uint64_t nowMs) {
    // 1. 状态一致 — 重置消抖
    if (candidate == st.confirmed) {
        st.target = 0xFF;
        st.startMs = 0;
        return StatusStepResult::UNCHANGED;
    }

    // 2. 紧急/失效状态绕过消抖，立即切换
    if (statusIsEmergency(candidate)) {
        st.target = 0xFF;
        st.startMs = 0;
        st.confirmed = candidate;
        return StatusStepResult::EMERGENCY;
    }

    // 3. 目标状态首次出现时记录起点
    if (candidate != st.target) {
        st.target = candidate;
        st.startMs = nowMs;
        return StatusStepResult::PENDING;
    }

    // 4. 持续确认到指定时长才切换
    uint32_t debounceMs = (st.target == STATUS_GROUND)
        ? DEBOUNCE_MS_AIR_GND   // 空中→地面: 更严格, 防止飞行中误停广播
        : DEBOUNCE_MS_GND_AIR;  // 地面→空中: 标准阈值
    if (nowMs - st.startMs >= debounceMs) {
        st.confirmed = candidate;
        st.target = 0xFF;
        st.startMs = 0;
        return StatusStepResult::DEBOUNCED;
    }
    return StatusStepResult::PENDING;
}

#endif // STATUS_MACHINE_H
