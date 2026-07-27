#ifndef FLIGHT_DATA_H
#define FLIGHT_DATA_H

#include <stdint.h>
#include "rid_messages.h"

// 从数据源读取当前飞行数据
// nowMs — 系统运行毫秒数 (Mock 阶段用于时间推算; 真飞控时忽略)
//
// Stage 1: Mock 实现 — 65s 起降循环模拟
// Stage 2: 替换为本文件的 UART 飞控/GPS 解析实现, 接口不变
void getFlightData(FlightData& fd, uint64_t nowMs);

#endif
