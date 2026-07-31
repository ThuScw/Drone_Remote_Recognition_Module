# ESP32-S3 RID Broadcaster

无人机远程识别（Remote ID）广播模块，运行在 ESP32-S3 上，通过 BLE5 Extended Advertising 广播 GB 46750-2025 标准飞行数据。

## 适用标准

| 标准 | 内容 |
|------|------|
| GB 46750-2025 | 民用无人驾驶航空器系统运行识别规范（数据格式与广播间隔） |
| GB 42590-2023 | 民用无人驾驶航空器系统安全要求（电子围栏、自检、全飞行周期） |
| GB 46860-2025 | 民用无人驾驶航空器系统唯一产品识别码 |

## 架构

```
main/
├── config.h                        # 用户配置（UAS ID、精度、定时参数、GPIO引脚、USB Host）
├── main.cpp                        # 精简编排器（~110行）：init + loop
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
│   ├── flight_data.h/cpp           # USB Host CDC-ACM 飞控数据读取 + CRC风暴恢复
│   ├── mavlink_parser.h/cpp        # MAVLink v1/v2 解析器（帧解析、消息解码、CRC校验）
│   ├── mavlink_crc.h/cpp           # CRC-16/MCRF4XX 校验
│   └── mavlink_tx.h/cpp            # MAVLink TX 联锁命令（ARM/DISARM）
│
├── indicators/
│   └── indicators.h/cpp            # 状态指示灯 + 飞控联锁 (GB 46750-2025 5.1.5, 5.1.7)
│
├── logging/
│   └── flight_log.h/cpp            # 飞行数据持久化存储 (GB 46750-2025 5.1.8)
│                                   # 环形缓冲区读写 + 异步磨损均衡写入
│
├── console/
│   └── console_cmd.h/cpp           # UART 命令行监听（DUMP 飞行日志导出）
│
tools/
└── flight_log_dump.py              # PC 端 Python 脚本：串口 DUMP → CSV 解码导出
```

### 数据流

```
飞控 USB ──→ USB Host (CDC-ACM) ──→ mavlink_parseByte() ──→ FlightData
                                                                    │
                                                                    ▼
                                                          RIDBroadcastManager::update()
                                                                    │
                                                          ┌─────────┼──────────┬──────────┐
                                                          ▼         ▼          ▼          ▼
                                                   validateData  buildPacket  broadcast  flightLog
                                                    (范围检查)   (M=0,O=条件)  (800ms)   (10s间隔)

PC (Python) ──UART0──→ "DUMP\r\n" ──→ ConsoleCmd ──→ flightLog.readRecord() ──→ CSV 文件
```

### 关键设计决策

- **M 字段始终存在**：即使数据不可用，`dataId` 位仍为 1，值编码为 0（GB 46750 "未知" 值）——确保每个包都是新包
- **永不跳过广播**：数据过期/缺失时仍广播（附带 `ESP_LOGW` 告警），满足 GB 46750 "全过程自动持续发送"
- **GPS fix 解耦**：`mavlink_fillFlightData()` 不再以 `gpsFixType >= 2` 硬拦截数据输出；GPS_RAW_INT 和 GLOBAL_POSITION_INT 是独立 MAVLink 消息，到达顺序不确定，仅以 `lastPositionMs > 0` 判断是否有位置数据，GPS fix 不足降级为 STALE 质量标记
- **状态机消抖**：地面↔空中切换需连续确认（空中→地面 500ms，地面→空中 300ms），防止 HEARTBEAT 短暂波动误触发；紧急/失效状态绕过消抖立即生效
- **数据缺失保状态**：飞行中数据短暂丢失时不覆盖 `opStatus`，保留上次已知空中状态，防止误判为地面而停止广播
- **DTR/RTS 飞控安全**：USB CDC-ACM 打开后显式清除 DTR/RTS（`set_control_line_state(false, false)`），飞控 USB 口的 DTR 可能连接到 MCU BOOT0/NRST 引脚，断言 DTR 会导致飞控复位或进入 bootloader 失控
- **USB 只读模式**：`MAVLINK_TX_ENABLED=0` 时 `out_buffer_size=0`，USB CDC 以只读模式打开，从物理层面杜绝任何数据反向注入飞控
- **BLE 自修复合并**：三级递进恢复（原地重启 → PHY 切换 → NimBLE 重初始化）统一为一个 `triggerSelfHeal()` 方法
- **空中不拉闸、地面拉闸**：空中故障只告警不拉闸（飞控自主飞行），地面故障拉闸禁止起飞 (GB 46750-2025 5.1.7)
- **CRC 风暴恢复**：连续 200 帧 CRC 校验失败 → 自动关闭并重新打开 USB 设备、重置 MAVLink 解析器，配合 5s 冷却期防止反复重连
- **MAVLink v1/v2 双协议**：同时支持 MAVLink v1 (0xFE) 和 v2 (0xFD)，覆盖 HEARTBEAT / GPS_RAW_INT / ATTITUDE / GLOBAL_POSITION_INT / VFR_HUD / HOME_POSITION 六种消息，满足 GB 46750 全部 21 字段需求
- **环形缓冲区读取**：`readRecord(index)` 自动处理环形缓冲区回绕，通过 `(oldestOffset + index * 96) % partitionSize` 计算物理偏移，每次读取校验 magic + CRC16
- **UART 命令行导出**：`ConsoleCmd` 监听 UART0 的 `DUMP\r\n` 命令，先抑制日志输出，以二进制协议 `+OK N\r\n` + N×96 bytes + `+DONE\r\n` 导出全部飞行记录

### 广播策略

- **地面状态**：停止广播，LED 绿色慢闪(0.5Hz)
- **空中/紧急状态**：`startBroadcast()` 启动 → `updateBroadcastData()` 原地更新（不停止广播），LED 蓝色快闪(2.5Hz)
- **状态切换**：消抖确认后执行（地面→空中 300ms / 空中→地面 500ms），紧急状态绕过立即切换
- **BLE 控制器复位**：自动检测 → 等待 NimBLE 重同步 → 触发三级自修复
- **数据缺失**：`FRESH_INVALID` 时保留上次有效包和状态，广播继续但标记数据过期
- **看门狗**：主循环 5s 无响应 → 系统自动复位
- **飞行日志导出**：PC 端 `python flight_log_dump.py COMx` → 通过 UART0 发送 `DUMP\r\n` → ESP32 回复二进制记录 → 解码为 GB 46750 全部 21 字段的 CSV 文件

### 硬件接口

#### ESP32-S3-DevKitC-1

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| GPIO19/20 | USB OTG (D-/D+) | 双向 | 连接飞控 USB 口，读取 MAVLink 数据 |
| GPIO6 | 飞控联锁 RID_OK | 输出 | 自检通过→拉高（飞控允许起飞），异常→拉低（飞控禁止起飞），符合 GB 46750-2025 5.1.7 |
| GPIO48 | WS2812B RGB LED | 输出 | RMT 外设驱动，绿色慢闪(0.5Hz)=地面待机，蓝色快闪(2.5Hz)=空中/紧急广播中，红色常亮=模块故障 |
| COM 口 | UART0 | 双向 | 连接电脑：调试输出 + 烧录 + `DUMP` 命令行飞行日志导出 |

**USB 接口说明**：
- **"USB" 口**（GPIO19/20）：连接飞控，USB Host 模式读取数据
- **"COM" 口**（UART0）：连接电脑，调试和烧录
- 两个口可同时使用，互不冲突

### Flash 要求

为满足 GB 46750-2025 5.1.8 规定的 "不少于 120 飞行小时" 存储要求：

| Flash 容量 | flight_log 分区 | 可存储时长 | 是否满足 |
|-----------|----------------|-----------|---------|
| 4 MB | ~2 MB | ~60 h | ✗ |
| 8 MB | ~6 MB | ~170 h | ✓ |

**量产推荐：ESP32-S3-WROOM-1 (8 MB Flash)**

## 构建

### 依赖

- ESP-IDF v5.5+（ESP32-S3 支持）
- NimBLE BLE5 Extended Advertising 栈
- USB Host CDC-ACM 驱动

### 编译 & 烧录

```bash
# 1. 清理旧的构建缓存
rm -rf build

# 2. 设置目标芯片
idf.py set-target esp32s3

# 3. 编译
idf.py build

# 4. 烧录（使用 "COM" 口，不是 "USB" 口）
idf.py -p <COM口> flash monitor
```

首次使用需配置 NimBLE、USB Host 和自定义分区表（已在 `sdkconfig.defaults` 和 `partitions.csv` 中预设）。

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

### USB Host 飞控数据接口

```c
// 指定设备模式
#define FC_USB_VID          0x1B8C  // 飞控 VID
#define FC_USB_PID          0x0036  // 飞控 PID

// USB CDC-ACM 参数
#define FC_USB_BAUD_RATE    115200  // 波特率（需与飞控一致）
#define FC_USB_DATA_BITS    8
#define FC_USB_PARITY       0       // 0=None, 1=Odd, 2=Even
#define FC_USB_STOP_BITS    1

// MAVLink TX 安全开关（通过 USB 向飞控发送命令）
#define MAVLINK_TX_ENABLED  0       // 0=禁用(只读模式，推荐) 1=启用(GPIO6+MAVLink双联锁)
```
**⚠️ 安全警告**：`MAVLINK_TX_ENABLED=0` 时 USB CDC 以只读模式打开（`out_buffer_size=0`），且打开后显式清除 DTR/RTS，防止飞控被 USB 控制线信号复位。**首次接入飞控务必设为 0**，确认飞行稳定后再评估是否需要启用 MAVLink TX 联锁。

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
#define INTERLOCK_RID_OK_GPIO  GPIO_NUM_6   // 飞控联锁（自检通过→拉高允许起飞，异常→拉低禁止起飞）
#define STATUS_LED_GPIO        GPIO_NUM_48  // WS2812B RGB LED（RMT 驱动）
#define STATUS_LED_NUM_LEDS    1
```

LED 状态指示 (GB 46750-2025, 5.1.5)：

| 状态 | LED 行为 | 含义 |
|------|----------|------|
| `OFF` | 熄灭 | 未初始化 |
| `STANDBY` | 绿色慢闪 (0.5Hz) | 地面待机，模块自检通过 |
| `BROADCASTING` | 蓝色快闪 (2.5Hz) | 空中/紧急状态，正在广播 |
| `FAULT` | 红色常亮 | 模块故障，三级自修复全部失败 |

### 飞行日志存储

```c
#define FLIGHT_LOG_INTERVAL_S    10          // 记录间隔（GB 46750 要求 ≤10s）
#define FLIGHT_LOG_PARTITION     "flight_log" // Flash 分区名（见 partitions.csv）
#define FLIGHT_LOG_TASK_STACK    3072        // 异步写入任务栈（bytes）
#define FLIGHT_LOG_QUEUE_DEPTH   16          // 写入队列深度
```

### 日志级别

```c
#define CONFIG_RID_VERBOSE_LOG 0  // 1=详细日志(hex dump等), 0=精简日志（量产固件推荐 0）
```

### CRC 风暴恢复

```c
#define MAVLINK_CONSECUTIVE_CRC_LIMIT 200   // 连续 CRC 失败阈值（≈1s无有效帧，触发 USB 恢复）
#define USB_RECOVERY_COOLDOWN_MS 5000       // USB 恢复冷却时间（防止反复重连）
```

## 开发指南

### 接入飞控（USB Host 模式）

数据源通过 USB Host CDC-ACM 读取飞控 MAVLink 数据。

**接线**: 飞控 USB 口 → ESP32-S3 "USB" 口（GPIO19/20）

**详细操作**: 见 [`doc/esp32s3_migration.md`](doc/esp32s3_migration.md) 和 [`doc/usb_plug_and_play.md`](doc/usb_plug_and_play.md)

### 验证方法

用手机安装 **nRF Connect**（Nordic Semiconductor），扫描 BLE 设备：

- 设备名：`ESP32S3_RID`
- Service UUID：`0x0D50`（ASTM F3411 RID Service）
- Service Data 中为 GB 46750-2025 编码的数据包

### 导出飞行日志

通过 PC 端 Python 脚本导出飞行日志为 CSV：

```bash
# 安装依赖
pip install pyserial

# 自动扫描 COM 口
python tools/flight_log_dump.py

# 指定 COM 口
python tools/flight_log_dump.py COM3

# 指定输出文件
python tools/flight_log_dump.py COM3 -o flight_20260731.csv
```

**导出协议**：PC 发送 `DUMP\r\n` → ESP32 响应 `+OK <N>\r\n` → N×96 bytes 二进制记录 → `+DONE\r\n`

**CSV 输出**包含 28 列：记录序号、Flash 时间戳（ms + UTC）、GB 46750 全部 21 字段（唯一产品识别码、实名登记号、运行类别、无人机分类、遥控站位置、经纬度、高度、速度、航向等）、CRC 有效性标志。

### 串口监控输出示例

```
I (1234) SYS: === ESP32-S3 RID Broadcaster — GB 46750-2025 ===
I (1235) SYS: USB Host CDC-ACM | BLE5 Extended Advertising
I (2345) FLIGHT_DATA: USB Host initialized. Waiting for flight controller...
I (3456) FLIGHT_DATA: ✓ USB device opened (VID=0x1B8C, PID=0x0036)
I (4567) FLIGHT_DATA: frames=10049(v1=10049,v2=0) crc_err=11317 armed=0 fix=3 sats=14 ...
I (5678) BCAST: Init OK — Packet=0 bytes, broadcast=800ms, update=1000ms
```

### 自检与告警

系统每 5 秒执行一次运行时自检（符合 GB 42590-2023 A.2.3.5.5），检测项：

| 检测项 | 故障表现 | 系统响应 |
|--------|----------|----------|
| BLE 同步丢失 | `Runtime check FAIL` | 连续 3 次触发 `triggerSelfHeal()` |
| 广播更新失败 | `update failed #N` | 连续 3 次触发 `triggerSelfHeal()` |
| 主循环卡死 | 无输出 | 看门狗 5s 后复位 |
| BLE 控制器复位 | `NimBLE controller reset` | `triggerSelfHeal()` 三级递进恢复 |
| USB 设备断开 | `USB device disconnected` | 自动重连（每 2 秒重试） |
| CRC 风暴 | `CRC storm detected` | 关闭 USB 设备 → 重置解析器 → 重连（5s 冷却） |
| 堆内存不足 | `LOW HEAP WARNING` | 每 60s 监控，< 10KB 输出告警 |
| 飞行日志栈溢出 | `stack watermark LOW` | < 512 bytes 输出告警 |
| 飞行日志读取失败 | `readRecord: CRC mismatch` | 记录损坏，跳过该条继续导出 |

## 产品化 Checklist

- [x] 接入真实飞控 USB Host 数据源
- [x] 确认飞控 VID/PID（0x1B8C / 0x0036）
- [x] MAVLink v1/v2 双协议解析（CRC 校验 + 消息解码）
- [x] CRC 风暴检测与 USB 自恢复
- [x] FlightLog 读取接口：`readRecord()` / `readLatestRecord()` + UART 命令行 DUMP 导出 + PC Python 脚本 (GB 46750-2025 5.1.8)
- [ ] 将 `UAS_ID` 替换为 UOM 平台备案的唯一产品识别码
- [ ] 将 `REALNAME_ID` 替换为实名登记系统获取的登记号后 8 位
- [ ] 确认经纬度坐标系（WGS-84 或 CGCS2000）
- [ ] 连接 GPIO6 (联锁) 到飞控输入引脚，飞控端检测低电平拒绝解锁
- [x] 验证 GPIO48 (WS2812B) LED 闪烁模式（绿色慢闪=待机 / 蓝色快闪=广播 / 红色常亮=故障）
- [ ] 验证飞行日志存储：串口导出记录数、估算容量是否满足 120h
- [ ] 场地实地验证：手机端 App 扫描距离、数据正确性

### 量产安全配置（最终生产阶段）

- [ ] 启用 Flash 加密 (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`)
- [ ] 禁用 JTAG via eFuse (`DIS_USB_JTAG`)
- [ ] 启用 Secure Boot V2 并配置签名密钥
- [ ] 设计加密 OTA 更新机制

## 安全配置

### Secure Boot V2

防止恶意固件刷入：

```bash
# 1. 生成签名密钥 (仅一次, 私钥妥善保管)
espsecure.py generate_signing_key secure_boot_signing_key.pem

# 2. 在 sdkconfig.defaults 中取消注释:
#    CONFIG_SECURE_BOOT=y
#    CONFIG_SECURE_SIGNED_ON_BOOT=y
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

## 相关文档

- [ESP32-S3 迁移指南](doc/esp32s3_migration.md) — 从 ESP32-C5 迁移到 ESP32-S3
- [USB 即插即用指南](doc/usb_plug_and_play.md) — USB Host 配置与使用
