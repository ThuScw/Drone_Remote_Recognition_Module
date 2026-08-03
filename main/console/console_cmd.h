#ifndef CONSOLE_CMD_H
#define CONSOLE_CMD_H

#include "flight_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 轻量串口命令监听器 — 独立 FreeRTOS task，监听 UART0 输入
// 收到 "DUMP" 命令后，将 FlightLog 全部记录以二进制协议发送

class ConsoleCmd {
public:
    static void init(FlightLog& flightLog);

private:
    static void taskFunc(void* param);
    void handleCommand(const char* cmd);
    void dumpAllRecords();
    void printStatus();

    FlightLog* _flightLog = nullptr;
    TaskHandle_t _taskHandle = nullptr;

    static constexpr int kTaskStack  = 3072;
    static constexpr int kTaskPrio   = 1;
    static constexpr int kMaxCmdLen  = 64;
};

#endif
