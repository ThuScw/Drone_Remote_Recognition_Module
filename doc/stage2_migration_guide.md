# Stage 2: 真实飞控数据接入指南

> 从 Stage 1 (Mock 数据) 迁移到 Stage 2 (真实 MAVLink 飞控数据)

---

## 软件变更概览

### 新增文件

| 文件 | 说明 |
|------|------|
| `main/data/mavlink_parser.h` | MAVLink v2 解析器接口定义 |
| `main/data/mavlink_parser.cpp` | MAVLink v2 解析器实现 (UART 接收 + 帧解析) |

### 修改文件

| 文件 | 变更内容 |
|------|---------|
| `main/config.h` | 新增 UART 配置区 (端口、引脚、波特率、缓冲区) |
| `main/data/flight_data.cpp` | 替换 Mock 实现为 UART 接收 + MAVLink 解析 |

### 新增文档

| 文件 | 说明 |
|------|------|
| `doc/hardware_connection_guide.md` | 飞控 UART 连接硬件指南 |
| `doc/stage2_migration_guide.md` | 本文档 |

---

## 架构变更

### Stage 1 (Mock)

```
getFlightData()
    │
    └──→ 基于 nowMs 的模拟数据生成
         - 65s 起降循环
         - 固定位置/高度/速度
```

### Stage 2 (真实飞控)

```
飞控 TELEM1 TX ──────→ ESP32-C5 GPIO4 (UART1 RX)
                              │
                              ▼
                    uart_rx_task (FreeRTOS 任务)
                    持续读取 UART 字节流
                              │
                              ▼
                    mavlink_parseByte()
                    MAVLink v2 帧解析状态机
                              │
                    ┌─────────┼─────────┐
                    ▼         ▼         ▼
              HEARTBEAT  GPS_RAW  GLOBAL_POS
              (状态)     (GPS)    (位置/速度)
                              │
                              ▼
                    mavlink_fillFlightData()
                    填充 FlightData 结构体
                              │
                              ▼
                    getFlightData() 返回
                              │
                              ▼
                    RIDBroadcastManager::update()
                    GB 46750-2025 编码 → BLE 广播
```

---

## 配置说明

### UART 配置 (config.h)

```c
// ================= UART 飞控数据接口 (Stage 2) =================

#define FC_UART_PORT_NUM      1        // UART1 (UART0 用于调试)
#define FC_UART_RX_GPIO       GPIO_NUM_4  // 接飞控 TELEM1 TX
#define FC_UART_TX_GPIO       -1       // 不使用 (单向接收)

#define FC_UART_BAUD_RATE     115200   // 必须与飞控一致!
#define FC_UART_RX_BUF_SIZE   1024
#define FC_UART_TX_BUF_SIZE   0

#define MAVLINK_PARSER_STACK  4096     // 解析任务栈
#define FC_DATA_TIMEOUT_MS    2000     // 数据超时阈值
```

### 关键参数说明

| 参数 | 作用 | 如何调整 |
|------|------|---------|
| `FC_UART_BAUD_RATE` | 串口波特率 | 必须与飞控 `SERIAL1_BAUD` 一致 |
| `FC_UART_RX_GPIO` | 接收引脚 | 可改为其他可用 GPIO |
| `FC_DATA_TIMEOUT_MS` | 数据超时阈值 | 默认 2000ms，超过此时间未收到数据标记 STALE |

---

## 数据字段映射

### MAVLink 消息 → FlightData 字段

| MAVLink 消息 | 字段 | FlightData 字段 | 说明 |
|--------------|------|----------------|------|
| GLOBAL_POSITION_INT | lat | `fd.lat` | 纬度 (度) |
| GLOBAL_POSITION_INT | lon | `fd.lon` | 经度 (度) |
| GLOBAL_POSITION_INT | alt | `fd.geoAlt` | 海拔高度 (m MSL) |
| GLOBAL_POSITION_INT | relative_alt | `fd.heightAgl` | 相对高度 (m AGL) |
| GLOBAL_POSITION_INT | vx, vy | `fd.speed` | 地速 = √(vx²+vy²) |
| GLOBAL_POSITION_INT | hdg | `fd.heading` | 航向角 (度) |
| GLOBAL_POSITION_INT | vz | `fd.vspeed` | 垂直速度 (m/s, 正=上升) |
| GPS_RAW_INT | fix_type | 内部检查 | fix ≥ 2 才输出数据 |
| GPS_RAW_INT | satellites_visible | 内部检查 | 卫星数 |
| HEARTBEAT | base_mode (bit 7) | `fd.opStatus` | armed → AIRBORNE |
| HEARTBEAT | system_status | `fd.opStatus` | EMERGENCY/CRITICAL |
| HOME_POSITION | lat, lon, alt | `fd.opLat/opLon/opAlt` | 操作员位置 |

---

## 编译与烧录

### 编译

```bash
cd Drone_Remote_Recognition_Module
idf.py build
```

### 烧录与监控

```bash
idf.py -p COMx flash monitor
# COMx 替换为实际串口号 (如 COM3)
```

### 预期启动日志

```
I (xxx) SYS: === ESP32-C5 RID Broadcaster — GB 46750-2025 ===
I (xxx) SYS: Light show drone | BLE5 Extended Advertising | Real FC data
I (xxx) FLIGHT_DATA: UART1 initialized: baud=115200 rx_gpio=4
I (xxx) FLIGHT_DATA: UART1 RX task started (baud=115200, rx_gpio=4)
I (xxx) BCAST: Init OK — Packet=77 bytes, broadcast=800ms, update=1000ms
I (xxx) SYS: Ready. Monitor with nRF Connect.
```

### 数据接收日志 (每 5 秒)

```
I (xxx) FLIGHT_DATA: frames=1234 crc_err=0 armed=0 status=3 fix=3 sats=12 lat=30.148875 lon=120.511457 alt=9.5
```

| 字段 | 含义 | 正常值 |
|------|------|--------|
| frames | 已解析帧数 | 持续增长 |
| crc_err | CRC 错误数 | 应为 0 或极少 |
| armed | 是否解锁 | 0=未解锁, 1=已解锁 |
| status | 系统状态 | 3=STANDBY, 4=ACTIVE |
| fix | GPS 定位类型 | 2=2D, 3=3D |
| sats | 可见卫星数 | ≥8 为佳 |
| lat/lon/alt | 位置/高度 | 应与 QGC 一致 |

---

## 硬件连接

详见 `doc/hardware_connection_guide.md`。

### 最小接线 (2 根线)

```
飞控 TELEM1 TX ──────→ ESP32-C5 GPIO4
飞控 TELEM1 GND ─────→ ESP32-C5 GND
```

### 引脚对照

| 飞控 | ESP32-C5 | 说明 |
|------|----------|------|
| TELEM1 TX (Pin 5) | GPIO4 (UART1_RX) | 飞控发送 → ESP32 接收 |
| TELEM1 GND (Pin 1) | GND | 信号共地 |

---

## 验证步骤

### 1. 串口监控

烧录后打开串口监控，观察：
- 启动日志显示 UART 初始化成功
- 每 5 秒输出状态摘要
- `frames` 数值持续增长
- `crc_err` 保持为 0 或极少

### 2. BLE 广播验证

使用 nRF Connect (手机 App) 扫描：
- 设备名：`ESP32C5_RID`
- Service UUID：`0x0D50`
- Service Data：77 字节 GB 46750 数据包

### 3. 数据对比

将 ESP32 解析的数据与 QGC/Mission Planner 显示对比：
- 纬度/经度误差 < 0.00001°
- 高度误差 < 1m
- 航向误差 < 5°

---

## 常见问题

### Q1: 串口日志显示 "Data STALE"

**原因**: 超过 2 秒未收到有效位置数据

**排查**:
1. 检查接线是否正确 (TX→RX, GND→GND)
2. 检查飞控是否上电
3. 检查波特率是否匹配
4. 确认 GPS 已锁定 (fix ≥ 2, sats ≥ 4)

### Q2: crc_err 持续增长

**原因**: CRC 校验失败

**排查**:
1. 波特率不匹配 — 检查 `FC_UART_BAUD_RATE` 与飞控设置
2. 接线接触不良 — 重新插拔杜邦线
3. 电磁干扰 — 使用屏蔽线或缩短线缆

### Q3: frames=0, 无数据

**原因**: 未收到任何 MAVLink 帧

**排查**:
1. 确认飞控 TELEM1 端口输出 MAVLink 数据
2. 用 QGC 测试飞控 TELEM1 是否工作
3. 用万用表测量飞控 TX 引脚电压 (应为 3.3V)

### Q4: 位置数据为 0

**原因**: GPS 未锁定

**解决**:
1. 移到室外或靠窗位置
2. 等待 GPS 锁定 (1-3 分钟)
3. 确认 GPS 模块正常连接

---

## 调试技巧

### 开启详细日志

在 `config.h` 中设置:

```c
#define CONFIG_RID_VERBOSE_LOG 1
```

重新编译烧录后，会输出每条消息的详细内容:

```
I (xxx) MAVLINK: HEARTBEAT: type=2 armed=0 status=3
I (xxx) MAVLINK: GPS: fix=3 lat=30.148875 lon=120.511457 alt=9.5 sats=12 eph=1.2
I (xxx) MAVLINK: POS: lat=30.148875 lon=120.511457 alt=9.5 rel=-0.3 v=(-1.08,1.16,0.00) hdg=25.1
```

### 使用逻辑分析仪

如果接线正常但无数据，可用逻辑分析仪抓包:
1. 连接逻辑分析仪到飞控 TX
2. 设置采样率 > 4x 波特率 (如 500kHz for 115200)
3. 检查是否有 MAVLink 帧 (0xFD 开头)

---

## 下一步计划

### Stage 2 完善

- [ ] 添加操作员位置手动配置 (遥控器 GPS)
- [ ] 添加高度精度优化 (气压计融合)
- [ ] 添加航向精度优化 (磁力计融合)

### Stage 3: 产品化

- [ ] 替换 UAS_ID 为 UOM 备案编码
- [ ] 替换 REALNAME_ID 为实名登记号
- [ ] 启用 Flash 加密
- [ ] 启用 Secure Boot V2
- [ ] 更换联锁引脚 (避免 JTAG 冲突)

---

## 文档版本

| 版本 | 日期 | 修改内容 |
|------|------|---------|
| 1.0 | 2026-07-28 | 初始版本 (Mock → 真实飞控) |
