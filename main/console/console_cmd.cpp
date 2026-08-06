#include "console_cmd.h"
#include "config.h"
#include "fault_log.h"
#include <cstdio>
#include <cstring>
#include <esp_log.h>

static const char* TAG = "CONSOLE";

void ConsoleCmd::init(FlightLog& flightLog) {
    static bool initialized = false;
    if (initialized) {
        ESP_LOGW(TAG, "ConsoleCmd already initialized — ignoring duplicate call");
        return;
    }
    initialized = true;

    auto* self = new ConsoleCmd();
    self->_flightLog = &flightLog;

    BaseType_t ret = xTaskCreate(taskFunc, "console_cmd", kTaskStack, self, kTaskPrio, &self->_taskHandle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
        delete self;
        initialized = false;
        return;
    }
    ESP_LOGI(TAG, "Console command listener ready (type DUMP to export flight log)");
}

void ConsoleCmd::taskFunc(void* param) {
    auto* self = static_cast<ConsoleCmd*>(param);

    // 注意: 本任务不订阅任务看门狗。阻塞式 fgets 空闲时无法喂狗, 若订阅 TWDT,
    // 模块会每 5s 触发 panic 复位。本任务为纯诊断用途, 卡死不影响 RID 合规广播
    // (GB 42590 A.2.3.5.5 由主/广播/日志任务的看门狗保障)。
    char line[kMaxCmdLen];

    while (true) {
        // 阻塞式 fgets 读取 stdin (UART0)。曾改用 select()+read() 以便空闲喂狗,
        // 但 select() 在 ESP32 console UART 上恒不报告可读 (IDFGH-2451), 导致
        // DUMP/HELP 命令永远不被处理 — 故回退到 fgets()。
        if (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                line[--len] = '\0';
            }
            if (len > 0) {
                self->handleCommand(line);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));  // stdin 关闭或错误 — 退避重试
        }
    }
}

void ConsoleCmd::handleCommand(const char* cmd) {
    if (strcmp(cmd, "DUMP") == 0) {
        dumpAllRecords();
    } else if (strcmp(cmd, "STATUS") == 0) {
        printStatus();
    } else if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "help") == 0) {
        printf("Commands: DUMP (export flight log), STATUS (fault counters), HELP\r\n");
        fflush(stdout);
    } else {
        printf("? Unknown: %s (type HELP)\r\n", cmd);
        fflush(stdout);
    }
}

void ConsoleCmd::printStatus() {
    char buf[384];
    faultLogFormat(buf, sizeof(buf));
    printf("%s\r\n", buf);
    fflush(stdout);
}

void ConsoleCmd::dumpAllRecords() {
    uint32_t maxRecs = _flightLog->getMaxRecords();
    uint32_t total   = _flightLog->getRecordCount();
    uint32_t available = (total < maxRecs) ? total : maxRecs;

    if (available == 0) {
        printf("+EMPTY\r\n");
        fflush(stdout);
        return;
    }

    // 静默期间抑制全部日志 (含 ERROR): 固件热路径有 ESP_LOGE (如 USB 打开失败、
    // BLE 更新失败), 只降级到 ERROR 仍会把日志行混入二进制记录流, 导致接收端
    // CRC 校验失败。改用 NONE + 加长排空, 确保记录字节纯净。
    esp_log_level_set("*", ESP_LOG_NONE);
    // 刷掉级别切换前已在 UART TX 缓冲/驱动中的日志行
    vTaskDelay(pdMS_TO_TICKS(150));

    // 预扫描统计有效记录数: 损坏记录 (readRecordRaw==0) 直接跳过, 不导出。
    // 旧的实现把损坏记录填 0 后当作有效块导出 — 接收方会把它解析成一条
    // "全部字段 unknown" 的合法广播, 污染还原出的飞行轨迹。
    uint8_t buf[FlightLog::kRecordSize];
    uint32_t validCount = 0;
    for (uint32_t i = 0; i < available; i++) {
        if (_flightLog->readRecordRaw(i, buf) != 0) {
            validCount++;
        }
    }

    if (validCount == 0) {
        printf("+EMPTY\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }

    printf("+OK %lu\r\n", (unsigned long)validCount);
    fflush(stdout);

    uint32_t sent = 0;
    for (uint32_t i = 0; i < available && sent < validCount; i++) {
        // readRecordRaw 把完整 kRecordSize 字节原始记录拷贝到 buf (含 magic/CRC/时间戳)。
        // 此前误用 readRecord — 它只把 payload 拷进 buf, 导致导出的是
        // "payload+栈上残留字节" 的假记录, 接收端 CRC 校验必失败。
        if (_flightLog->readRecordRaw(i, buf) == 0) {
            continue;  // 损坏记录 — 跳过, 接收方按 +OK N 计数解析, 索引不丢失
        }
        fwrite(buf, 1, sizeof(buf), stdout);
        sent++;
    }
    fflush(stdout);

    printf("+DONE\r\n");
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_log_level_set("*", ESP_LOG_INFO);
}
