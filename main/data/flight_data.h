#ifndef FLIGHT_DATA_H
#define FLIGHT_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include "rid_messages.h"

// USB Host CDC-ACM → MAVLink 解析 → FlightData
void getFlightData(FlightData& fd, uint64_t nowMs);

// Send raw data back to flight controller via USB CDC-ACM.
// Returns true on success.
bool flightData_sendToFC(const uint8_t* data, size_t len);

// Build and send a MAVLink COMMAND_LONG arm/disarm command.
// arm=true allows takeoff, arm=false prevents takeoff.
bool flightData_sendArmDisarm(bool arm);

#endif
