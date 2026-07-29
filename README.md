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
├── config.h                        # 用户配置（UAS ID、精度、定时参数、GPIO引脚、UART配置）
├── main.cpp                        # 精简编排器（~80行）：init + loop
│
├── broadcast/
│   └── broadcast_manager.h/cpp     # 广播编排器：数据验证、包构建、状态机、BLE自修复、
│                                   # 飞行日志调度、运行时自检、堆内存监控
│
├── protocol/
│   └── rid_messages.h/cpp          # GB 46750-2025 协议编码（21字段，M字段始终存在，
│                                   # 缺失编码为0；O字段条件编码）
│
├── broadcaster/
│   └── ble_rid_broadcaster.h/cpp   # BLE5 广播控制（NimBLE EXT_ADV，三级自修复）
│
├── data/
│   ├── flight_data.h               # 数据源接口：void getFlightData(FlightData&, uint64_t)
│   ├── flight_data.cpp             # Stage 2: UART MAVLink 解析实现
│   └── mavlink_parser.h/cpp        # MAVLink v2 解析器（帧解析、消息解码）
│
├── indicators/
│   └── indicators.h/cpp            # 状态指示灯 + 飞控联锁 (GB 46750-2025 5.1.5, 5.1.7)
│
└── logging/
    └── flight_log.h/cpp            # 飞行数据持久化存储 (GB 46750-2025 5.1.8)
```

### 数据流

```
getFlightData()  ──→  FlightData  ──→  RIDBroadcastManager::update()
                                              │
                                    ┌─────────┼──────────┐
                                    ▼         ▼          ▼
                             validateData  buildPacket  handleBroadcast
                              (范围检查)   (M=0,O=条件)  (永不跳过)
                                              │
                                              ▼
                              broadcaster.updateBroadcastData(pkt)
                              (原地更新AD数据，不停止广播)
```

### 关键设计决策

- **M 字段始终存在**：即使数据不可用，`dataId` 位仍为 1，值编码为 0（GB 46750 "未知" 值）——确保每个包都是新包，Message Counter 每包自增
- **永不跳过广播**：数据过期/缺失时仍广播（附带 `ESP_LOGW` 告警），满足 GB 46750 "全过程自动持续发送"
- **BLE 自修复合并**：三级递进恢复（原地重启 → PHY 切换 → NimBLE 重初始化）统一为一个 `triggerSelfHeal()` 方法
- **地面↔空中状态机**：空中故障只告警不拉闸（飞控自主飞行），地面故障拉闸禁止起飞

### 广播策略

- **地面状态**：停止广播
- **空中/紧急状态**：`startBroadcast()` 配置并启动 → `updateBroadcastData()` 原地更新数据（不停止广播，满足 GB 42590 "持续广播" 要求）
- **BLE 控制器复位**：自动检测 → 等待 NimBLE 重同步 → 自动重建广播链路
- **看门狗**：主循环 5s 无响应 → 系统自动复位

### 硬件接口

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| GPIO6 (MTMS) | 飞控联锁 RID_OK | 输出 | 自检通过→拉高（飞控允许起飞），异常→拉低（飞控禁止起飞），符合 GB 46750-2025 5.1.7；电平极性可通过 `INTERLOCK_ACTIVE_LEVEL` 配置 |
| GPIO27 | WS2812B RGB LED | 输出 | RMT 外设驱动，绿色慢闪(0.5Hz)=地面待机，蓝色快闪(2.5Hz)=空中/紧急广播中，红色常亮=模块故障，符合 GB 46750-2025 5.1.5 |

### Flash 要求

为满足 GB 46750-2025 5.1.8 规定的 "不少于 120 飞行小时" 存储要求：

| Flash 容量 | flight_log 分区 | 可存储时长 | 是否满足 |
|-----------|----------------|-----------|---------|
| 4 MB | ~2 MB | ~60 h | ✗ |
| 8 MB | ~6 MB | ~170 h | ✓ |

**量产推荐：ESP32-C5-WROOM-1 (8 MB Flash)**

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

首次使用需配置 NimBLE 和自定义分区表（已在 `sdkconfig.defaults` 和 `partitions.csv` 中预设）。

## 配置文件

核心配置项在 `config.h`：

### 识别信息

```c
#define UAS_ID "CPNYMDL001234567890A"  // 唯一产品识别码（20字符，替换为 UOM 备案编码）
#define REALNAME_ID "00000000"         // 实名登记号后 8 位
#define OP_CATEGORY 1                  // 运行类别：0=未定义, 1=开放类, 2=特定类, 3=审定类
#define UA_CLASS 1                     // 无人机分类：0=微型, 1=轻型, 2=小型, 3=中型, 4=大型
#define OP_LOCATION_TYPE 0             // 遥控站位置类型：0=起飞点, 1=遥控站位置
#define COORD_SYS 0                    // 坐标系：0=WGS-84, 1=CGCS2000
```

### 精度取值

```c
#define HORIZ_ACC 10  // 水平精度：<10m
#define VERT_ACC  5   // 垂直精度：<3m
#define SPEED_ACC 3   // 速度精度：<1m/s
#define TS_ACC    5   // 时间戳精度：≤0.1s
```

### 定时参数

```c
#define BROADCAST_INTERVAL_MS 800      // 数据包广播间隔（GB 46750 要求 ≤1s）
#define BLE_ADV_INTERVAL_MS 100        // BLE 底层广播间隔（影响功耗与发现延迟）
#define DATA_UPDATE_INTERVAL_MS 1000   // 飞行数据刷新间隔
#define DATA_FRESH_THRESHOLD_MS 2000   // 数据过期阈值（超时未更新标记为 STALE）
#define SELF_TEST_INTERVAL_MS 5000     // 运行时自检间隔
#define WATCHDOG_TIMEOUT_MS 5000       // 任务看门狗超时
```

### GPIO 引脚

```c
#define INTERLOCK_RID_OK_GPIO   GPIO_NUM_6   // 飞控联锁（自检通过→拉高，异常→拉低）
#define INTERLOCK_ACTIVE_LEVEL  1            // 联锁有效电平：1=高有效, 0=低有效
#define STATUS_LED_GPIO         GPIO_NUM_27  // WS2812B RGB LED（RMT 驱动）
#define STATUS_LED_NUM_LEDS     1
```

### 飞行日志存储

```c
#define FLIGHT_LOG_INTERVAL_S    10          // 记录间隔（GB 46750 要求 ≤10s）
#define FLIGHT_LOG_PARTITION     "flight_log" // Flash 分区名（见 partitions.csv）
#define FLIGHT_LOG_TASK_STACK    3072        // 异步写入任务栈（bytes）
#define FLIGHT_LOG_QUEUE_DEPTH   16          // 写入队列深度
```

### 模拟飞行（Stage 1 Mock）

```c
#define MOCK_LATITUDE       31.230416f   // 起飞点纬度（上海, WGS-84）
#define MOCK_LONGITUDE     121.473701f   // 起飞点经度
#define MOCK_GEO_BASE_ALT  120.5f        // 地面大地高度 (m)
#define SIM_GROUND_WAIT_MS   5000        // 地面等待
#define SIM_TAKEOFF_MS      10000        // 起飞爬升
#define SIM_CRUISE_MS       40000        // 巡航飞行
#define SIM_LANDING_MS      10000        // 降落
#define SIM_CRUISE_ALT      50.0f        // 巡航高度 AGL (m)
#define SIM_CRUISE_SPEED    15.0f        // 巡航地速 (m/s)
```

### 日志级别

```c
#define CONFIG_RID_VERBOSE_LOG 0  // 1=详细日志(hex dump等), 0=精简日志（量产固件推荐 0）
```

## 开发指南

### Stage 2: 接入真实飞控 (当前)

数据源从 Mock 切换到真实飞控 MAVLink UART 数据。

**接线**: 飞控 TELEM1 TX → ESP32-C5 GPIO4, GND → GND (只需 2 根线)

**配置**: 修改 `config.h` 中 UART 参数 (端口、波特率必须与飞控一致)

**详细操作**: 见 [`doc/hardware_connection_guide.md`](doc/hardware_connection_guide.md) 和 [`doc/stage2_migration_guide.md`](doc/stage2_migration_guide.md)

### 数据流

```
飞控 TELEM1 ──→ UART1 ──→ mavlink_parseByte() ──→ FlightData ──→ RIDBroadcastManager::update()
                                                                          │
                                                                ┌─────────┼──────────┐
                                                                ▼         ▼          ▼
                                                         validateData  buildPacket  handleBroadcast
                                                          (范围检查)   (M=0,O=条件)  (永不跳过)
```

### 验证方法

用手机安装 **nRF Connect**（Nordic Semiconductor），扫描 BLE 设备：

- 设备名：`ESP32C5_RID`
- Service UUID：`0x0D50`（ASTM F3411 RID Service）
- Service Data 中为 GB 46750-2025 编码的 77 字节数据包

串口监控输出示例：

```
I (1234) SYS: === ESP32-C5 RID Broadcaster — GB 46750-2025 ===
I (1235) SYS: Light show drone | BLE5 Extended Advertising | Mock data
I (1240) BCAST: Init OK — Packet=77 bytes, broadcast=800ms, update=1000ms
I (1250) SYS: Ready. Monitor with nRF Connect.

W (7000) BCAST: Data STALE (> 2000 ms), broadcasting anyway
I (7800) BCAST: Broadcast START (status=2)
I (7800) BCAST: TX #1 (77 bytes):
ff 01 47 e7 07 00 31 ...
I (8600) BCAST: TX #2 (77 bytes):
ff 01 47 e7 07 00 32 ...
```

### 自检与告警

系统每 5 秒执行一次运行时自检（符合 GB 42590-2023 A.2.3.5.5），检测项：

| 检测项 | 故障表现 | 系统响应 |
|--------|----------|----------|
| BLE 同步丢失 | `Runtime check FAIL` | 连续 3 次触发 `triggerSelfHeal()` |
| 广播更新失败 | `update failed #N` | 连续 3 次触发 `triggerSelfHeal()` |
| 主循环卡死 | 无输出 | 看门狗 5s 后复位 |
| BLE 控制器复位 | `NimBLE controller reset` | `triggerSelfHeal()` 三级递进恢复 |
| 堆内存不足 | `LOW HEAP WARNING` | 每 60s 监控，< 10KB 输出告警 |
| 飞行日志栈溢出 | `stack watermark LOW` | < 512 bytes 输出告警 |

## 产品化 Checklist

- [ ] 将 `UAS_ID` 替换为 UOM 平台备案的唯一产品识别码
- [ ] 将 `REALNAME_ID` 替换为实名登记系统获取的登记号后 8 位
- [ ] 将 `data/flight_data.cpp` 替换为真实飞控 UART 解析实现
- [ ] 确认飞控输出的运行状态（地面/空中/紧急）与 GB 46750-2025 Table 3-015 的映射关系
- [ ] 确认经纬度坐标系（WGS-84 或 CGCS2000）
- [ ] 连接 GPIO6 (联锁) 到飞控输入引脚，飞控端检测低电平拒绝解锁
- [ ] 连接 GPIO27 (WS2812B) 到 RGB LED，验证闪烁模式（绿色慢闪=待机 / 蓝色快闪=广播 / 红色常亮=故障）
- [ ] 验证飞行日志存储：串口导出记录数、估算容量是否满足 120h
- [ ] 选用 8 MB Flash 模组以确保存储容量满足 GB 46750-2025 5.1.8
- [ ] 场地实地验证：手机端 App 扫描距离、数据正确性

### 量产安全配置（最终生产阶段）

- [ ] 启用 Flash 加密 (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`)
- [ ] 禁用 JTAG via eFuse (`DIS_USB_JTAG`)
- [ ] 启用 Secure Boot V2 并配置签名密钥
- [ ] 将 GPIO6 (MTMS) 联锁引脚替换为非 JTAG GPIO，避免调试接口冲突
- [ ] 设计加密 OTA 更新机制

## 量产安全配置

### Secure Boot V2

防止恶意固件刷入：

```bash
# 1. 生成签名密钥 (仅一次, 私钥妥善保管)
espsecure.py generate_signing_key secure_boot_signing_key.pem

# 2. 在 sdkconfig.defaults 中取消注释:
#    CONFIG_SECURE_BOOT=y
#    CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"

# 3. 重新配置并构建
idf.py reconfigure
idf.py build
```

首次启用需烧录 bootloader 并将芯片设置为安全模式（一次性操作）。

### Flash 加密

保护固件镜像不被读取/复制：

- 开发阶段：使用 Release 模式 (CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE)
- 预置密钥到 eFuse, 后续 OTA 使用 `idf.py encrypted-app-flash`
- 注意：加密后无法通过物理 Flash 读取恢复固件, 务必保留原始 .bin

### Brownout 检测

`sdkconfig.defaults` 已启用。电池供电无人机在机动时电压可能骤降, Brownout 检测器在供电低于阈值时触发安全复位, 避免 Flash 写入损坏。

### 日志控制

量产固件应在 `config.h` 中设置 `CONFIG_RID_VERBOSE_LOG 0`, 关闭 hex dump 和 TX 详细日志以减少 UART 功耗。
