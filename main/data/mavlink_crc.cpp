#include "mavlink_crc.h"

uint8_t mavlink_crc_extra(uint16_t msgid) {
    switch (msgid) {
        case 2:   return 137;  // SYSTEM_TIME
        case 0:   return 50;   // HEARTBEAT
        case 1:   return 124;  // SYS_STATUS
        case 11:  return 89;   // SET_MODE
        case 24:  return 24;   // GPS_RAW_INT
        case 30:  return 39;   // ATTITUDE
        case 33:  return 104;  // GLOBAL_POSITION_INT
        case 36:  return 222;  // SERVO_OUTPUT_RAW
        case 74:  return 20;   // VFR_HUD
        case 76:  return 152;  // COMMAND_LONG
        case 105: return 104;  // HOME_POSITION
        case 253: return 83;   // STATUSTEXT
        default:  return 0;
    }
}
