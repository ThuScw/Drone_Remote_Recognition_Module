#ifndef FLIGHT_LOG_H
#define FLIGHT_LOG_H

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "wear_levelling.h"

// GB 46750-2025 5.1.8: 滚动存储运行识别信息
//   - 更新间隔 ≤10s, 存储容量 ≥120 飞行小时
//   - 不可手动删除（无文件系统暴露，仅通过专用接口读取）
//   - 使用 wear_levelling 组件均衡 Flash 磨损
//   - 每条记录含 CRC16，上电时检测掉电损坏

class FlightLog {
public:
    FlightLog() = default;
    ~FlightLog();

    FlightLog(const FlightLog&) = delete;
    FlightLog& operator=(const FlightLog&) = delete;

    bool init();

    // 非阻塞入队 — 主循环调用, 将记录投递到日志任务异步写入
    // 队列满时丢弃并告警; 返回 true 表示入队成功
    bool enqueueRecord(const uint8_t* data, uint16_t len, uint64_t timestampMs);

    uint32_t getRecordCount() const { return _recordCount; }
    uint32_t getWriteOffset() const { return _writeOffset; }
    float estimateRemainingHours() const;

private:
    static constexpr uint32_t kMagic       = 0x5249444C;  // "RIDL"
    static constexpr uint16_t kRecordSize  = 96;
    static constexpr uint16_t kMaxDataLen  = 80;

    struct LogItem {
        uint8_t  data[kMaxDataLen];
        uint16_t len;
        uint64_t timestampMs;
    };

    static uint16_t crc16(const uint8_t* data, size_t len);

    // 实际 Flash 写入 (仅由日志任务调用)
    uint16_t writeRecord(const uint8_t* data, uint16_t len, uint64_t timestampMs);

    // FreeRTOS 日志任务入口
    static void logTaskFunc(void* param);
    void logTaskLoop();

    wl_handle_t   _wlHandle      = WL_INVALID_HANDLE;
    uint32_t      _partitionSize = 0;
    uint32_t      _writeOffset   = 0;
    uint32_t      _recordCount   = 0;
    uint64_t      _lastWriteMs   = 0;

    TaskHandle_t  _taskHandle    = nullptr;
    QueueHandle_t _queue         = nullptr;
};

#endif
