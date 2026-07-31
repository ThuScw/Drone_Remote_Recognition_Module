#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// ================= 用户配置区 =================

// 唯一产品识别码 (GB 46860-2025 Section 4.1 — 厂商生产格式)
// 结构: 厂商识别码(4) + 产品型号代码(4) + 序列号(12) = 20字符 ASCII
// 字符范围: 0-9 及除 O/I 外的大写字母 A-Z
// 产品化时替换为 UOM 平台备案的真实编码
#define UAS_ID "CPNYMDL001234567890A"

// 实名登记标志 (GB 46750-2025 Table 3-002)
// 在 UOM 实名登记系统获取的登记号码后 8 位字符，ASCII 编码，未填写时以 NULL 填充
#define REALNAME_ID "00000000"

// 运行类别 (GB 46750-2025 Table 3-003)
// 0=未定义, 1=开放类, 2=特定类, 3=审定类
// 表演型 35cm 轻型无人机通常属于开放类
#define OP_CATEGORY 1  // 开放类

// 无人机分类 (GB 46750-2025 Table 3-004)
// 0=微型, 1=轻型, 2=小型, 3=中型, 4=大型
// 35cm*35cm*10cm 表演无人机属于轻型
#define UA_CLASS 1  // 轻型

// 遥控站位置类型 (GB 46750-2025 Table 3-005)
// 0=起飞点位置, 1=遥控站位置
#define OP_LOCATION_TYPE 0

// 坐标系类型 (GB 46750-2025 Table 3-016)
// 0=WGS-84, 1=CGCS2000
#define COORD_SYS 0

// 精度取值 (GB 46750-2025 Table 3-017/018/019)
// 当 GPS eph/epv 可用时，动态计算精度；以下为 GPS 精度不可用时的 fallback 值
#define HORIZ_ACC 10  // <10m (fallback)
#define VERT_ACC  5   // <3m (fallback)
#define SPEED_ACC 3   // <1m/s

// 时间戳精度 (GB 46750-2025 Table 3-021) — GPS 授时后的动态值; 未授时设为 0 (未知)
#define TS_ACC 5  // ≤0.1s

// 广播间隔 (毫秒) — 完整包发送周期
// GB 46750 5.1.3: 更新和发送间隔 ≤ 1s
#define BROADCAST_INTERVAL_MS 800

// BLE 底层广播间隔 (毫秒) — 影响功耗和被发现概率
// 值越小广播越密集 (功耗越高), 值越大越省电 (但接收方发现延迟增加)
// 根据 GB 46750 5.1.3, 每秒至少发送 1 次, 间隔 ≤ 1000ms
#define BLE_ADV_INTERVAL_MS 100

// BLE 发射功率 (ESP32-S3)
// GB 46750-2025 6.1.3: 轻型无人机 EIRP ≥ 4 dBm (360°) 或 ≥ 6 dBm (平均)
// ESP32-S3 最大 +9 dBm (ESP_PWR_LVL_P9), 加 PCB 天线 ~2 dBi → EIRP ≈ 11 dBm
// 可选用: ESP_PWR_LVL_P3(+3), ESP_PWR_LVL_P6(+6), ESP_PWR_LVL_P9(+9)
#include <esp_bt.h>
#define BLE_TX_POWER_LEVEL ESP_PWR_LVL_P9

// 数据更新间隔 (毫秒) — 飞行数据刷新频率
// 独立于广播间隔，避免广播分片被打断
#define DATA_UPDATE_INTERVAL_MS 1000

// 数据新鲜度阈值 (毫秒) — 超过此时间未更新的数据标记为"过期"
// GB 46750-2025 要求实时性，超过 2s 的数据视为不可靠
#define DATA_FRESH_THRESHOLD_MS 2000

// 自检间隔 (毫秒) — 全飞行周期持续监测
// GB 42590-2023 A.2.3.5.5: 全飞行周期持续监测模块状态
#define SELF_TEST_INTERVAL_MS 5000

// 任务看门狗超时 (毫秒) — 主循环卡死超过此时长触发系统复位
#define WATCHDOG_TIMEOUT_MS 5000

// ================= 日志配置 =================

// 设为 1 开启详细日志 (hex dump, TX 详情, 调试信息)
// 量产固件应设为 0
#define CONFIG_RID_VERBOSE_LOG 0

// ================= GPIO 引脚分配 (ESP32-S3) =================

// 飞控联锁 GPIO (GB 46750-2025, 5.1.7)
// 模块自检通过 → GPIO6 拉高（允许飞控起飞），异常 → 拉低（禁止起飞）
// 功能由 RIDInterlock 类管理，需要硬件连线到飞控的 RID 联锁输入引脚
#define INTERLOCK_RID_OK_GPIO  GPIO_NUM_6

// 状态指示灯 (GB 46750-2025, 5.1.5)
// ESP32-S3-DevKitC-1 板载 WS2812B RGB LED，连接至 GPIO48，通过 RMT 外设驱动
// 绿色慢闪=待机, 蓝色快闪=广播中, 红色常亮=故障
#define STATUS_LED_GPIO        GPIO_NUM_48
#define STATUS_LED_NUM_LEDS    1

// ================= 飞行数据存储配置 (GB 46750-2025, 5.1.8) =================

// 记录间隔 (秒) — 标准要求 ≤10s
#define FLIGHT_LOG_INTERVAL_S   10

// Flash 分区名 (在 partitions.csv 中定义)
#define FLIGHT_LOG_PARTITION    "flight_log"

// 每条记录: 4B magic + 2B CRC + 8B timestamp + 2B len + 80B payload = 96B
#define FLIGHT_LOG_MAGIC        0x5249444C  // "RIDL"

// 飞行日志异步写入任务
#define FLIGHT_LOG_TASK_STACK       3072   // 任务栈 (bytes)
#define FLIGHT_LOG_TASK_PRIO        1      // 低优先级, 不影响广播
#define FLIGHT_LOG_QUEUE_DEPTH      16     // 队列深度 (160s 缓冲 @ 10s 间隔)
#define FLIGHT_LOG_WRITE_TIMEOUT_MS 100    // 队列满时等待超时 (ms)

// ================= USB Host 飞控数据接口 (ESP32-S3) =================

// USB Host CDC-ACM 配置
// 通过 USB OTG 口 (GPIO19/20) 读取飞控 MAVLink 数据
// 注意: ESP32-S3 的 USB OTG 引脚是固定的 GPIO19 (D-) 和 GPIO20 (D+)

// USB Host 设备指定
// 必须设置正确的 VID 和 PID（十六进制），不支持 VID=0 自动检测
// 如需自动检测（即插即用），需要另行实现 USB 设备枚举逻辑
//
// 常见飞控 VID/PID 参考:
//   - Pixhawk/Cube (ArduPilot): 0x1209 / 0x5740
//   - PX4: 0x26AC / 0x0011
//   - Betaflight: 0x0483 / 0x5740
//   - 通用 CDC-ACM: 0x303A / 0x4001
//   - CH340 芯片: 0x1A86 / 0x7523
//   - CP2102 芯片: 0x10C4 / 0xEA60

// 用户无人机飞控的 VID/PID (已确认所有同型号无人机一致)
// VID = 0x1B8C, PID = 0x0036
// 通过设备管理器硬件 ID 确认: USB\VID_1B8C&PID_0036&REV_0101
#define FC_USB_VID          0x1B8C
#define FC_USB_PID          0x0036

// USB CDC-ACM 参数 (需与飞控 USB 配置一致)
#define FC_USB_BAUD_RATE    115200
#define FC_USB_DATA_BITS    8
#define FC_USB_PARITY       0   // 0=None, 1=Odd, 2=Even
#define FC_USB_STOP_BITS    1

// USB Host 任务配置
#define USB_HOST_TASK_STACK     4096
#define USB_HOST_TASK_PRIO      10  // 较高优先级, 确保及时处理 USB 事件

// MAVLink 解析配置
#define MAVLINK_MAX_PAYLOAD_LEN  255   // MAVLink v2 最大 payload
#define MAVLINK_PARSER_STACK     4096  // MAVLink 解析任务栈

// 数据超时配置
// 如果超过此时间未收到有效位置数据, 标记为 STALE
#define FC_DATA_TIMEOUT_MS     2000

// MAVLink TX (USB 发送) — 通过 USB 向飞控发送 MAVLink 命令
// 当前仅用于发送 ARM/DISARM 联锁命令 (MAV_CMD_COMPONENT_ARM_DISARM)
// 警告: 如果飞控 USB 口也是烧录口，MAVLink TX 可能干扰飞控正常工作
// 设为 0 禁用 MAVLink TX，仅使用 GPIO6 硬件联锁（推荐先禁用测试）
// 设为 1 启用 MAVLink TX（需要确认飞控兼容后再开启）
#define MAVLINK_TX_ENABLED 0

// MAVLink 连续 CRC 失败阈值 — 超过此值触发 USB 恢复
// 正常运行时约 47% 的帧通过 CRC，但有效帧间最多几十个未知帧
// 200 个连续失败 ≈ 约 1 秒无任何已知消息类型通过，表明数据流损坏
#define MAVLINK_CONSECUTIVE_CRC_LIMIT 200

// USB 恢复冷却时间 (毫秒) — 防止反复重连
#define USB_RECOVERY_COOLDOWN_MS 5000

#endif // CONFIG_H
