# RID 检测（安卓手机端 APP）

对 ESP32-S3 远程识别广播模块（GB 46750-2025）的安卓检测 APP，用于现场抓包、监听、解码与合规判定。纯原生实现：**零 AndroidX、零第三方依赖**，全部界面用 `android.app.Activity` + 程序化 View 构建，APK 极小（约 866KB），侧载即用。

## 功能

| 功能 | 说明 |
|------|------|
| 实时抓包监听 | BLE 5 扩展广播扫描（Service UUID `0x0D50`），全接受扫描 + 回调内过滤；多设备按 MAC 区分 |
| 逐字段解码 | 按 GB 46750-2025 表 3 解析全部 21 项字段（含 M/O 可选标志与原始字节 HEX 对照） |
| 粘贴 HEX 解码 | 粘贴 nRF Connect 等工具抓到的完整广播帧或 GB 数据包，自动抽取后单包静态判定 |
| 合规判定 | 对照 GB 条款自动判定「正常 / 警告 / 故障」，附具体问题与条款编号 |
| 记录与分析 | RSSI / 速率 1Hz 采样曲线；生成 Word 合规报告（内嵌曲线图）；导出逐帧 CSV |
| 内容分享 | 文本 / CSV / Word 报告经系统分享面板发出（自建 FileProvider，无 AndroidX） |
| 内置说明页 | RID 简介、表 3 字段表、判定标准、使用方法 |

## 目录结构

```
app_android/
├── settings.gradle.kts        # 阿里云 Maven 镜像优先（国内网络加速，失败回退官方源）
├── build.gradle.kts
├── gradle/                    # Gradle Wrapper（腾讯镜像发行版）
├── local.properties           # 本机 sdk.dir（不入库，模板见 local.properties.example）
└── app/
    ├── build.gradle.kts       # versionName 1.4 / versionCode 5；minSdk 26 / targetSdk 34
    └── src/
        ├── main/
        │   ├── AndroidManifest.xml        # BLE 权限（12+ 用 BLUETOOTH_SCAN/CONNECT）
        │   └── java/com/ridcheck/
        │       ├── MainActivity.kt        # 主界面：设备列表 + 详情页 + 底部导航（746 行）
        │       ├── ble/
        │       │   └── BleScanner.kt      # BLE 扫描 + 0x0D50 提取 + 同包去重
        │       ├── core/                  # 纯逻辑，可 JVM 单元测试
        │       │   ├── Decoder.kt         # GB 46750 解码器（移植自 PC 版 decoder.py）
        │       │   ├── Health.kt          # 健康判定（单包 + 10s 流式窗口）
        │       │   ├── Models.kt          # 数据模型 + 表 3 字段表 + 采样/帧记录
        │       │   ├── DeviceRegistry.kt  # 多设备注册表 + 采样/逐帧存档
        │       │   ├── ReportBuilder.kt   # 文本报告 + 逐帧 CSV（公式注入防护）
        │       │   ├── DocxBuilder.kt     # 零依赖 Word(.docx) 生成器（OOXML）
        │       │   └── RidFileProvider.kt # 文件分享用自建 content URI
        │       └── ui/                    # 程序化 View（零 XML 布局）
        │           ├── Theme.kt           # 紫色系色板
        │           ├── DeviceListAdapter.kt
        │           ├── RidChartView.kt    # 实时采样曲线
        │           ├── ChartPng.kt        # 报告内嵌曲线位图
        │           ├── ShareUtil.kt       # 分享封装 + 导出文件清理
        │           └── ExplainPage.kt     # 说明页
        └── test/java/com/ridcheck/        # JVM 单元测试（36 个全部通过）
```

## 构建与安装

**工具链**（已装于本机，非管理员，全部便携版，环境变量持久化）：
- JDK 17：`C:\Users\86134\Tools\jdk-17.0.20+8`（`JAVA_HOME` 已设）
- Gradle 8.5：`C:\Users\86134\Tools\gradle-8.5`（已加入 PATH）
- Android SDK：`C:\Users\86134\AppData\Local\Android\Sdk`（platform-34、build-tools 34.0.0）
- `local.properties`：`sdk.dir=C:/Users/86134/AppData/Local/Android/Sdk`

```bash
cd app_android
gradle assembleDebug          # 构建 APK
gradle testDebugUnitTest      # 运行 JVM 单元测试
```

产物：`app_android/app/build/outputs/apk/debug/app-debug.apk`（约 866KB，debug 签名，侧载即用）。

安装：`adb install`（开启 USB 调试），或把 APK 复制到手机后点击安装（需允许未知来源）。

## 使用

1. 主界面点 **开始扫描**，信号源列表实时出现所有广播 UUID `0x0D50` 的设备（按 MAC 区分）。
2. 点某台设备进详情：查看逐字段解码、问题清单、RSSI / 速率曲线；可复制或分享原始 HEX。
3. 详情页可生成该设备 **Word 合规报告**，或导出 **历史采样 CSV**（Excel / WPS 可打开）。
4. **粘贴解码**：粘贴 nRF Connect 抓到的完整 Raw 广播帧、或从 `FF` 开始的 GB 数据包，做单包静态判定（不参与扫描统计）。

> 判定基准：GB 46750-2025 5.1.2 全程连续广播 / 5.1.3 广播间隔 ≤1s / 表 3 字段要求。判定结果仅供合规自查参考，不构成官方检测结论。

## 关键技术细节

- **BLE 5 扩展广播**：模块使用 BLE 5 Extended Advertising，Android 扫描必须 `ScanSettings.setLegacy(false)`，否则扩展广播包根本到不了回调（S3-v5.1 修复）。若某设备在 nRF Connect 可见但 App 扫不到，优先检查此项。
- **同包去重**：固件每个事件在 3 个信道重发同一数据包，扫描器按设备（MAC）记录最后原始包，相同包丢弃，避免重复计数。
- **多设备**：信号源按 MAC 注册，每台设备独立累积采样与逐帧存档（环形：采样 600 点 @1Hz、逐帧 30000 条），互不干扰；表 3 各字段「携带帧数」会话全程累计不随截断丢失。
- **CSV 公式注入防护**：非数值且以 `= + - @`（或制表/回车）开头的字段加前导单引号强制按文本读。
- **docx 控制字符剔除**：设备脏字节含 XML 1.0 非法控制字符时会被剔除，保证 Word / WPS 能打开。
- **导出文件清理**：分享目录只保留 7 天内文件，避免无限累积。

## 与 PC 版的关系

- 解码器 / 判定器 / 数据模型均**逐行移植**自 PC 版 `app/rid/decoder.py`、`health.py`、`models.py`，字节级行为一致（golden 向量对拍通过）。
- PC 版（`app/`）多出串口飞行日志 DUMP 导出；手机无 UART，故安卓版移除串口功能，改以 BLE 抓包 + 报告 / CSV 导出为主。
- 安卓版是**多设备**视角（每台独立判定），PC 版单设备表格 + 串口导出，二者互补。

## 版本历史

| 版本 | 提交 | 内容 |
|------|------|------|
| S3-v5.5 | 17fcf70 | 优化数据文件与报告文件内容（Word 报告、逐帧 CSV、内嵌曲线图） |
| S3-v5.4 | 9cb111d | 新增说明页；数据导出合并进单独信号源 |
| S3-v5.3 | 9f6a048 | 更新部分文字、新增内容复制 |
| S3-v5.2 | dbeb907 | 新增多设备查看 |
| S3-v5.1 | a7bd79e | 修复监听 BLE 扩展广播（setLegacy(false)） |
| S3-v5  | a75d0dd | 新增安卓抓包、监听、检测 APP |

## 测试

`app/src/test/java/com/ridcheck/` 下 **36 个 JVM 单元测试全部通过**（无需 Android 设备，`gradle testDebugUnitTest`）：

| 测试文件 | 数量 | 覆盖内容 |
|----------|------|----------|
| DecoderTest | 10 | 数据包解码、AD Service Data 提取、HEX 解析、广播帧抽包 |
| HealthTest | 9 | 单包判定、流式窗口（速率 / 停滞 / 冻结） |
| DeviceRegistryTest | 9 | 多设备注册、采样 / 帧存档截断、字段携带计数 |
| ReportBuilderTest | 8 | 文本报告、逐帧 CSV、Word 报告、公式注入防护 |

> 测试时钟可注入（`nowFunc`），流式判定（StreamAssessor）不依赖真实时间，纯 JVM 可测。
