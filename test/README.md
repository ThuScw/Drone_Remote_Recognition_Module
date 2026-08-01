# ESP32-S3 无人机远程识别模块 — PC端测试套件

## 概述

本目录包含三个核心逻辑模块的PC端测试，无需ESP32硬件、无需ESP-IDF，只需一台装有g++的电脑即可运行。

| 被测模块 | 测试文件 | 覆盖内容 |
|----------|----------|----------|
| CRC-16/MCRF4XX | `test_crc.cpp` | CRC算法正确性、MAVLink CRC extra byte常量 |
| MAVLink解析器 | `test_parser.cpp` | v1/v2帧解析、字段提取、CRC校验、超时检测、签名帧、未知msgid、压力测试（粘包/丢字节/乱码洪流/位翻转） |
| GB 46750-2025编码 | `test_rid_messages.cpp` | 数据包构建、字段编码公式、M/O字段语义、范围验证、新鲜度 |

## 快速开始

### Windows（推荐使用MSYS2或MinGW-w64）

双击 `run.bat`，自动编译并运行。

或手动执行：
```bash
g++ -std=c++14 -Wall -Wextra -g -D_USE_MATH_DEFINES \
    -I../main -I../main/data -I../main/protocol -I./stubs -I./stubs/driver \
    -o tests.exe \
    test_main.cpp test_crc.cpp test_parser.cpp test_rid_messages.cpp \
    ../main/data/mavlink_crc.cpp \
    ../main/data/mavlink_parser.cpp \
    ../main/protocol/rid_messages.cpp
./tests.exe
```

### Linux / macOS / MSYS2

```bash
cd test
make
make run    # 或直接 ./tests
```

### 预期输出

```
ESP32-S3 RID -- Host Test Suite

--- CRC-16/MCRF4XX ---
--- MAVLink Parser ---
--- GB 46750-2025 Protocol ---

=== ALL TESTS PASSED ===
```

## 目录结构

```
test/
├── stubs/
│   ├── esp_log.h          # ESP_LOG宏 → fprintf（测试可见输出）
│   ├── esp_timer.h        # esp_timer_get_time() → 可控全局变量
│   ├── config.h           # 测试用常量（超时阈值、mock坐标等）
│   ├── driver/
│   │   └── gpio.h         # 空桩（满足编译依赖）
│   └── esp_bt.h           # 蓝牙枚举桩
├── test_main.cpp          # 极简测试框架（CHECK / CHECK_EQ / CHECK_CLOSE）
├── test_crc.cpp           # CRC算法测试（6项）
├── test_parser.cpp        # MAVLink解析测试（21项）
├── test_rid_messages.cpp  # GB 46750-2025合规测试（21项）
├── Makefile               # Linux/macOS/MSYS2构建脚本
├── run.bat                # Windows双击运行脚本
└── README.md              # 本文件

tools/
└── flight_log_dump.py     # PC端飞行日志DUMP导出工具（Python + pyserial）
```

## 测试用例清单

### test_crc.cpp — CRC算法正确性

| 编号 | 测试项 | 说明 |
|------|--------|------|
| 1 | 空缓冲CRC | 空输入返回初始值0xFFFF |
| 2 | 确定性 | 相同输入→相同输出 |
| 3 | 区分性 | 不同输入→不同输出 |
| 4 | 非零输出 | 非空数据CRC不为0 |
| 5 | CRC extra byte | HEARTBEAT(50)、GPS_RAW_INT(24)、ATTITUDE(39)、HIGHRES_IMU(93)、HOME_POSITION(242→104)、SYSTEM_TIME(137)等9个已知值 |
| 6 | 往返验证 | 合成HEARTBEAT帧，CRC计算→CRC_EXTRA累积，结果非平凡且确定 |

### test_parser.cpp — MAVLink解析正确性

测试数据 `test_data.h` 由真实飞控 .DAT 日志抽取，expected 值由 pymavlink 独立解码（非 C 代码转写），共 1899 帧。

| 编号 | 测试项 | 说明 |
|------|--------|------|
| 1 | 全量帧CRC | 1899帧全部CRC通过，crcErrors=0 |
| 2 | HEARTBEAT解析 | armed标志、systemStatus提取 |
| 3 | GLOBAL_POSITION_INT | lat/lon/alt/vel/hdg全部浮点精度验证 |
| 4 | GPS_RAW_INT | gpsFixType、gpsSats提取 |
| 5 | 完整fillFlightData | 真实v2帧组合，全部字段正确填充，validMask=FLD_ALL（不含BARO_ALT） |
| 6 | CRC错误检测 | 篡改CRC→crcErrors递增，frame不计入 |
| 7 | 连续错误复位 | 合法帧后consecutiveCrcErrors归零 |
| 8 | 无效STX | 随机字节不触发解析 |
| 9 | 数据超时 | 超过阈值时isDataStale返回true |
| 10 | 恢复判断 | 无有效帧不触发恢复；连续CRC错误超阈值才触发 |
| 11 | 状态摘要 | mavlink_getStatus输出非空 |
| 12 | GPS fix过滤 | fix<2时仍输出数据，标记为FRESH_STALE+validationFlags bit0 |
| 13 | VFR_HUD短帧 | 真实帧16-20B全覆盖；16-17B截断帧解码groundspeed/climb且heading保持NAN（不越界读CRC字节）；≥18B帧heading验证 |
| 14 | HOME_POSITION | msgid=242帧home坐标提取验证（105是HIGHRES_IMU，不是HOME_POSITION） |
| 15 | 未知msgid帧 | 无CRC extra的未知msgid不计入CRC错误，避免假恢复风暴 |
| 16 | 签名帧 | MAVLink v2签名帧（13字节签名块）正常解析，CRC验证正确 |
| 17 | SYSTEM_TIME | 真实11B变体（boot_ms低3字节）与标准12B均正确解码unixUsec/bootMs |
| 18 | 粘包重入 | 连续两帧无间隔送入，解析器正确拆出2帧（CRC无误、字段齐全），不误并帧 |
| 19 | 丢字节/截断重同步 | 帧中途丢1字节+噪声/截断至一半，解析器跳过坏帧后从下一STX重新同步，后续合法帧全数恢复 |
| 20 | 随机乱码洪流 | 3000字节随机噪声+30合法帧：未知msgid假帧不误报CRC错误、不触发假恢复，合法帧≥20恢复；限制噪声（排除0xFD/0xFE）时10帧全数解析、crcErrors=0 |
| 21 | payload位翻转 | 合法帧payload翻转1bit → CRC拒绝、totalFrames=0、consecutiveCrcErrors=1 |

### test_rid_messages.cpp — GB 46750-2025合规性

| 编号 | 测试项 | 对应国标 |
|------|--------|----------|
| 1 | 包结构自检 | Section 5.2.1 — dataType=0xFF, version=0x20 (V1.0=0b001_00000), totalLen自洽 |
| 2 | packetVerify校验 | 结构校验函数正确性 |
| 3 | serialize序列化 | 序列化输出非空 |
| 4 | **M字段dataId恒为1** | Section 5.2.3 Table 2 — 数据不可用时编码为表3未知哨兵值（位置0xFFFFFFFF、航迹/速度0xFFFF）而非省略 |
| 5 | **O字段条件出现** | Section 5.2.3 — REL_HEIGHT、VERT_SPEED、BARO_ALT仅在validMask允许时出现 |
| 6 | 经纬度编码 | Table 3-008 — deg×1e7, int32 LE, 读回验证 |
| 7 | 高度编码 | Table 3-013 — (alt+1000)×2, 分辨率0.5m, 100m和-500m两点验证 |
| 8 | 相对高度编码 | Table 3-011 — (h+9000)×2, 分辨率0.5m |
| 9 | 航迹角编码 | Table 3-009 — deg×10, 0~3599 |
| 10 | 地速编码 | Table 3-010 — m/s×10, 分辨率0.1m/s |
| 11 | 垂直速度编码 | Table 3-012 — bit7方向+bit6-0绝对值×2, 上升/下降双验证 |
| 12 | 时间戳编码 | Table 3-020 — 6字节LE, Unix ms |
| 13 | 范围验证 | 经纬度±90/±180、高度-1000~10000边界、无效状态检测 |
| 14 | 新鲜度检查 | FRESH_OK/STALE/INVALID分级, 多字段混合、空数据边界 |
| 15 | 负纬度编码 | 南半球(Sydney -33.8688°)正确处理 |
| 16 | OpStatus字节位置 | content中运行状态字节偏移正确 |
| 17 | 缺M字段 | dataId位仍置1，数值编码为表3未知哨兵值（合规关键） |
| 18 | 精度字段 | HORIZ_ACC、VERT_ACC、SPEED_ACC出现在content中 |
| 19 | 未知哨兵值 | Table 3 — validMask=0 时 006/008位置→0xFFFFFFFF、009/010→0xFFFF、高度/时间戳/状态→0 |
| 20 | golden包字节级验证 | 固定输入下逐字节核对序列化输出（header/dataId/全部content字段） |
| 21 | **独立解码** | 表3硬编码偏移的独立解码器反向解析golden包，打破编解码自洽闭环 |

## GB 46750-2025 字段编码公式对照

以下编码公式在测试中逐项验证（参考 GB 46750-2025 Table 3）：

| 字段 | 编码公式 | 字节数 | 测试编号 |
|------|----------|--------|----------|
| 纬度/经度 | `deg × 1e7` → int32 LE | 4+4 | #6, #15 |
| 大地高度 | `(alt + 1000) × 2` → uint16 LE | 2 | #7 |
| 相对高度 | `(h + 9000) × 2` → uint16 LE | 2 | #8 |
| 航迹角 | `deg × 10` → uint16 LE | 2 | #9 |
| 地速 | `m/s × 10` → uint16 LE | 2 | #10 |
| 垂直速度 | bit7方向 + bit6-0 `abs(m/s)×2` | 1 | #11 |
| 时间戳 | Unix ms → uint48 LE | 6 | #12 |

## 信任度验证

测试本身的正确性可通过以下方式验证：

### 方法一：故意破坏（推荐）

1. 打开 `main/data/mavlink_crc.cpp`，将 HEARTBEAT 的 CRC extra 从 50 改为 0
2. 运行 `make && ./tests`
3. **预期**：test_parser #1（HEARTBEAT解析）因CRC校验失败而FAIL
4. 改回原值，测试恢复PASS

### 方法二：边界验证

- 故意传入lat=91（超出±90范围），确认范围验证测试FAIL
- 故意传入负速度（-5 m/s），确认范围验证捕获该异常

### 方法三：交叉验证

- CRC extra byte值与MAVLink官方common.xml一致
- 编码公式与GB 46750-2025 Table 3原文可逐项对照

## 不与烧录代码冲突

- 测试代码位于独立 `test/` 目录
- `CMakeLists.txt` 不引用test目录，ESP-IDF构建完全忽略
- stub头文件仅在PC编译时使用（通过 `-I./stubs` 优先搜索）
- 生产代码零修改、零侵入

## 依赖

- g++ 5.1+ 或 clang++ 3.5+（需支持C++14）
- 无第三方库依赖
- 无需ESP-IDF、无需硬件
