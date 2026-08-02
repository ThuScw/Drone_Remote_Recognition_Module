#include "console_cmd.h"
#include "config.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/select.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

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

    // 注册任务看门狗: 命令行任务卡死(如 DUMP 长输出)时系统复位而非静默失效。
    // 本任务为诊断用途, 不注册也不影响 RID 合规广播, 属加固措施 (GB 42590 A.2.3.5.5)。
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "console_cmd task not subscribed to TWDT");
    }

    int fd = fileno(stdin);
    char line[kMaxCmdLen];
    size_t lineLen = 0;

    while (true) {
        // 用 select 有限等待替代阻塞 fgets: 空闲时也要定期喂狗, 避免任务
        // 因 stdin 长时间无输入被看门狗误复位。按行累积, 保持"回车才执行"语义。
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 500000 };   // 500ms 轮询
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        esp_task_wdt_reset();

        if (sel < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));   // EINTR 等 — 退避重试
            continue;
        }
        if (sel == 0) {
            continue;                        // 超时无输入 — 已喂狗, 本轮结束
        }

        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));  // stdin 异常 — 退避重试
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r' || c == '\n') {
                if (lineLen > 0) {
                    line[lineLen] = '\0';
                    self->handleCommand(line);
                    lineLen = 0;
                }
            } else if (lineLen + 1 < kMaxCmdLen) {
                line[lineLen++] = c;
            }
        }
    }
}

void ConsoleCmd::handleCommand(const char* cmd) {
    if (strcmp(cmd, "DUMP") == 0) {
        dumpAllRecords();
    } else if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "help") == 0) {
        printf("Commands: DUMP (export flight log), HELP\r\n");
        fflush(stdout);
    } else {
        printf("? Unknown: %s (type HELP)\r\n", cmd);
        fflush(stdout);
    }
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

    // 静默期间抑制除错误外的所有日志，防止日志与二进制数据交错
    esp_log_level_set("*", ESP_LOG_ERROR);
    // 刷掉已在缓冲区的日志
    vTaskDelay(pdMS_TO_TICKS(50));

    // 预扫描统计有效记录数: 损坏记录 (readRecord==0) 直接跳过, 不导出。
    // 旧的实现把损坏记录填 0 后当作有效块导出 — 接收方会把它解析成一条
    // "全部字段 unknown" 的合法广播, 污染还原出的飞行轨迹。
    uint8_t buf[96];
    uint32_t validCount = 0;
    for (uint32_t i = 0; i < available; i++) {
        // 逐条喂狗: 全量导出可能远超看门狗超时(96B/条 @115200≈120条/s,
        // 5s 仅能传 ~600 条), 不在循环内喂狗会被 TWDT 误复位。
        esp_task_wdt_reset();

        uint16_t outLen = 0;
        uint64_t ts = 0;
        if (_flightLog->readRecord(i, buf, &outLen, &ts) != 0) {
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
        esp_task_wdt_reset();

        uint16_t outLen = 0;
        uint64_t ts = 0;
        if (_flightLog->readRecord(i, buf, &outLen, &ts) == 0) {
            continue;  // 损坏记录 — 跳过, 接收方按 +OK N 计数解析, 索引不丢失
        }
        fwrite(buf, 1, sizeof(buf), stdout);
        sent++;
    }
    fflush(stdout);
    esp_task_wdt_reset();

    printf("+DONE\r\n");
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_log_level_set("*", ESP_LOG_INFO);
}
