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
│                                   # 缺失编码为表3未知哨兵值（位置0xFFFFFFFF、航迹/速度0xFFFF）；O字段条件编码）
│
├── broadcaster/
│   └── ble_rid_broadcaster.h/cpp   # BLE5 广播控制（NimBLE EXT_ADV，三级自修复）
│
├── data/
│   ├── flight_data.h/cpp           # USB Host CDC-ACM 飞控数据读取 + CRC风暴恢复
│   ├── mavlink_parser.h/cpp        # MAVLink v1/v2 解析器（帧解析、消息解码、CRC校验）
│   │                               # 支持 7 种消息：HEARTBEAT / GPS_RAW_INT / ATTITUDE /
│   │                               # GLOBAL_POSITION_INT / VFR_HUD / HOME_POSITION / SYSTEM_TIME
│   └── mavlink_crc.h/cpp           # CRC-16/MCRF4XX 校验
│
├── indicators/
│   └── indicators.h/cpp            # 状态指示灯 (GB 46750-2025 5.1.5)
│
├── logging/
│   └── flight_log.h/cpp            # 飞行数据持久化存储 (GB 46750-2025 5.1.8)
│                                   # 环形缓冲区读写 + 异步磨损均衡写入
│
├── console/
│   └── console_cmd.h/cpp           # UART 命令行监听（DUMP 飞行日志导出）
│
├── interlink/
│   ├── fc_interlink.h              # 飞控交联抽象接口 (GB 46750-2025 5.1.7)
│   └── interlink_stub.h/cpp        # 测试阶段 stub 后端（仅记录，不实际发送）
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
                                                    (范围检查)   (M=0,O=条件)  (400ms)   (10s间隔)

PC (Python) ──UART0──→ "DUMP\r\n" ──→ ConsoleCmd ──→ flightLog.readRecord() ──→ CSV 文件
```

### 关键设计决策

- **M 字段始终存在**：即使数据不可用，`dataId` 位仍为 1，值编码为表3未知哨兵值（位置→0xFFFFFFFF、航迹/速度→0xFFFF，高度/时间戳/状态→0）——确保每个包都是新包
- **永不跳过广播**：数据过期/缺失时仍广播（附带 `ESP_LOGW` 告警），满足 GB 46750 "全过程自动持续发送"
- **GPS fix 解耦**：`mavlink_fillFlightData()` 不再以 `gpsFixType >= 2` 硬拦截数据输出；GPS_RAW_INT 和 GLOBAL_POSITION_INT 是独立 MAVLink 消息，到达顺序不确定，仅以 `lastPositionMs > 0` 判断是否有位置数据，GPS fix 不足降级为 STALE 质量标记
- **操作员位置三级回退**：优先使用 `HOME_POSITION`（飞控 Home 点），其次使用首次解锁时记录的"起飞点"（符合 `OP_LOCATION_TYPE=0` 起飞点位置语义），最后编码为表3未知哨兵值 0xFFFFFFFF（符合 GB 46750 Table 3 第 006 项要求）
- **状态机消抖**：地面↔空中切换需连续确认（空中→地面 500ms，地面→空中 300ms），防止 HEARTBEAT 短暂波动误触发；紧急/失效状态绕过消抖立即生效
- **数据缺失保状态**：飞行中数据短暂丢失时不覆盖 `opStatus`，保留上次已知空中状态，防止误判为地面而停止广播
- **DTR/RTS 飞控安全**：USB CDC-ACM 打开后显式清除 DTR/RTS（`set_control_line_state(false, false)`），飞控 USB 口的 DTR 可能连接到 MCU BOOT0/NRST 引脚，断言 DTR 会导致飞控复位或进入 bootloader 失控
- **USB 只读模式**：USB CDC 以只读模式打开（`out_buffer_size=0`），从物理层面杜绝任何数据反向注入飞控
- **BLE 自修复合并**：三级递进恢复（原地重启 → PHY 切换 → NimBLE 重初始化）统一为一个 `triggerSelfHeal()` 方法
- **CRC 风暴恢复**：连续 200 帧 CRC 校验失败 → 自动关闭并重新打开 USB 设备、重置 MAVLink 解析器，配合 5s 冷却期防止反复重连
- **MAVLink v1/v2 双协议**：同时支持 MAVLink v1 (0xFE) 和 v2 (0xFD)，覆盖 HEARTBEAT / GPS_RAW_INT / ATTITUDE / GLOBAL_POSITION_INT / VFR_HUD / HOME_POSITION / SYSTEM_TIME 七种消息，满足 GB 46750 全部 21 字段需求
- **Unix 时间戳来源**：从飞控 MAVLink `SYSTEM_TIME` 消息获取 GPS 授时，计算 `unixBootOffsetMs = unixTime - bootMs`，广播时使用 `unixBootOffsetMs + lastPositionBootMs`；未授时时正确填 0（未知）
- **环形缓冲区读取**：`readRecord(index)` 自动处理环形缓冲区回绕，通过 `(oldestOffset + index * 96) % partitionSize` 计算物理偏移，每次读取校验 magic + CRC16
- **UART 命令行导出**：`ConsoleCmd` 监听 UART0 的 `DUMP\r\n` 命令，先抑制日志输出，以二进制协议 `+OK N\r\n` + N×96 bytes + `+DONE\r\n` 导出全部飞行记录
- **飞控重启 / boot_ms 回绕检测**：`bootMs` 显著回退（>500ms 容差 `MAVLINK_BOOT_ROLLOVER_TOLERANCE_MS`）判定飞控重启，`unixTimeValid` 作废、时间戳输出 0（未知）；新 `SYSTEM_TIME` 重锚定基线后时间戳正确重建，且 SYSTEM_TIME 先到不误判回绕
- **过期字段老化**：广播前 `gb46750_expireStaleFields()` 将位置/大地高度/航迹/地速中超过新鲜度阈值的字段清除 validMask 位，编码侧输出表3未知哨兵值；`opStatus`/操作员位置不老化（避免误判地面停播）
- **NaN/Inf 防御**：校验层 `isfinite` 识别非法值并置位 flags；编码层 NaN/Inf 一律编码为表3哨兵值，单一字段非法不影响其它字段
- **签名帧非阻塞 CRC**：MAVLink v2 签名帧 CRC 字节一收齐立即校验，13 字节签名视为"可选尾部"只消费不校验——避免解析器卡死、防止截断签名时读垃圾字节
- **任务看门狗覆盖 4 任务**：TWDT 同时订阅 main / usb_host(flight_data) / flight_log / console_cmd 四任务，任一任务 5s 无响应即触发系统复位（GB 42590-2023 A.2.3.5.5）
- **跨任务共享变量安全**：`_cdcDev/_deviceConnected/_deviceReady` 标记 `volatile`，USB 句柄生命周期由 `_devMutex` 保护，避免主循环与 USB Host 任务间寄存器缓存读到过期值
- **栈高水位监控**：启动时打印主任务栈高水位，此后每 60s 复查一次；HWM < 1024B 输出告警提示增大栈
- **飞控交联解耦**：失效→飞控安全处置通过 `IFcInterlink` 抽象接口注入（当前测试阶段 `StubFcInterlink` 仅记录），BLE 三级自愈全部失败时通知飞控禁飞/降落（GB 46750-2025 5.1.7），具体后端（UART MAVLink / DroneCAN / 飞控内部调用）待量产形态确定后实现

### 广播策略

- **地面状态**：停止广播，LED 绿色慢闪(0.5Hz)
- **空中/紧急状态**：`startBroadcast()` 启动 → `updateBroadcastData()` 原地更新（不停止广播），LED 蓝色快闪(2.5Hz)
- **状态切换**：消抖确认后执行（地面→空中 300ms / 空中→地面 500ms），紧急状态绕过立即切换
- **BLE 控制器复位**：自动检测 → 等待 NimBLE 重同步 → 触发三级自修复
- **数据缺失**：`FRESH_INVALID` 时保留上次有效包和状态，广播继续但标记数据过期
- **操作员位置**：优先使用飞控 Home 点，其次使用首次解锁时记录的起飞点，最后编码为表3未知哨兵值 0xFFFFFFFF
- **飞行日志导出**：PC 端 `python flight_log_dump.py COMx` → 通过 UART0 发送 `DUMP\r\n` → ESP32 回复二进制记录 → 解码为 GB 46750 全部 21 字段的 CSV 文件

### 硬件接口

#### ESP32-S3-DevKitC-1

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| GPIO19/20 | USB OTG (D-/D+) | 双向 | 连接飞控 USB 口，读取 MAVLink 数据 |
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

## 配置

核心配置集中在 `main/config.h`，关键项：

```c
#define UAS_ID "CPNYMDL001234567890A"  // 唯一产品识别码（20字符，量产替换为 UOM 备案编码）
#define REALNAME_ID "00000000"         // 实名登记号后 8 位
#define OP_CATEGORY 1                  // 运行类别：0=未定义,1=开放类,2=特定类,3=审定类
#define UA_CLASS 1                     // 无人机分类：0=微型,1=轻型,2=小型,3=中型,4=大型
#define OP_LOCATION_TYPE 0             // 遥控站位置类型：0=起飞点,1=遥控站位置
#define COORD_SYS 0                    // 坐标系：0=WGS-84,1=CGCS2000
#define BROADCAST_INTERVAL_MS 400      // 数据包广播间隔（GB 46750 要求 ≤1s）
#define FLIGHT_LOG_INTERVAL_S 10       // 飞行日志记录间隔（GB 46750 要求 ≤10s）
#define FLIGHT_LOG_PARTITION "flight_log"  // Flash 分区（见 partitions.csv）
#define STATUS_LED_GPIO GPIO_NUM_48    // WS2812B RGB LED（RMT 驱动）
```

完整参数（USB Host VID/PID、精度映射、定时、看门狗、CRC 风暴阈值、日志级别等）见 `main/config.h` 内注释。

LED 状态指示 (GB 46750-2025, 5.1.5)：

| 状态 | LED 行为 | 含义 |
|------|----------|------|
| `OFF` | 熄灭 | 未初始化 |
| `STANDBY` | 绿色慢闪 (0.5Hz) | 地面待机，模块自检通过 |
| `BROADCASTING` | 蓝色快闪 (2.5Hz) | 空中/紧急状态，正在广播 |
| `DEGRADED` | 橙色快闪 (~1.7Hz) | 自修复后降级运行（PHY 切换 / NimBLE 重初始化） |
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

### 飞控交联（GB 46750-2025 5.1.7）

> **注意**：此接口与上面「接入飞控」方向相反。USB Host 是**模块读取**飞控数据（只读，见上）；本节是**模块向飞控发送**失效处置指令，走独立的 `interlink/` 抽象层。

GB 46750-2025 5.1.7 要求运行识别发送模块与飞行控制功能模块互联：
- **5.1.7 a) 起飞前失效** → 应能阻止起飞
- **5.1.7 b) 飞行中失效** → 应能触发悬停/返航/降落

**当前阶段**：测试期无法与真实飞控交联，接口先行。实现为 `IFcInterlink` 抽象接口（`main/interlink/fc_interlink.h`），由构造函数注入 `RIDBroadcastManager`，当前注入 `StubFcInterlink`（`interlink_stub.cpp`）——只写日志 + 记录 `INTERLINK` 故障事件，**不实际发送任何指令**。BLE 三级自愈全部失败（识别发送功能失效）时触发 `notifyFault()`；自愈成功时触发 `notifyRecovered()`。

**量产形态待定（二选一）**，确定后替换 stub 实现：
- **A. 独立模块嵌入机体** → 经 UART 向飞控发送指令（MAVLink `MAV_CMD` 或 DroneCAN），与现有 USB Host 数据读取链路物理独立
- **B. 合并进飞控固件** → 模块逻辑编译进飞控，直接调用飞控内部安全接口（不再需要物理链路）

**集成注意事项**：
1. **方向单向**：接口只负责「模块→飞控」的失效通知，不反向接收飞控指令；失效状态同时通过 BLE 广播字段 015（运行状态 4=识别发送功能失效非紧急 / 5=失效紧急）对外可见
2. **失效语义分飞行阶段**：`notifyFault(reason, airborne, nowMs)` 的 `airborne` 参数由模块状态机判定——地面失效应禁止起飞，空中失效应悬停/返航/降落，两者处置策略由飞控侧执行
3. **恢复语义**：`notifyRecovered()` 仅告知飞控模块识别功能已恢复，**是否解除**禁飞/降落限制由飞控安全策略决定（推荐保守：一次触发失效后保持，除非飞控显式重新解锁）
4. **触发源当前唯一**：现阶段只有 BLE 三级自愈全部失败（`BLE_HEAL_FAILED`）会触发失效通知；自检失败走自愈链路，最终也汇聚于此，避免重复通知
5. **防误触**：`airborne` 判定复用状态机消抖结果，避免数据短暂波动误触发；未来量产后端需自带指令重试 + 超时确认机制
6. **与看门狗的关系**：任务看门狗超时是系统级复位（模块已死，无法通知飞控），不属于交联接口范围；交联接口覆盖的是模块**存活但识别功能失效**的场景

### 验证方法

推荐使用本仓库自带的**安卓检测 APP**（`app_android/`）现场抓包验证，安装与使用见 [`app_android/README.md`](app_android/README.md)：

- 安装 APK（`app_android/app/build/outputs/apk/debug/app-debug.apk`）→ 点 **开始扫描**；
- App 实时列出所有广播 UUID `0x0D50` 的设备，点进详情可查看逐字段解码、合规判定、RSSI / 速率曲线，并生成 Word 报告 / 导出 CSV。

也可用通用抓包工具 **nRF Connect**（Nordic Semiconductor）手动查看：

- 设备名：`GBI_RID_001`
- Service UUID：`0x0D50`（ASTM F3411 RID Service）
- Service Data 中为 GB 46750-2025 编码的数据包（可复制完整 Raw 广播帧，贴进 RID 检测 APP 的「粘贴解码」做单包解析）

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

**CSV 输出**包含 27 列：记录序号、Flash 时间戳（ms + UTC）、GB 46750 全部 21 字段（唯一产品识别码、实名登记号、运行类别、无人机分类、遥控站位置、经纬度、高度、速度、航向等）、CRC 有效性标志。

### 自检与告警

系统每 5 秒执行一次运行时自检（符合 GB 42590-2023 A.2.3.5.5），检测项：

| 检测项 | 故障表现 | 系统响应 |
|--------|----------|----------|
| BLE 同步丢失 | `Runtime check FAIL` | 连续 3 次触发 `triggerSelfHeal()` |
| 广播更新失败 | `update failed #N` | 连续 3 次触发 `triggerSelfHeal()` |
| 任一任务卡死 | 无输出 | 任务看门狗 5s 后系统复位（覆盖 main/usb_host/flight_log/console_cmd） |
| BLE 控制器复位 | `NimBLE controller reset` | `triggerSelfHeal()` 三级递进恢复 |
| USB 设备断开 | `USB device disconnected` | 自动重连（每 2 秒重试） |
| CRC 风暴 | `CRC storm detected` | 关闭 USB 设备 → 重置解析器 → 重连（5s 冷却） |
| 堆内存不足 | `LOW HEAP WARNING` | 每 60s 监控，< 10KB 输出告警 |
| 飞行日志栈溢出 | `stack watermark LOW` | < 512 bytes 输出告警 |
| 主任务栈不足 | `Main stack LOW (HWM=...)` | 启动及每 60s 复查，HWM < 1024 bytes 输出告警 |
| 飞行日志读取失败 | `readRecord: CRC mismatch` | 记录损坏，跳过该条继续导出 |
| BLE 三级自愈全部失败 | `Self-heal FAILED — all 3 tiers exhausted` | LED 红色 FAULT + 向飞控发出失效通知（stub 阶段仅记录） |

## 产品化 Checklist

- [x] 接入真实飞控 USB Host 数据源
- [x] 确认飞控 VID/PID（0x1B8C / 0x0036）
- [x] MAVLink v1/v2 双协议解析（CRC 校验 + 消息解码）
- [x] CRC 风暴检测与 USB 自恢复
- [x] FlightLog 读取接口：`readRecord()` / `readLatestRecord()` + UART 命令行 DUMP 导出 + PC Python 脚本 (GB 46750-2025 5.1.8)
- [x] 操作员位置三级回退：HOME_POSITION → 起飞点（首次解锁时记录）→ 表3未知哨兵值 0xFFFFFFFF
- [x] PC 测试套件 8228 用例全部通过（含 1899 真实 .DAT 帧解析 + golden packet 逐字节验证 + 解析器压力测试）
- [x] Unix 时间戳从飞控 SYSTEM_TIME 获取，未授时正确填 0
- [ ] 将 `UAS_ID` 替换为 UOM 平台备案的唯一产品识别码
- [ ] 将 `REALNAME_ID` 替换为实名登记系统获取的登记号后 8 位
- [ ] 确认经纬度坐标系（WGS-84 或 CGCS2000）
- [x] 验证 GPIO48 (WS2812B) LED 闪烁模式（绿色慢闪=待机 / 蓝色快闪=广播 / 橙色慢闪=降级 / 红色常亮=故障）
- [ ] 验证飞行日志存储：串口导出记录数、估算容量是否满足 120h
- [ ] 场地实地验证：RID 检测安卓 APP（`app_android/`，见[验证方法](#验证方法)）扫描距离、数据正确性
- [ ] 实现量产飞控交联后端（UART MAVLink / DroneCAN 或飞控内部调用），替换 `StubFcInterlink`（GB 46750-2025 5.1.7）
- [ ] 向整机厂确认量产机网络报送功能（4 问，见「已知限制 → 网络式 RID」），确定本模块是否需实现网络式

### 量产安全配置（最终生产阶段）

- [ ] 启用 Flash 加密 (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`)
- [ ] 禁用 JTAG via eFuse (`DIS_USB_JTAG`)
- [ ] 启用 Secure Boot V2 并配置签名密钥
- [ ] 设计加密 OTA 更新机制

## 安全配置

### Secure Boot V2

防止恶意固件刷入（一次性启用）：
1. `espsecure.py generate_signing_key secure_boot_signing_key.pem` 生成签名密钥（私钥妥善保管）
2. `sdkconfig.defaults` 启用 `CONFIG_SECURE_BOOT=y` / `CONFIG_SECURE_SIGNED_ON_BOOT=y` 并指向签名密钥
3. `idf.py reconfigure && idf.py build`，首次烧录 bootloader 并设为安全模式

### Flash 加密

保护固件镜像不被读取/复制：

- 开发阶段：使用 Release 模式 (CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE)
- 预置密钥到 eFuse, 后续 OTA 使用 `idf.py encrypted-app-flash`
- 注意：加密后无法通过物理 Flash 读取恢复固件, 务必保留原始 .bin

### Brownout 检测

`sdkconfig.defaults` 已启用。电池供电无人机在机动时电压可能骤降, Brownout 检测器在供电低于阈值时触发安全复位, 避免 Flash 写入损坏。

### 日志控制

量产固件应在 `config.h` 中设置 `CONFIG_RID_VERBOSE_LOG 0`, 关闭 hex dump 和 TX 详细日志以减少 UART 功耗。

## 已知限制（明确不做）

以下两项属于 GB 46750-2025 的系统级 / 待主管部门发布要求，本模块**明确不做**，在此记录以避免误解：

### 网络式 RID（GB 46750-2025 5.1.1）

5.1.1 要求整机系统同时具备**广播式 + 网络式**两条远程识别链路。本模块只实现广播式（BLE 5.0+ 扩展广播）。网络式 RID 依赖飞控的蜂窝/4G 链路与云平台配套，属于整机系统集成职责，非本模块范围。

> **⚠️ 待办（量产前确认）**：量产机整机**可能已有** 4G 网络报送链路（自 2024 年《无人驾驶航空器飞行管理暂行条例》第 24 条起即为常见配置）。若确认整机已向 UOM 报送实时数据，则 5.1.1「同时具备」由 **广播式（本模块）+ 网络式（整机）** 共同满足，本模块无需实现网络式。
>
> **向整机厂 / 组长确认 4 项**：
> 1. 量产机有无 4G / 蜂窝模块？
> 2. 是否已向 UOM（民航局一体化综合监管服务平台）报送**实时**位置/高度/航向/速度/UPIC？
> 3. 报送链路是否整机自带（无需外接模块）？
> 4. 实名登记与 UOM 注册是否出厂完成？
>
> 全部为"是" → 网络式由整机兜底，本模块专注广播式；否则网络式缺口由整机厂自行补齐（不属本模块范围）。

### 防篡改 / 消息计数器（GB 46750-2025 5.2.2）

5.2.2 规定数据包扩展内容应使用民用航空行业主管部门**统一发布的协议**。自行在包内添加消息计数器 / HMAC 字段不合规，故**不实现**。当前防篡改防线为模块级 Secure Boot + Flash 加密（见上文"安全配置"），待主管部门发布官方扩展协议后再跟进。

## 同类产品对比

| 项目 | 本模块 | CUAV C-RID | Walkera W-RID |
|------|--------|------------|---------------|
| 主控 | ESP32-S3 | ESP32 | 未公开 |
| 广播链路 | BLE 5 扩展广播 | WiFi + BLE 5 双链路 | 未公开（宣称 3km） |
| 与飞控接口 | USB（自带 USB Host） | DroneCAN / UART | DroneCAN / USRT |
| 天线 | 板载 PCB 天线 | MMCX 外接天线（20dBm，>300m） | 未公开 |
| 参考价 | — | 约 ¥299 | 约 ¥199 |

> 以上两款均基于 ArduPilot 开源固件 ArduRemoteID 开发。经核实，**尚不能确认其符合 GB 46750-2025**：
>
> 1. **资料自相矛盾**：C-RID 英文页称支持 GB 46750-2025，但中文产品页 / 2025-06 手册仅引用 GB/T 41300-2022 与 GB 42590-2023，未提及新国标。
> 2. **上游固件未支持**：ArduRemoteID 仓库存在未关闭的 issue [#158 "Add support for China's RemoteID requirements GB 46750-2025"](https://github.com/ArduPilot/ArduRemoteID/issues/158)，说明该数据格式在新固件中尚未落地。
> 3. **无监管认证**：属厂商设计声明，未见 CAAC 认证记录。
>
> **本模块差异**：GB 46750 数据包为**自行实现**（版本字节 `0x20`），并配套两套检测工具作为合规性验证手段——PC 端软件（`app/`，BLE 接收 / 逐字段解码 / 内置判断器自检 / 串口飞行日志导出）与**安卓手机 APP**（`app_android/`，现场 BLE 抓包 / 逐字段解码 / 合规判定 / Word 报告与 CSV 导出）。

### 广播链路选择（为何保持纯 BLE 扩展广播）

无人机编队控制常用 WiFi，若再加 WiFi 广播会直接抢占 2.4GHz 信道。BLE 广播只占用 37/38/39 三个信道、占空比极低，与 WiFi 共存友好；且 ESP32-S3 为单射频，WiFi 与 BLE 需时分复用，同时承载两条 2.4GHz 链路会互相挤压。故保持**纯 BLE 5 扩展广播**为主链路，不做 WLAN 广播。

## 相关文档

- [ESP32-S3 迁移指南](doc/esp32s3_migration.md) — 从 ESP32-C5 迁移到 ESP32-S3
- [USB 即插即用指南](doc/usb_plug_and_play.md) — USB Host 配置与使用
- [安卓检测 APP](app_android/README.md) — 手机端 BLE 抓包 / 解码 / 合规判定 / 报告导出

**最后更新**: 2026-08-04
