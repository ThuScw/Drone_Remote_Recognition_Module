# USB 设备连接指南（即插即用 vs 指定设备）

## 两种模式

### 模式 1：即插即用模式（计划中，未实现）🚧

> **注意**: 此模式尚未实现。当前固件仅支持"指定设备模式"（模式 2），必须设置 VID/PID。
> 设置 `FC_USB_VID=0` 会导致编译时返回 `ESP_ERR_NOT_SUPPORTED`。
> 如需此功能请参考下方工作原理图自行实现 USB 设备枚举逻辑。

**配置**:
```c
#define FC_USB_VID          0   // 0 = 自动检测（暂未实现）
#define FC_USB_PID          0   // 0 = 自动检测（暂未实现）
```

**工作原理**:
1. ESP32-S3 自动扫描所有连接的 USB 设备
2. 查找 CDC-ACM 类设备（USB 串口设备）
3. 自动连接到第一个找到的 CDC-ACM 设备
4. 开始读取 MAVLink 数据

**使用步骤**:
```
1. 烧录固件
2. 用 USB 线连接飞控到 ESP32-S3 的 "USB" 口
3. 上电
4. 自动开始工作！无需任何配置
```

**适用场景**:
- 只有一个飞控连接
- 不确定飞控的 VID/PID
- 快速测试和原型开发
- 多架无人机使用不同飞控

**支持的设备**:
- 所有 CDC-ACM 类 USB 串口设备
- 大部分飞控（Pixhawk、PX4、Betaflight 等）
- CH340、CP2102 等 USB 转串口芯片

---

### 模式 2：指定设备模式（当前唯一可用模式）✅

**配置**:
```c
#define FC_USB_VID          0x1B8C  // 用户飞控 VID（已确认）
#define FC_USB_PID          0x0036  // 用户飞控 PID
```

**工作原理**:
1. ESP32-S3 只查找匹配 VID/PID 的设备
2. 找到后连接，未找到则持续重试
3. 忽略其他 USB 设备

**使用步骤**:
```
1. 查找飞控的 VID/PID（见下方方法）
2. 修改 config.h
3. 烧录固件
4. 连接飞控
5. 自动开始工作
```

**适用场景**:
- 多 USB 设备同时连接，需要选择特定设备
- 安全要求高的场景（防止误连其他设备）
- 量产产品（固定飞控型号）

---

## 如何查找 VID/PID

### Windows

1. **连接飞控到电脑**
2. **打开设备管理器**
   - Win + X → 设备管理器
3. **找到飞控设备**
   - 通常在"端口 (COM 和 LPT)"或"通用串行总线设备"下
   - 可能显示为 "USB Serial Device" 或 "ArduPilot" 等
4. **查看硬件 ID**
   - 右键设备 → 属性 → 详细信息
   - 下拉选择"硬件 ID"
   - 本机飞控显示: `USB\VID_1B8C&PID_0036&REV_0101`
   - VID = 0x1B8C, PID = 0x0036

### Linux

```bash
# 连接飞控后运行
lsusb

# 输出示例:
# Bus 001 Device 002: ID 1B8C:0036 Flight Controller
#       ↑              ↑    ↑
#     总线号          VID  PID
#
# VID = 0x1B8C, PID = 0x0036
```

### macOS

```bash
# 连接飞控后运行
system_profiler SPUSBDataType

# 找到你的飞控，查看:
# Vendor ID: 0x1B8C
# Product ID: 0x0036
```

### 通过串口日志

如果飞控已经连接到 ESP32-S3，开启详细日志：
```c
#define CONFIG_RID_VERBOSE_LOG 1
```

串口输出会显示：
```
I (1234) FLIGHT_DATA: Trying to open USB device (VID=0x1B8C, PID=0x0036)
I (1235) FLIGHT_DATA: USB device opened (VID=0x1B8C, PID=0x0036)
I (1236) FLIGHT_DATA: USB device configured (DTR/RTS cleared for FC safety) — waiting for MAVLink data...
```

---

## 常见问题

### ⚠️ Q0: 飞控连接后无人机失控怎么办？

**重要安全警告**：ESP32-S3 作为 USB Host 打开飞控 USB 口时，**DTR/RTS 控制线可能触发飞控复位或进入 bootloader 模式**。如果你的飞控 USB 口的 DTR 连到了 MCU 的 BOOT0/NRST 引脚，连接时会导致飞行中失控。

**本固件的防护措施**：
1. **显式清除 DTR/RTS**：`cdc_acm_host_set_control_line_state(dev, false, false)` 在打开设备后立即执行
2. **只读模式**：`MAVLINK_TX_ENABLED=0` 时 `out_buffer_size=0`，USB CDC 以只读模式打开，不向飞控发送任何数据
3. **首次接入飞控务必先不装桨叶测试**，确认飞控灯正常、QGC 能正常控制后再飞行

### Q1: 即插即用模式会连接错误的设备吗？

**不会**。CDC-ACM 驱动只连接 USB 串口类设备，不会连接鼠标、键盘、U盘等。

如果你的飞控同时连接了电脑，ESP32-S3 也能检测到（USB 支持多主机）。

### Q2: 同时连接多个 USB 设备怎么办？

**即插即用模式**: 自动连接第一个检测到的 CDC-ACM 设备

**指定设备模式**: 只连接匹配 VID/PID 的设备，忽略其他

### Q3: 热插拔支持吗？

**支持！** 
- 拔掉飞控 → ESP32 自动检测断开
- 重新插入 → ESP32 自动重新连接
- 无需重启

### Q4: 飞控需要先上电还是 ESP32 先上电？

**都可以**。
- ESP32 会持续扫描 USB 设备（每 2 秒一次）
- 无论谁先上电，最终都会自动连接

### Q5: 连接失败怎么办？

**检查清单**:
1. ✅ USB 线是数据线（不是纯充电线）
2. ✅ 飞控连的是 "USB" 口（不是 "COM" 口）
3. ✅ 飞控已上电
4. ✅ 飞控输出的是 MAVLink 协议（不是其他协议）
5. ✅ 开启详细日志查看错误信息

---

## 工作原理图

### 即插即用模式

```
ESP32-S3 (USB Host)
    │
    ├─ 枚举所有 USB 设备
    │
    ├─ 设备1: 鼠标 (class=0x03) → 跳过
    │
    ├─ 设备2: CDC-ACM (class=0x02) → ✓ 连接！
    │
    └─ 开始读取 MAVLink 数据
```

### 指定设备模式

```
ESP32-S3 (USB Host)
    │
    ├─ 查找 VID=0x1B8C, PID=0x0036
    │
    ├─ 设备1: VID=0x1234 → 不匹配，跳过
    │
    ├─ 设备2: VID=0x1B8C, PID=0x0036 → ✓ 匹配！连接
    │
    └─ 开始读取 MAVLink 数据
```

---

## 代码实现细节

### 自动检测逻辑

```c
// 1. 枚举所有 USB 设备
usb_host_get_device_list(&num_devices, dev_infos);

// 2. 遍历检查每个设备
for (size_t i = 0; i < num_devices; i++) {
    // 获取设备描述符
    usb_host_get_device_descriptor(dev_handle, &dev_desc);
    
    // 3. 检查是否是 CDC-ACM 设备
    if (dev_desc->bDeviceClass == 0x02 ||  // CDC
        dev_desc->bDeviceClass == 0xFF ||  // Vendor-specific
        dev_desc->bDeviceClass == 0xEF) {  // Composite
        // 4. 尝试连接
        cdc_acm_host_open(dev_desc->idVendor, dev_desc->idProduct, ...);
    }
}
```

### 支持的设备类型

| 设备类 | Class Code | 说明 | 是否支持 |
|--------|-----------|------|---------|
| CDC | 0x02 | 通信设备（USB 串口） | ✅ |
| CDC-Data | 0x0A | CDC 数据接口 | ✅ |
| Vendor-Specific | 0xFF | 厂商自定义（很多飞控用这个） | ✅ |
| Composite | 0xEF | 复合设备（多功能） | ✅ |
| HID | 0x03 | 鼠标/键盘 | ❌ |
| Mass Storage | 0x08 | U盘 | ❌ |

---

## 推荐配置

> **当前状态**: 仅支持指定设备模式。即插即用（VID=0）尚未实现。

### 开发测试阶段 & 量产阶段
```c
#define FC_USB_VID          0x1B8C  // 本机飞控（已确认，所有同型号无人机一致）
#define FC_USB_PID          0x0036
```

### 多飞控环境
```c
// 每架无人机烧录不同的固件，指定各自的 VID/PID
#define FC_USB_VID          0x1B8C  // 无人机 A
#define FC_USB_PID          0x0036

// 或保持即插即用，通过 UAS_ID 区分
#define UAS_ID "DRONE_A_001"
```

> 常见飞控 VID/PID 参考（config.h 内注释）：
> Pixhawk/Cube (ArduPilot) `0x1209/0x5740`、PX4 `0x26AC/0x0011`、Betaflight `0x0483/0x5740`、
> 通用 CDC-ACM `0x303A/0x4001`、CH340 `0x1A86/0x7523`、CP2102 `0x10C4/0xEA60`。

---

**最后更新**: 2026-08-01  
**适用版本**: ESP-IDF v5.5.5+

---

## 相关功能

- **飞行日志导出**：通过 UART0 的 `DUMP` 命令导出飞行日志，详见 [`tools/flight_log_dump.py`](../tools/flight_log_dump.py)
- **MAVLink TX 安全开关**：`config.h` 中 `MAVLINK_TX_ENABLED=0` 时 USB CDC 只读模式，`out_buffer_size=0`，且打开后显式清除 DTR/RTS
