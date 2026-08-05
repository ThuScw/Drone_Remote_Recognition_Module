#ifndef MAVLINK_CRC_H
#define MAVLINK_CRC_H

#include <stdint.h>

// MAVLink CRC-16/MCRF4XX — shared between parser (RX) and TX builder
inline uint16_t mavlink_crc_accumulate(uint8_t data, uint16_t crcAcc) {
    uint8_t tmp = data ^ (uint8_t)(crcAcc & 0xFF);
    tmp ^= (tmp << 4);
    return ((uint16_t)(crcAcc >> 8) ^ (uint16_t)(tmp << 8) ^
            (uint16_t)(tmp << 3) ^ (uint16_t)(tmp >> 4)) & 0xFFFF;
}

inline uint16_t mavlink_crc_calculate(const uint8_t* buf, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = mavlink_crc_accumulate(buf[i], crc);
    }
    return crc;
}

// CRC extra byte per MAVLink message type (MAVLink 1.0/2.0 spec)
uint8_t mavlink_crc_extra(uint16_t msgid);

#endif
