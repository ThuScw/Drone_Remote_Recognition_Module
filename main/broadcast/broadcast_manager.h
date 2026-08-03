#ifndef BROADCAST_MANAGER_H
#define BROADCAST_MANAGER_H

#include <stdint.h>
#include "rid_messages.h"
#include "ble_rid_broadcaster.h"
#include "flight_log.h"
#include "indicators.h"
#include "status_machine.h"

// ======================== RIDBroadcastManager ========================
//
// 广播编排器 — 封装所有远程识别广播逻辑:
//   - 数据验证与校验
//   - 数据包构建 (P0: 始终构建新包，缺失字段编码为 unknown)
//   - 广播调度 (P0: 永不跳过广播，满足 ≤1s 间隔要求)
//   - 状态机管理 (地面→空中→紧急→失效)
//   - BLE 自修复 (三级递进，合并为单一方法)
//   - 飞行日志记录 (GB 46750-2025 5.1.8)
//   - 运行时自检 (GB 42590-2023 A.2.3.5.5)
//   - 堆内存监控 (P1)
//
// 主循环只需: getFlightData() → manager.update(fd, nowMs)

class RIDBroadcastManager {
public:
    RIDBroadcastManager(
        BleRidBroadcaster& broadcaster,
        FlightLog& flightLog,
        StatusLed& statusLed);

    // 配置校验、BLE 自检
    bool init();

    // 主循环每次迭代调用 (~10ms 间隔)
    // fd: 本周期从数据源获取的飞行数据
    // nowMs: 当前系统时间 (ms)
    void update(const FlightData& fd, uint64_t nowMs);

    bool isBroadcasting() const { return _broadcastActive; }

private:
    // --- 内部方法 ---
    void handleBleRecovery();
    void validateAndBuildPacket(const FlightData& fd, uint64_t nowMs);
    void handleStatusTransition(uint64_t nowMs);
    void handleBroadcast(uint64_t nowMs);
    void handleFlightLog(uint64_t nowMs);
    void handleSelfTest();
    void handleHeapMonitor(uint64_t nowMs);

    bool isAirborne() const;
    void triggerSelfHeal();
    void applyStatusChange(uint8_t newStatus);  // 执行广播启停 + LED 状态切换

    // --- 引用的外部模块 ---
    BleRidBroadcaster& _broadcaster;
    FlightLog&         _flightLog;
    StatusLed&         _statusLed;

    // --- 内部状态 ---
    GB46750Packet _currentPacket;
    FlightData    _lastValidData;
    bool          _broadcastActive;

    // 状态消抖状态机 (决策逻辑在 status_machine.h 的 statusStep(), 可宿主测试)
    DebounceState _debounce;

    // --- 定时器 ---
    uint64_t _nextBroadcastMs;        // 下一次广播时刻 (绝对时隙, 相位累加防漂移)
    uint64_t _lastBroadcastSuccessMs; // 最近一次广播数据实际更新成功时间 (合规监测)
    uint64_t _lastDataUpdateMs;
    uint64_t _lastSelfTestMs;
    uint64_t _lastFlightLogMs;
    uint64_t _lastHeapCheckMs;

    // --- 计数器 ---
    uint32_t _broadcastCount;
    uint32_t _validationFailCount;

    // --- 故障记录节流 (同型持续故障只在"进入故障"时记一次, 防环形缓冲被淹没) ---
    bool _rangeBad = false;      // 上一周期范围校验是否失败
    bool _staleReported = false; // 当前广播周期是否已记录过期事件
};

#endif // BROADCAST_MANAGER_H
