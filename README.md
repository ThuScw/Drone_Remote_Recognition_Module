# ESP32-C5 RID Broadcaster

无人机远程识别（Remote ID）广播模块，运行在 ESP32-C5 上，通过 BLE5 Extended Advertising 广播 GB 46750-2025 标准飞行数据。

## 适用标准

| 标准 | 内容 |
|------|------|
| GB 46750-2025 | 民用无人驾驶航空器系统运行识别规范（数据格式与广播间隔） |
| GB 42590-2023 | 民用无人驾驶航空器系统安全要求（电子围栏、自检、全飞行周期） |
| GB 46860-2025 | 民用无人驾驶航空器系统唯一产品识别码 |

## 架构

```
main/
├── config.h                  # 用户配置（UAS ID、精度、定时参数）
├── flight_data.h             # 数据源接口：void getFlightData(FlightData&, uint64_t)
├── flight_data.cpp           # Mock 实现（65s 起降循环）→ Stage 2 替换为 UART 解析
├── rid_messages.h/cpp        # GB 46750-2025 协议编码（21 个字段，二进制序列化）
├── ble_rid_broadcaster.h/cpp # BLE5 广播控制（NimBLE EXT_ADV，原地更新，复位恢复）
└── main.cpp                  # 主循环：读数据 → 组包 → 广播 → 自检
```

### 数据流

```
getFlightData()  ──→  FlightData  ──→  gb46750_buildPacket()  ──→  GB46750Packet
                                                                      │
                                              ┌───────────────────────┘
                                              ▼
                              broadcaster.startBroadcast(pkt)     // 起飞时（一次性）
                              broadcaster.updateBroadcastData(pkt) // 飞行中（周期性，原地更新）
```

### 广播策略

- **地面状态**：停止广播
- **空中/紧急状态**：`startBroadcast()` 配置并启动 → `updateBroadcastData()` 原地更新数据（不停止广播，满足 GB 42590 "持续广播" 要求）
- **BLE 控制器复位**：自动检测 → 等待 NimBLE 重同步 → 自动重建广播链路
- **看门狗**：主循环 5s 无响应 → 系统自动复位

## 构建

### 依赖

- ESP-IDF v5.1+（ESP32-C5 支持）
- NimBLE BLE5 Extended Advertising 栈

### 编译 & 烧录

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <串口> flash monitor
```

首次使用需配置 NimBLE 选项（已在 `sdkconfig.defaults` 中预设）：

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_EXT_ADV=y
CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=1650
CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=1
```

## 配置文件

核心配置项在 `config.h`：

```c
#define UAS_ID "CPNYMDL00123456789A"   // 唯一产品识别码（替换为 UOM 备案编码）
#define REALNAME_ID "00000000"         // 实名登记号后 8 位
#define OP_CATEGORY 1                  // 运行类别：1=开放类, 2=特定类, 3=审定类
#define UA_CLASS 1                     // 无人机分类：1=轻型
#define BROADCAST_INTERVAL_MS 800      // 广播间隔（≤1s）
#define DATA_UPDATE_INTERVAL_MS 1000   // 数据更新间隔
#define SELF_TEST_INTERVAL_MS 5000     // 自检间隔
#define WATCHDOG_TIMEOUT_MS 5000       // 看门狗超时
```

## 开发指南

### Stage 1 → Stage 2：接入真实飞控

只需修改 **一个文件** — `flight_data.cpp`：

1. 确认飞控串口协议（MAVLink / MSP / 自定义）、波特率、数据帧格式
2. 初始化 UART
3. 解析 GPS 位置（经纬度）、高度（大地/气压/相对）、速度、航向、运行状态
4. 填充 `FlightData` struct 返回

当前 Mock 实现使用 `nowMs % 65000` 取模做 65s 起降循环，包含四个阶段：

| 阶段 | 时长 | 模拟行为 |
|------|------|----------|
| 地面等待 | 5s | opStatus=GND，无广播 |
| 起飞爬升 | 10s | 高度 0→50m，向北微移 |
| 巡航 | 40s | 高度 50m，15m/s 顺时针画圆 |
| 降落 | 10s | 高度 50→0m，速度递减 |

### 接口约定

`getFlightData()` 签名不可变，返回值不走 return 而是直接填 struct：

```cpp
void getFlightData(FlightData& fd, uint64_t nowMs);
//                              ^^^^^^^^^^^^^^^^
//                              Mock 用时间，真飞控可忽略
```

### 验证方法

用手机安装 **nRF Connect**（Nordic Semiconductor），扫描 BLE 设备：

- 设备名：`ESP32C5_RID`
- Service UUID：`0x0D50`（ASTM F3411 RID Service）
- Service Data 中为 GB 46750-2025 编码的 77 字节数据包

串口监控输出示例：

```
[SYS] Packet=77 bytes | Broadcast every 800 ms | Update every 1000 ms
[SYS] Ready. Monitor with nRF Connect.

[TX] Status=AIR Alt=50.0m Spd=15.0m/s Hdg=180.0°
[TX] Packet (77 bytes): FF 01 4B FF FF E0 43 50 4E ...
```

### 自检与告警

系统每 5 秒执行一次运行时自检（符合 GB 42590-2023 A.2.3.5.5），检测项：

| 检测项 | 故障表现 | 系统响应 |
|--------|----------|----------|
| BLE 同步丢失 | `RUNTIME-CHECK FAIL` | 连续 3 次触发恢复流程 |
| 广播更新失败 | `update failed #N` | 连续 3 次输出 CRITICAL 告警 |
| 主循环卡死 | 无输出 | 看门狗 5s 后复位 |
| BLE 控制器复位 | `NimBLE controller reset` | 自动重同步 + 重建广播 |

## 产品化 Checklist

- [ ] 将 `UAS_ID` 替换为 UOM 平台备案的唯一产品识别码
- [ ] 将 `REALNAME_ID` 替换为实名登记系统获取的登记号后 8 位
- [ ] 将 `flight_data.cpp` 替换为真实飞控 UART 解析实现
- [ ] 确认飞控输出的运行状态（地面/空中/紧急）与 GB 46750-2025 Table 3-015 的映射关系
- [ ] 确认经纬度坐标系（WGS-84 或 CGCS2000）
- [ ] 场地实地验证：手机端 App 扫描距离、数据正确性
