#ifndef FLIGHT_LOG_H
#define FLIGHT_LOG_H

#include <stdint.h>
#include "wear_levelling.h"

// GB 46750-2025 5.1.8: 滚动存储运行识别信息
//   - 更新间隔 ≤10s
//   - 存储容量 ≥120 飞行小时
//   - 不可手动删除（无文件系统暴露，仅通过专用接口读取）
//   - 使用 wear_leveling 组件均衡 Flash 磨损
//   - 每条记录含 CRC16，上电时检测掉电损坏

class FlightLog {
public:
    bool init();

    // 写入一条飞行记录，返回实际写入字节数，失败返回 0
    uint16_t writeRecord(const uint8_t* data, uint16_t len, uint64_t timestampMs);

    uint32_t getRecordCount() const { return _recordCount; }
    uint32_t getWriteOffset() const { return _writeOffset; }

    // 估算剩余容量可支撑的飞行小时数
    float estimateRemainingHours() const;

private:
    static constexpr uint32_t kMagic       = 0x5249444C;  // "RIDL"
    static constexpr uint16_t kRecordSize  = 96;
    static constexpr uint16_t kMaxDataLen  = 80;

    // CRC-16-CCITT (poly=0x1021)
    static uint16_t crc16(const uint8_t* data, size_t len);

    wl_handle_t _wlHandle      = WL_INVALID_HANDLE;
    uint32_t    _partitionSize = 0;
    uint32_t    _writeOffset   = 0;    // 逻辑写入偏移
    uint32_t    _recordCount   = 0;
    uint64_t    _lastWriteMs   = 0;
};

#endif
