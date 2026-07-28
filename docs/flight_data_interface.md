# 飞控数据接口规范 (Flight Data Interface Contract)

## 概述

本文档定义了飞控（Flight Controller）与 RID 模块之间的数据接口契约。  
**任何飞控接入必须严格遵循此规范，否则无法保证合规性。**

---

## 接口函数签名

```cpp
void getFlightData(FlightData& fd, uint64_t nowMs);
```

### 参数说明

| 参数 | 类型 | 方向 | 说明 |
|------|------|------|------|
| `fd` | `FlightData&` | 输出 | 飞控必须填充此结构体 |
| `nowMs` | `uint64_t` | 输入 | 系统运行时间（毫秒），飞控可忽略（用真实时间戳替代） |

### 返回值

`void` — 不通过返回值传递错误，而是通过 `fd.validMask` 标记有效字段。

---

## FlightData 结构体定义

```cpp
struct FlightData {
    // --- 必填字段 (M) ---
    float lat, lon;          // 纬度/经度 (度) 范围: [-90, 90] / [-180, 180]
    float geoAlt;            // 大地高度 (m) 范围: [-1000, 10000]
    float speed;             // 地速 (m/s) 范围: [0, 6553.5]
    float heading;           // 航迹角 (度) 范围: [0, 360)
    uint8_t opStatus;        // 运行状态 取值: 0-5 (见下表)
    float opLat, opLon;      // 遥控站位置 (度) 范围: 同 lat/lon
    float opAlt;             // 遥控站高度 (m) 范围: [-1000, 10000]

    // --- 可选字段 (O) ---
    float baroAlt;           // 气压高度 (m) 范围: [-1000, 10000]
    float heightAgl;         // 相对高度 (m) 范围: [-9000, 9000]
    float vspeed;            // 垂直速度 (m/s) 范围: [-63.5, 63.5]

    // --- 元数据 ---
    uint32_t validMask;      // 字段有效标志位 (见 FlightDataField 枚举)

    // --- 时间戳 (飞控必须填充) ---
    uint64_t ts_pos;         // lat/lon 更新时间 (ms)
    uint64_t ts_geoAlt;      // geoAlt 更新时间 (ms)
    uint64_t ts_speed;       // speed 更新时间 (ms)
    uint64_t ts_heading;     // heading 更新时间 (ms)
    uint64_t ts_opStatus;    // opStatus 更新时间 (ms)
    uint64_t ts_opPos;       // opLat/opLon 更新时间 (ms)

    // --- 数据质量 (由 RID 模块自动计算，飞控无需填充) ---
    DataFreshness freshness; // 整体新鲜度
    uint32_t validationFlags; // 范围验证结果
};
```

---

## 运行状态码 (opStatus)

| 值 | 名称 | 含义 | 广播行为 |
|----|------|------|----------|
| 0 | STATUS_UNREPORTED | 未报告 | 不广播 |
| 1 | STATUS_GROUND | 地面 | 不广播 |
| 2 | STATUS_AIRBORNE | 空中 | 持续广播 (800ms) |
| 3 | STATUS_EMERGENCY | 紧急状态 | 持续广播 (800ms) |
| 4 | STATUS_FAIL_SAFE | 识别功能失效(非紧急) | 持续广播 (800ms) |
| 5 | STATUS_FAIL_EMERG | 识别功能失效(紧急) | 持续广播 (800ms) |

**飞控必须正确设置此字段**，RID 模块根据此值决定是否广播。

---

## validMask 字段标志位

飞控必须根据实际提供的数据设置 `validMask`：

```cpp
enum FlightDataField : uint32_t {
    FLD_POS        = 1 << 0,   // lat, lon (M)
    FLD_GEO_ALT    = 1 << 1,   // geoAlt (M)
    FLD_BARO_ALT   = 1 << 2,   // baroAlt (O)
    FLD_HEIGHT_AGL = 1 << 3,   // heightAgl (O)
    FLD_SPEED      = 1 << 4,   // speed (M)
    FLD_HEADING    = 1 << 5,   // heading (M)
    FLD_VSPEED     = 1 << 6,   // vspeed (O)
    FLD_OP_STATUS  = 1 << 7,   // opStatus (M)
    FLD_OP_POS     = 1 << 8,   // opLat, opLon (M)
    FLD_OP_ALT     = 1 << 9,   // opAlt (M)

    FLD_ALL_M      = FLD_POS | FLD_GEO_ALT | FLD_SPEED | FLD_HEADING
                   | FLD_OP_STATUS | FLD_OP_POS | FLD_OP_ALT,
};
```

**示例**：如果飞控提供了位置、高度、速度、航向、状态，则：
```cpp
fd.validMask = FLD_POS | FLD_GEO_ALT | FLD_SPEED | FLD_HEADING | FLD_OP_STATUS;
```

**注意**：所有 M 字段必须有效，否则 RID 模块将对应字段编码为 0（"未知或不可用"），但仍然构建并广播数据包。详见下方"异常处理"。

---

## 时间戳要求 (关键！)

### 为什么需要时间戳？

GB 46750-2025 要求广播数据具有**实时性**。RID 模块通过时间戳检测：
- **数据新鲜度** — 超过 2s 未更新的数据标记为"过期"
- **数据完整性** — 缺少时间戳的字段视为无效

### 飞控必须做什么？

**方案 A：使用系统时间**
```cpp
void getFlightData(FlightData& fd, uint64_t nowMs) {
    fd.lat = gps_latitude;
    fd.lon = gps_longitude;
    fd.ts_pos = nowMs;  // 使用传入的系统时间
    
    fd.geoAlt = gps_altitude;
    fd.ts_geoAlt = nowMs;
    
    // ... 其他字段同理
}
```

**方案 B：使用 GPS 时间戳（推荐）**
```cpp
void getFlightData(FlightData& fd, uint64_t nowMs) {
    fd.lat = gps_latitude;
    fd.lon = gps_longitude;
    fd.ts_pos = gps_rmc_timestamp_ms;  // 从 GPS RMC 语句提取的真实时间
    
    fd.geoAlt = gps_altitude;
    fd.ts_geoAlt = gps_rmc_timestamp_ms;
    
    // ... 其他字段同理
}
```

### 时间戳规则

1. **所有 M 字段必须有时间戳** — 缺少任何时间戳会导致该字段无效
2. **时间戳必须是单调递增** — 不能回退（GPS 周翻转除外）
3. **时间戳精度 ≤ 100ms** — GB 46750 Table 3-021 要求
4. **时间戳必须是毫秒级** — 单位：ms（Unix 时间戳或系统运行时间）

---

## 数据更新频率

### RID 模块的调用频率

```cpp
// main.cpp 主循环
if (nowMs - lastDataUpdateTime >= DATA_UPDATE_INTERVAL_MS) {  // 默认 1000ms
    lastDataUpdateTime = nowMs;
    getFlightData(currentFlightData, nowMs);
}
```

**飞控必须保证**：每次调用时，返回的数据是**最新的**（延迟 < 100ms）。

### 广播频率

- **地面**：不广播
- **空中**：每 800ms 广播一次（GB 46750 要求 ≤ 1s）
- **紧急**：每 800ms 广播一次

**飞控必须保证**：数据更新频率 ≥ 1Hz（每秒至少更新一次）。

---

## 异常处理

### 情况 1：GPS 信号丢失

```cpp
void getFlightData(FlightData& fd, uint64_t nowMs) {
    if (!gps_valid) {
        // 不设置 FLD_POS 标志
        fd.validMask &= ~FLD_POS;
        // 但必须设置其他字段
        fd.validMask |= FLD_GEO_ALT | FLD_SPEED | FLD_HEADING | FLD_OP_STATUS;
        return;
    }
    
    // GPS 有效，正常填充
    fd.lat = gps_latitude;
    fd.lon = gps_longitude;
    fd.validMask |= FLD_POS;
}
```

**RID 模块行为**：检测到 `FLD_POS` 未置位，将位置编码为 (0, 0)（"未知"），`dataId` 位仍为 1，数据包正常构建并广播。接收方看到 (0,0) 可判断 GPS 不可用。

### 情况 2：数据超范围

```cpp
void getFlightData(FlightData& fd, uint64_t nowMs) {
    fd.lat = 999.0f;  // 错误数据！
    fd.ts_pos = nowMs;
    
    // 飞控应检测并标记为无效
    if (fd.lat < -90.0f || fd.lat > 90.0f) {
        fd.validMask &= ~FLD_POS;  // 标记为无效
    }
}
```

**RID 模块行为**：范围验证失败，将对应字段编码为 0（"未知"），记录 `ESP_LOGW` 告警日志，数据包正常构建并广播。

### 情况 3：飞控死机

**飞控必须**：通过看门狗或其他机制检测死机，并通过 `opStatus` 通知 RID 模块：
```cpp
if (flight_controller_watchdog_timeout) {
    fd.opStatus = STATUS_FAIL_SAFE;  // 或 STATUS_FAIL_EMERG
    fd.ts_opStatus = nowMs;
}
```

**RID 模块行为**：检测到 `STATUS_FAIL_SAFE`，继续广播（告知地面站飞控失效）。

---

## 飞控接入 Checklist

### 硬件接口

- [ ] UART 连接（TX/RX/GND）
- [ ] 波特率配置（默认 115200）
- [ ] 数据帧格式确认（MAVLink / MSP / 自定义）

### 软件实现

- [ ] 实现 `getFlightData()` 函数
- [ ] 填充所有 M 字段（lat, lon, geoAlt, speed, heading, opStatus, opLat, opLon, opAlt）
- [ ] 为每个 M 字段设置时间戳
- [ ] 正确设置 `validMask`
- [ ] 范围验证（GPS、高度、速度）

### 状态同步

- [ ] 确认飞控状态映射（地面/空中/紧急）
- [ ] 确认 opStatus 取值（0-5）
- [ ] 确认联锁引脚逻辑（高/低电平有效）

### 数据质量

- [ ] 数据更新频率 ≥ 1Hz
- [ ] 数据延迟 < 100ms
- [ ] 时间戳精度 ≤ 100ms
- [ ] GPS 丢失时的降级策略

### 测试验证

- [ ] 串口日志检查（所有字段正确）
- [ ] BLE 抓包验证（nRF Connect）
- [ ] 数据新鲜度检查（无 STALE 告警）
- [ ] 连续运行 1h 无内存泄漏

---

## 示例实现（UART 解析 MAVLink）

```cpp
#include "flight_data.h"
#include <driver/uart.h>
#include <mavlink.h>

static mavlink_message_t msg;
static mavlink_status_t status;

void getFlightData(FlightData& fd, uint64_t nowMs) {
    // 从 UART 读取 MAVLink 消息
    uint8_t data[256];
    int len = uart_read_bytes(UART_NUM_1, data, sizeof(data), pdMS_TO_TICKS(100));
    
    for (int i = 0; i < len; i++) {
        if (mavlink_parse_char(MAVLINK_COMM_0, data[i], &msg, &status)) {
            if (msg.msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
                mavlink_global_position_int_t pos;
                mavlink_msg_global_position_int_decode(&msg, &pos);
                
                fd.lat = pos.lat / 1e7f;  // 1e-7 deg → deg
                fd.lon = pos.lon / 1e7f;
                fd.geoAlt = pos.alt / 1000.0f;  // mm → m
                fd.heightAgl = pos.relative_alt / 1000.0f;
                fd.vspeed = pos.vz / 100.0f;  // cm/s → m/s
                
                fd.validMask |= FLD_POS | FLD_GEO_ALT | FLD_HEIGHT_AGL | FLD_VSPEED;
                fd.ts_pos = fd.ts_geoAlt = fd.ts_opPos = nowMs;
            }
            else if (msg.msgid == MAVLINK_MSG_ID_GPS_RAW_INT) {
                mavlink_gps_raw_int_t gps;
                mavlink_msg_gps_raw_int_decode(&msg, &gps);
                
                fd.speed = gps.vel / 100.0f;  // cm/s → m/s
                fd.heading = gps.cog / 100.0f;  // centideg → deg
                
                fd.validMask |= FLD_SPEED | FLD_HEADING;
                fd.ts_speed = fd.ts_heading = nowMs;
            }
        }
    }
    
    // 操作员位置（固定或从遥控器获取）
    fd.opLat = 31.230500f;
    fd.opLon = 121.473800f;
    fd.opAlt = 10.0f;
    fd.validMask |= FLD_OP_POS | FLD_OP_ALT;
    fd.ts_opPos = nowMs;
    
    // 运行状态（根据高度判断）
    if (fd.heightAgl < 0.5f) {
        fd.opStatus = STATUS_GROUND;
    } else {
        fd.opStatus = STATUS_AIRBORNE;
    }
    fd.validMask |= FLD_OP_STATUS;
    fd.ts_opStatus = nowMs;
}
```

---

## 常见问题

### Q1: 如果飞控无法提供某个 M 字段怎么办？

**A**: 必须尽力提供。M 字段缺失时，RID 模块将该字段编码为 0（GB 46750 "未知或不可用"值），`dataId` 位仍为 1，数据包正常广播。这确保 Message Counter 每包自增，接收方始终看到最新状态。

如果某个字段确实无法获取（如无气压计），飞控应提供**估算值**或**默认值**，并设置 `validMask`。RID 模块会输出 `ESP_LOGW` 告警日志，但不会阻止广播。

### Q2: 时间戳可以用系统运行时间吗？

**A**: 可以，但必须单调递增。推荐使用 GPS RMC 时间戳（真实世界时间）。

### Q3: 数据更新频率可以低于 1Hz 吗？

**A**: 不可以。GB 46750 要求广播间隔 ≤ 1s，飞控数据更新必须 ≥ 1Hz。

### Q4: GPS 丢失时应该怎么做？

**A**: 不设置 `FLD_POS` 标志，位置和高度等字段编码为 0（"未知"），数据包仍正常广播。RID 模块会输出 `ESP_LOGW` 告警日志，接收方可根据 (0,0) 坐标判断 GPS 不可用。

### Q5: 如何验证飞控接口是否正确？

**A**: 
1. 串口日志检查（所有字段正确）
2. BLE 抓包（nRF Connect）
3. 数据新鲜度检查（无 STALE 告警）
4. 连续运行 1h 无内存泄漏

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.2 | 2026-07-27 | v2.3 同步：补充 CONFIG_RID_VERBOSE_LOG、BLE_ADV_INTERVAL_MS、DATA_FRESH_THRESHOLD_MS 等新增配置项；确认 struct 与枚举定义与 rid_messages.h 一致 |
| 1.1 | 2026-07-27 | P0 合规修复：M 字段缺失时编码为 0（未知），不再拒绝广播；架构重构为子目录结构 |
| 1.0 | 2026-07-27 | 初始版本（Stage 1 Mock 接口） |
