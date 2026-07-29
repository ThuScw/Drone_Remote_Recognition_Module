#ifndef FLIGHT_DATA_H
#define FLIGHT_DATA_H

#include <stdint.h>
#include "rid_messages.h"

// USB Host CDC-ACM → MAVLink 解析 → FlightData
// 接口不变，内部实现为 USB Host 飞控数据读取
void getFlightData(FlightData& fd, uint64_t nowMs);

#endif
