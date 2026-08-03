#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include <stdint.h>
#include <stddef.h>

// 故障记录 — 运行时累计计数器 + 最近故障事件环形缓冲。
// 背景: 恢复动作已存在 (USB CRC 风暴重启 / BLE 三级自愈 / 校验失败构建哨兵包),
// 但"为何触发"此前只进日志行, 会话后无法复盘。本模块把触发原因记录下来,
// 供 console STATUS 命令实时读出, 定位"恢复→再失败"循环的根因。
//
// 存储: 仅 RAM, 掉电/复位即清零。掉电场景的复盘由飞行日志 + 串口日志承担;
// 持久化 (NVS) 属量产阶段考虑。
//
// 并发: 所有调用点均在主循环任务上下文 (flight_data 恢复 / broadcast_manager
// 各 handler), 单任务访问, 无需锁。若未来从其它任务调用, 需加互斥。

#define FAULT_RING_MAX 16

enum FaultEvent : uint8_t {
    FAULT_CRC_STORM = 0,     // MAVLink 连续 CRC 错误达阈值 → USB 恢复
    FAULT_BLE_HEAL_OK,       // BLE 自愈成功 (原地重启)
    FAULT_BLE_HEAL_DEGRADED, // BLE 自愈降级 (PHY 切换 / NimBLE 重初始化)
    FAULT_BLE_HEAL_FAILED,   // BLE 三级自愈全部失败 → FAULT
    FAULT_VALIDATION,        // 飞行数据范围校验失败 (构建哨兵包)
    FAULT_STALE_BROADCAST,   // 数据过期仍广播 (GB 要求持续发送, 记录告警)
    FAULT_LOW_HEAP,          // 堆低于告警阈值
    FAULT_EVENT_COUNT
};

// 记录一次故障 (计数器 +1, 事件入环形缓冲)。
// nowMs: 触发时刻 (ms)。同型持续故障建议仅在"进入故障"时调用一次, 避免环形缓冲被单类事件淹没。
void faultLogRecord(FaultEvent ev, uint64_t nowMs);

// 格式化故障摘要到 buf: "faults: CRC_STORM=2 BLE_OK=1 last: t1000:CRC_STORM ..."
void faultLogFormat(char* buf, size_t bufSize);

#endif // FAULT_LOG_H
