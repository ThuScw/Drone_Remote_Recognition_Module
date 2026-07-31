#include "console_cmd.h"
#include "config.h"
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
    char line[kMaxCmdLen];

    while (true) {
        if (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                line[--len] = '\0';
            }
            if (len > 0) {
                self->handleCommand(line);
            }
        } else {
            // stdin closed or error — avoid busy-wait
            vTaskDelay(pdMS_TO_TICKS(500));
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

    printf("+OK %lu\r\n", (unsigned long)available);
    fflush(stdout);

    uint8_t buf[96];
    for (uint32_t i = 0; i < available; i++) {
        uint16_t outLen = 0;
        uint64_t ts = 0;
        uint16_t rd = _flightLog->readRecord(i, buf, &outLen, &ts);
        if (rd == 0) {
            // 记录损坏？发送空块以保持索引对齐
            memset(buf, 0, sizeof(buf));
        }
        fwrite(buf, 1, sizeof(buf), stdout);
    }
    fflush(stdout);

    printf("+DONE\r\n");
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_log_level_set("*", ESP_LOG_INFO);
}
