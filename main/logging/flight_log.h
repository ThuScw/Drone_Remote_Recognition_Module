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
    uint32_t getMaxRecords() const { return _partitionSize / kRecordSize; }
    uint32_t getWriteOffset() const { return _writeOffset; }
    float estimateRemainingHours() const;

    // 读取单条记录（index: 0=最旧, N-1=最新）
    // 返回读取字节数（kRecordSize 成功, 0 失败/越界）
    uint16_t readRecord(uint32_t index, uint8_t* outData, uint16_t* outLen, uint64_t* outTimestampMs);

    // 读取单条记录完整原始 kRecordSize 字节（index: 0=最旧, N-1=最新）
    // 校验 magic + CRC 后整体拷贝到 outBuf，供 DUMP 原样导出。
    // 返回读取字节数（kRecordSize 成功, 0 失败/越界/校验不通过）
    uint16_t readRecordRaw(uint32_t index, uint8_t* outBuf);

    // 读取最新一条记录
    uint16_t readLatestRecord(uint8_t* outData, uint16_t* outLen, uint64_t* outTimestampMs);

    // 记录格式 (128B): 4B magic + 2B CRC + 8B timestamp + 2B len + 80B payload + 32B 填充零
    // kRecordSize=128 为 4096B 扇区整数因子 (4096/128=32): 记录永不跨扇区边界,
    // erase-on-new-sector 逻辑即可消除跨扇区擦除导致的记录损坏 (旧 96B: 4096/96=42.67
    // 非整数, 每 43 条约 1 条尾部被下一扇区擦除抹掉)。
    // 公开: console_cmd 的 DUMP 导出用 kRecordSize 声明缓冲区, 避免硬编码漂移。
    static constexpr uint32_t kMagic       = 0x5249444C;  // "RIDL"
    static constexpr uint16_t kRecordSize  = 128;
    static constexpr uint16_t kMaxDataLen  = 80;

private:

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
    uint32_t      _sectorSize    = 0;        // flash 扇区大小 (wear_levelling)
    uint32_t      _writeOffset   = 0;
    uint32_t      _recordCount   = 0;
    uint32_t      _currentSector = 0xFFFFFFFF; // 已擦除的扇区号; 哨兵=尚未擦除
    bool          _sectorDirty   = true;     // 写指针所在扇区含旧数据, 写入前需整扇区擦除
    uint64_t      _lastWriteMs   = 0;

    portMUX_TYPE  _spinlock      = portMUX_INITIALIZER_UNLOCKED;

    TaskHandle_t  _taskHandle    = nullptr;
    QueueHandle_t _queue         = nullptr;
};

#endif
