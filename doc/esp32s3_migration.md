# ESP32-C5 → ESP32-S3 迁移指南

## 概述

本指南说明如何将远程识别广播模块从 ESP32-C5 迁移到 ESP32-S3，以支持 USB Host 模式直接读取飞控数据。

## 主要变更

### 1. 硬件接口变化

| 特性 | ESP32-C5 | ESP32-S3 |
|------|----------|----------|
| **数据接口** | UART (GPIO4 RX) | USB Host (GPIO19/20) |
| **连接方式** | 飞控 TELEM TX → GPIO4 | 飞控 USB → USB OTG 口 |
| **调试接口** | USB Serial/JTAG | COM 口 (UART0) |
| **状态 LED** | GPIO27 | GPIO48 |
| **联锁** | GPIO6 | MAVLink 联锁（无专用引脚） |

### 2. 软件变更

#### 2.1 `config.h` 修改

**删除**:
- UART 相关配置 (`FC_UART_PORT_NUM`, `FC_UART_RX_GPIO`, `FC_UART_TX_GPIO` 等)

**新增**:
- USB Host CDC-ACM 配置 (`FC_USB_VID`, `FC_USB_PID`, `FC_USB_BAUD_RATE` 等)
- USB Host 任务配置 (`USB_HOST_TASK_STACK`, `USB_HOST_TASK_PRIO`)
- MAVLink TX 安全开关 (`MAVLINK_TX_ENABLED`, 默认 0=只读模式)
- 更新 GPIO 引脚定义 (LED 从 GPIO27 → GPIO48)
- 飞行日志存储配置 (GB 46750-2025 5.1.8)：Flash 分区、记录间隔、环形缓冲区
- BLE TX 功率配置 (`BLE_TX_POWER_LEVEL`)，ESP32-S3 使用 ESP_PWR_LVL_P9 (+9 dBm)

#### 2.2 `flight_data.cpp` 重写

**ESP32-C5 版本** (UART):
```cpp
#include "driver/uart.h"

static void uart_rx_task(void* arg) {
    while (true) {
        int len = uart_read_bytes(UART_NUM_1, rxbuf, sizeof(rxbuf), ...);
        for (int i = 0; i < len; i++) {
            mavlink_parseByte(s_parser, rxbuf[i], nowMs);
        }
    }
}
```

**ESP32-S3 版本** (USB Host):
```cpp
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

// 解析器 + USB 句柄封装为类成员（替代文件级全局 s_parser）
class FlightDataSource {
    MavlinkParser _parser;                  // 成员解析器
    portMUX_TYPE  _parserMux;               // 解析器状态锁（数据回调 <-> 主循环）
    // volatile: 跨任务共享 (主循环 <-> USB host 任务)，防止寄存器缓存读到过期值
    volatile cdc_acm_dev_hdl_t _cdcDev = nullptr;
    volatile bool _deviceConnected = false;
    volatile bool _deviceReady = false;
    SemaphoreHandle_t _devMutex;            // USB 句柄生命周期互斥锁
};

// 打开设备 — 用局部变量接收句柄（volatile 成员不能取地址）
cdc_acm_dev_hdl_t newDev = nullptr;
esp_err_t ret = cdc_acm_host_open(FC_USB_VID, FC_USB_PID, 0, &dev_config, &newDev);
if (ret == ESP_OK) {
    configureLineCoding(newDev);
    // 显式清除 DTR/RTS，防止飞控被复位
    cdc_acm_host_set_control_line_state(newDev, false, false);
    // 配置完成后在互斥锁内发布句柄，避免与 DISCONNECT/recovery 竞态
    xSemaphoreTake(_devMutex, portMAX_DELAY);
    _cdcDev = newDev; _deviceConnected = true; _deviceReady = true;
    xSemaphoreGive(_devMutex);
}

// MAVLINK_TX_ENABLED=0 时 out_buffer_size=0，只读模式
dev_config.out_buffer_size = MAVLINK_TX_ENABLED ? 512 : 0;

// USB 数据回调 — 在 _parserMux 临界区内喂字节
static bool usbDataCb(const uint8_t* data, size_t data_len, void* arg) {
    auto* self = static_cast<FlightDataSource*>(arg);
    portENTER_CRITICAL_SAFE(&self->_parserMux);
    for (size_t i = 0; i < data_len; i++) {
        mavlink_parseByte(self->_parser, data[i], nowMs);
    }
    portEXIT_CRITICAL_SAFE(&self->_parserMux);
    return true;
}
```

**关键变化**:
- 从轮询读取 UART 改为 USB 回调驱动
- 新增 USB 设备连接/断开事件处理
- 新增自动重连机制 (每 2 秒尝试一次)
- 解析器改为类成员 + `_parserMux` 临界区，替代文件级全局 `s_parser`
- `_cdcDev/_deviceConnected/_deviceReady` 标记 `volatile`（跨任务共享）
- `_devMutex` 互斥锁保护 USB 句柄生命周期：防止 `tryUsbRecovery`（主循环）与 DISCONNECT 事件回调（USB host 任务）并发 close 同一句柄导致双重 close / use-after-free
- USB host 任务注册任务看门狗（死锁时系统复位而非静默断流）
- **CRC 风暴恢复**：连续 200 次 CRC 校验失败 → `tryUsbRecovery()` 关闭 USB 设备 + 重置解析器 + 重连（5s 冷却防止反复重连）

#### 2.3 `sdkconfig.defaults` 更新

**新增 USB Host 配置**:
```ini
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=256
CONFIG_USB_HOST_CDC_ACM=y
CONFIG_USB_HOST_CDC_ACM_DATA_BUF_SIZE=1024
```

#### 2.4 `CMakeLists.txt` 更新

**新增 USB 和存储依赖**:
```cmake
REQUIRES ... usb spi_flash wear_levelling
INCLUDE_DIRS "." "broadcast" "protocol" "broadcaster" "data" "indicators" "logging" "console"
```

**新增模块**:
- `logging/` — FlightLog 飞行数据持久化（环形缓冲区 + 异步磨损均衡写入）
- `console/` — ConsoleCmd UART 命令行监听（DUMP 飞行日志导出）

## 构建步骤

### 1. 清理旧的构建缓存

```bash
# 删除旧的 build 目录 (重要！)
rm -rf build
```

### 2. 设置目标芯片

```bash
idf.py set-target esp32s3
```

### 3. 配置项目

```bash
idf.py menuconfig
```

**需要确认的配置**:
- USB Host 已启用 (Component config → USB Host Support)
- CDC-ACM 驱动已启用
- BLE 5.0 Extended Advertising 已启用

### 4. 编译

```bash
idf.py build
```

### 5. 烧录

**使用 COM 口** (不是 USB 口):
```bash
idf.py -p COM端口 flash monitor
```

**注意**: 
- ESP32-S3-DevKitC-1 有两个 USB-C 口
- 烧录时使用标记为 **"COM"** 的口（连接电脑）
- **"USB"** 口用于连接飞控（不要插电脑！）

### 6. 连接飞控

```
飞控 USB 口 ──→ USB 线 ──→ ESP32-S3 "USB" 口
                              (GPIO19/20, USB OTG)
```

## VID/PID 配置

### 用户环境（已确认）

```c
// config.h
#define FC_USB_VID          0x1B8C
#define FC_USB_PID          0x0036
```

所有同型号无人机飞控的 VID/PID 一致，通过设备管理器确认：
```
USB\VID_1B8C&PID_0036&REV_0101
```

### 其他飞控的 VID/PID

**Windows**:
1. 连接飞控到电脑
2. 打开设备管理器
3. 找到飞控设备，右键 → 属性 → 详细信息
4. 选择"硬件 ID"，查看 VEN_XXXX & DEV_XXXX
5. VEN = VID, DEV = PID (十六进制)

**Linux**:
```bash
lsusb
# 输出示例: Bus 001 Device 002: ID 1209:5740 ArduPilot
# VID = 0x1209, PID = 0x5740
```

### 修改 config.h

```c
#define FC_USB_VID          0x1209  // 替换为你的飞控 VID
#define FC_USB_PID          0x5740  // 替换为你的飞控 PID
```

## 常见问题

### Q1: 编译报错 "usb/usb_host.h: No such file"

**解决**: 确保 `sdkconfig.defaults` 中启用了 USB Host:
```ini
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=256
```

### Q2: 运行时日志显示 "USB device not found"

**可能原因**:
1. VID/PID 配置错误 → 重新查找并修改
2. 飞控未上电 → 先给飞控供电
3. USB 线问题 → 换一根数据线（不是充电线）
4. 插错口 → 确保飞控连的是 "USB" 口，不是 "COM" 口

### Q3: 连接飞控后无人机失控或飞控异常

**原因**：飞控 USB 口的 DTR 线可能连接到 MCU 的 BOOT0 或 NRST 引脚。USB Host 打开设备时断言 DTR 会触发飞控复位。

**防护措施**：
1. 本固件已在 `tryOpenUsbDevice()` 中显式清除 DTR/RTS
2. `MAVLINK_TX_ENABLED=0` 时 USB CDC 以只读模式打开（`out_buffer_size=0`）
3. **首次接入必须不装桨叶测试**：监听串口确认 `DTR/RTS cleared for FC safety` 日志

### Q4: USB 设备连接了但收不到 MAVLink 数据

**检查**:
1. 飞控是否输出 MAVLink（而不是其他协议）
2. 波特率是否匹配（默认 115200）
3. 查看串口日志是否有 "USB RX" 输出
4. 开启详细日志 `CONFIG_RID_VERBOSE_LOG 1` 查看调试信息

### Q5: 可以同时连电脑和飞控吗？

**可以！**
- "COM" 口连电脑 → 调试、烧录
- "USB" 口连飞控 → 读取 MAVLink 数据
- 两个口独立工作，互不冲突

## 性能对比

| 指标 | ESP32-C5 (UART) | ESP32-S3 (USB Host) |
|------|----------------|---------------------|
| **数据延迟** | ~1ms | ~2-5ms |
| **CPU 占用** | 低 (中断驱动) | 中 (回调驱动) |
| **可靠性** | 高 | 高 |
| **接线复杂度** | 中 (需找 TX 引脚) | 低 (直接用 USB) |

## 下一步

1. 确认飞控 VID/PID，修改 `config.h`
2. 编译烧录
3. 连接飞控测试
4. 用 nRF Connect 验证 BLE 广播

## 回退到 ESP32-C5

如果需要回退，使用 Git:
```bash
git checkout -- main/config.h main/data/flight_data.cpp sdkconfig.defaults main/CMakeLists.txt
git checkout -- .  # 撤销所有修改
```

---

**最后更新**: 2026-08-01  
**适用版本**: ESP-IDF v5.5.5+
