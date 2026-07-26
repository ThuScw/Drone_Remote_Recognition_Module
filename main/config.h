#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// ================= 用户配置区 =================

// 唯一产品识别码 (GB 46860-2025 Section 4.1 — 厂商生产格式)
// 结构: 厂商识别码(4) + 产品型号代码(4) + 序列号(12) = 20字符 ASCII
// 字符范围: 0-9 及除 O/I 外的大写字母 A-Z
// 产品化时替换为 UOM 平台备案的真实编码
#define UAS_ID "CPNYMDL00123456789A"

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
#define HORIZ_ACC 10  // <10m
#define VERT_ACC  5   // <3m
#define SPEED_ACC 3   // <1m/s

// 时间戳精度 (GB 46750-2025 Table 3-021)
#define TS_ACC 5  // ≤0.1s

// 广播间隔 (毫秒) — 完整包发送周期
// GB 46750 5.1.3: 更新和发送间隔 ≤ 1s
#define BROADCAST_INTERVAL_MS 800

// BLE 底层广播间隔 (毫秒) — 影响功耗和被发现概率
// 值越小广播越密集 (功耗越高), 值越大越省电 (但接收方发现延迟增加)
// 根据 GB 46750 5.1.3, 每秒至少发送 1 次, 间隔 ≤ 1000ms
#define BLE_ADV_INTERVAL_MS 100

// 数据更新间隔 (毫秒) — 飞行数据刷新频率
// 独立于广播间隔，避免广播分片被打断
#define DATA_UPDATE_INTERVAL_MS 1000

// 自检间隔 (毫秒) — 全飞行周期持续监测
// GB 42590-2023 A.2.3.5.5: 全飞行周期持续监测模块状态
#define SELF_TEST_INTERVAL_MS 5000

// 任务看门狗超时 (毫秒) — 主循环卡死超过此时长触发系统复位
#define WATCHDOG_TIMEOUT_MS 5000

// ================= 模拟飞行数据 =================
// Stage 1: 模拟飞行循环验证广播链路
// Stage 2: 替换为 UART 飞控/GPS 真实数据 — 仅需实现 FlightData 填充

// 起飞点 (上海, WGS-84)
#define MOCK_LATITUDE       31.230416f
#define MOCK_LONGITUDE     121.473701f
#define MOCK_GEO_BASE_ALT  120.5f       // 地面大地高度 (m)

// 遥控站/操作员位置 (固定)
#define MOCK_OP_LAT         31.230500f
#define MOCK_OP_LON        121.473800f
#define MOCK_OP_ALT         10.0f

// 仿真阶段时长 (毫秒)
#define SIM_GROUND_WAIT_MS   5000       // 地面等待
#define SIM_TAKEOFF_MS      10000       // 起飞爬升
#define SIM_CRUISE_MS       40000       // 巡航飞行
#define SIM_LANDING_MS      10000       // 降落

// 巡航参数
#define SIM_CRUISE_ALT      50.0f       // 巡航高度 AGL (m)
#define SIM_CRUISE_SPEED    15.0f       // 巡航地速 (m/s)

// ================= 日志配置 =================

// 设为 1 开启详细日志 (hex dump, TX 详情, 调试信息)
// 量产固件应设为 0
#define CONFIG_RID_VERBOSE_LOG 0

// ================= GPIO 引脚分配 =================

// 飞控联锁引脚 (GB 46750-2025, 5.1.7a)
// 自检通过 → 拉高（飞控允许起飞），异常 → 拉低（飞控禁止起飞/执行处置）
// 飞控端需将此引脚配置为输入，低电平时拒绝解锁
// GPIO6 = MTMS (JTAG)，量产固件中 JTAG 已禁用，可安全使用
#define INTERLOCK_RID_OK_GPIO  GPIO_NUM_6
#define INTERLOCK_ACTIVE_LEVEL 1  // 1=高电平有效, 0=低电平有效

// 状态指示灯 (GB 46750-2025, 5.1.5)
// ESP32-C5-DevKitC-1 板载 WS2812B RGB LED，连接至 GPIO27，通过 RMT 外设驱动
// 绿色慢闪=待机, 蓝色快闪=广播中, 红色常亮=故障
#define STATUS_LED_GPIO        GPIO_NUM_27
#define STATUS_LED_NUM_LEDS    1

// ================= 飞行数据存储配置 (GB 46750-2025, 5.1.8) =================

// 记录间隔 (秒) — 标准要求 ≤10s
#define FLIGHT_LOG_INTERVAL_S   10

// Flash 分区名 (在 partitions.csv 中定义)
#define FLIGHT_LOG_PARTITION    "flight_log"

// 每条记录: 4B magic + 8B timestamp + 2B len + 数据
#define FLIGHT_LOG_MAGIC        0x5249444C  // "RIDL"

#endif // CONFIG_H
