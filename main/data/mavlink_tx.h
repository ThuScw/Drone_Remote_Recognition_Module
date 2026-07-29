#ifndef MAVLINK_TX_H
#define MAVLINK_TX_H

#include <stdint.h>
#include <stdbool.h>

// Build a MAVLink v2 COMMAND_LONG frame for MAV_CMD_COMPONENT_ARM_DISARM (cmd 400).
// Writes the complete frame (STX through CRC) to buf.
// Returns the total frame length (45 bytes), or 0 if bufLen is insufficient.
uint16_t mavlink_build_arm_disarm(uint8_t* buf, uint16_t bufLen,
                                   uint8_t sysId, uint8_t compId,
                                   bool arm, bool force);

#endif
