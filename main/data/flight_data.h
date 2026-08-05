#ifndef FLIGHT_DATA_H
#define FLIGHT_DATA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rid_messages.h"

// USB Host CDC-ACM → MAVLink 解析 → FlightData
void getFlightData(FlightData& fd, uint64_t nowMs);

#endif
