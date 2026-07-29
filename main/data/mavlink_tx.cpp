#include "mavlink_tx.h"
#include <string.h>

#define MAVLINK_STX_V2          0xFD
#define MAVLINK_MSG_COMMAND_LONG 76
#define CRC_EXTRA_COMMAND_LONG   152

// --- CRC-16/MCRF4XX (identical to mavlink_parser.cpp) ---

static uint16_t crc_accumulate(uint8_t data, uint16_t crcAcc) {
    uint8_t tmp = data ^ (uint8_t)(crcAcc & 0xFF);
    tmp ^= (tmp << 4);
    return ((uint16_t)(crcAcc >> 8) ^ (uint16_t)(tmp << 8) ^
            (uint16_t)(tmp << 3) ^ (uint16_t)(tmp >> 4)) & 0xFFFF;
}

static uint16_t crc_calculate(const uint8_t* buf, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = crc_accumulate(buf[i], crc);
    }
    return crc;
}

uint16_t mavlink_build_arm_disarm(uint8_t* buf, uint16_t bufLen,
                                   uint8_t sysId, uint8_t compId,
                                   bool arm, bool force) {
    // MAVLink v2 COMMAND_LONG frame:
    //   STX(1)+LEN(1)+INCOMPAT(1)+COMPAT(1)+SEQ(1)+
    //   SYSID(1)+COMPID(1)+MSGID(3)+PAYLOAD(33)+CRC(2) = 45 bytes
    //
    // Payload field order (per MAVLink XML definition):
    //   param1..param7 (float, 7×4=28), command (uint16, 2),
    //   target_system (1), target_component (1), confirmation (1) = 33
    const uint16_t headerLen = 10;  // STX + 9 header-after-STX
    const uint16_t payloadLen = 33;
    const uint16_t crcLen = 2;
    const uint16_t frameLen = headerLen + payloadLen + crcLen;  // 45
    if (bufLen < frameLen) return 0;

    memset(buf, 0, frameLen);
    static uint8_t seq = 0;

    // --- Header ---
    buf[0] = MAVLINK_STX_V2;     // STX
    buf[1] = payloadLen;          // LEN
    // INCOMPAT=0, COMPAT=0 (already zeroed)
    buf[4] = seq++;               // SEQ
    buf[5] = sysId;               // SYSID
    buf[6] = compId;              // COMPID

    // MSGID = 76 (3 bytes LE)
    buf[7] = 76;
    buf[8] = 0;
    buf[9] = 0;

    // --- Payload (offset 10, 33 bytes) ---
    // Field order: param1..7, command, target_system, target_component, confirmation
    uint8_t* p = buf + 10;

    // param1 (float, offset 0-3): 1.0 = arm, 0.0 = disarm
    float val_arm = arm ? 1.0f : 0.0f;
    memcpy(p + 0, &val_arm, 4);

    // param2 (float, offset 4-7): 21196.0 = force, 0.0 = respect safety checks
    float val_force = force ? 21196.0f : 0.0f;
    memcpy(p + 4, &val_force, 4);

    // param3-7 (float, offsets 8-27) remain zero

    // command (uint16 LE, offset 28-29) = 400 = MAV_CMD_COMPONENT_ARM_DISARM
    p[28] = 144;   // 400 & 0xFF
    p[29] = 1;     // 400 >> 8

    // target_system (uint8, offset 30)
    p[30] = 1;

    // target_component (uint8, offset 31)
    p[31] = 1;

    // confirmation (uint8, offset 32) — already zero

    // --- CRC ---
    // CRC covers bytes 1 through (frameLen - crcLen - 1) = bytes 1..42 (42 bytes)
    uint16_t crc = crc_calculate(buf + 1, frameLen - crcLen - 1);
    crc = crc_accumulate(CRC_EXTRA_COMMAND_LONG, crc);
    buf[43] = crc & 0xFF;
    buf[44] = (crc >> 8) & 0xFF;

    return frameLen;
}
