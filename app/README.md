# RID 模块检测工具（桌面软件）

对 ESP32-S3 远程识别广播模块（GB 46750-2025）的 PC 端检测软件。经典 Win7/WinX 风格界面。

## 功能

| # | 功能 | 说明 |
|---|------|------|
| 1 | 开启电脑蓝牙并扫描 | 主动扫描，匹配模块广播（Service Data / UUID `0x0D50`，名称 `ESP32S3_RID`） |
| 2 | 接收模块信号 | 接收广播并解析出 GB 46750-2025 数据包 |
| 3 | 解码 + 内置判断器 | 逐字段解码；自动检查结构/字段/速率/新鲜度，判定模块“正常 / 警告 / 故障” |
| 4 | 串口提取内部数据 | 通过 `DUMP` 协议导出模块 Flash 中的飞行日志 → CSV |

另提供 **“粘贴 HEX 解码”**：从任何抓包工具复制数据包十六进制即可手动解码自检，
不依赖系统 BLE 栈解析（BLE 5 扩展广播在部分 Windows 版本上 service_data 可能拿不到）。

## 安装

```bash
cd app
python -m venv .venv
.venv\Scripts\activate        # Windows
pip install -r requirements.txt
```

## 运行

```bash
python main.py
```

> 依赖缺失时程序仍会启动，只是对应标签页给出安装提示。

## 使用

### BLE 接收 / 解码 / 自检

1. 给模块上电，模块通过 USB 连接飞控（或单独上电）。
2. 点 **开始扫描**。
3. 左侧“内置判断器”实时给出判定与问题清单；中间表格为解码后的 GB 字段。
4. 右上角设备下拉框可切换已发现的模块（按 MAC 区分）。

判断器判定逻辑：
- **结构**：dataType/version/dataLength 是否合规
- **字段**：位置/高度/速度/航向/运行状态范围，唯一产品识别码与实名登记号格式
- **速率**：GB 5.1.3 要求广播 ≥1 包/s，低于则告警
- **新鲜度**：超过 2s 无新数据告警，超过 5s 判故障
- **内容停滞**：长时间收不到内容变化提示数据源异常

> 验收时的 BLE 5 说明：模块使用 BLE 5 **扩展广播**（载荷超过传统 31 字节上限），
> 电脑必须带 BLE 5 收音器才能看到。注意：当前接收测试电脑的蓝牙适配器为 Realtek
> RTL8821CE（BT 4.2，USB\VID_0BDA&PID_C024），不支持 BLE 5 扩展广播，无法直接
> 接收本模块广播；请在 BT 5.0+ 适配器上验证，或用下方“粘贴 HEX 解码”兜底。

### 串口飞行日志提取

1. 用 USB 线把模块 **COM 口**（不是 USB 口）连电脑。
2. 选串口与波特率（默认 115200），点 **导出飞行日志**。
3. 选择保存路径，等待完成。CSV 含全部 GB 字段 + CRC 有效性列。

## 打包成 exe

```bash
.venv\Scripts\python -m pip install pyinstaller
cd app
.venv\Scripts\python -m PyInstaller --noconfirm --onefile --windowed ^
  --name RIDCheck --collect-all bleak --collect-all winrt main.py
```

产物：`app/dist/RIDCheck.exe`（单文件，开箱即用，可自行改名）。

> **说明**：
> - bleak 3.x 在 Windows 走 WinRT 运行时，`--collect-all winrt` 会把
>   `winrt-runtime` 的 C 扩展和全部 `winrt.windows.*` 子模块一起打进 exe，
>   避免运行时报“找不到 winrt 模块”。
> - `--onefile` 启动时会在临时目录解压，首次启动稍慢（几百 MB 的 Qt）。
> - 若你只在没装 Python 的机器上用串口功能，BLE 又失败，可退而求其次：
>   把蓝牙扫描交给蓝牙 LE 工具，本程序专注“HEX 粘贴解码 + 串口导出”。

## 目录结构

```
app/
  main.py              # 入口
  requirements.txt
  rid/                 # 后端（纯逻辑，可单测）
    decoder.py         # GB 46750 数据包解码（与固件 rid_messages.cpp 一致）
    health.py          # 内置判断器
    ble_scanner.py     # bleak 扫描 + 0x0D50 载荷提取
    serial_dump.py     # DUMP 协议 + CSV 导出
    models.py          # 数据模型
  gui/                 # PySide6 界面
    main_window.py
    ble_panel.py
    serial_panel.py
    workers.py         # QThread 后台任务
```

## 相关

- 解码与字段顺序与固件 `main/protocol/rid_messages.cpp` 一致（版本字节 `0x20` = V1.0）
- 数据包在广播中的位置：AD Service Data（类型 `0x16`）→ UUID `0x0D50` → 原始 GB 数据包
- 命令行版串口导出工具见 `../tools/flight_log_dump.py`
