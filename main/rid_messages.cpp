#include <stdint.h>
#include <string.h>
#include "rid_messages.h"

// ---------- 小端序写入辅助 ----------

static inline void writeI32LE(uint8_t* buf, int32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

static inline void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static void writeTimestamp(uint8_t* buf, uint64_t unixMs) {
    for (int i = 0; i < 6; i++) {
        buf[i] = (unixMs >> (i * 8)) & 0xFF;
    }
}

// ---------- 编码函数 (GB 46750-2025 Table 3) ----------

// 经纬度: int32 LE, deg * 1e7
static void encodeLatLon(uint8_t* buf, float lat, float lon) {
    int32_t lat_i = (int32_t)(lat * 10000000.0f);
    int32_t lon_i = (int32_t)(lon * 10000000.0f);
    writeI32LE(buf, lat_i);
    writeI32LE(buf + 4, lon_i);
}

// 大地/气压高度: uint16 LE, (val + 1000) * 2, 分辨率 0.5m
// GB 46750-2025 Table 3-007, 3-013, 3-014
static uint16_t encodeAlt1000(float alt) {
    float encoded = (alt + 1000.0f) * 2.0f;
    if (encoded < 0.0f)       encoded = 0.0f;
    if (encoded > 65535.0f)   encoded = 65535.0f;
    return (uint16_t)(encoded + 0.5f);
}

// 相对高度: uint16 LE, (val + 9000) * 2, 分辨率 0.5m
// GB 46750-2025 Table 3-011 — 偏移量是 9000 不是 1000
static uint16_t encodeRelHeight(float h) {
    float encoded = (h + 9000.0f) * 2.0f;
    if (encoded < 0.0f)       encoded = 0.0f;
    if (encoded > 65535.0f)   encoded = 65535.0f;
    return (uint16_t)(encoded + 0.5f);
}

// 航迹角: uint16 LE, val * 10, 范围 0~3599, 分辨率 0.1°
// GB 46750-2025 Table 3-009
static uint16_t encodeHeading(float deg) {
    if (deg < 0.0f)      deg = 0.0f;
    if (deg >= 360.0f)   deg = 359.9f;
    uint16_t val = (uint16_t)(deg * 10.0f + 0.5f);
    if (val > 3599)      val = 3599;
    return val;
}

// 地速: uint16 LE, val * 10, 分辨率 0.1 m/s
// GB 46750-2025 Table 3-010
static uint16_t encodeSpeed(float mps) {
    if (mps < 0.0f)        mps = 0.0f;
    if (mps > 6553.5f)     mps = 6553.5f;
    return (uint16_t)(mps * 10.0f + 0.5f);
}

// 垂直速度: 1 byte, bit7=direction(0=上升, 1=下降), bit6-0=val*2
// GB 46750-2025 Table 3-012
static uint8_t encodeVSpeed(float mps) {
    uint8_t dir = (mps < 0.0f) ? 0x80 : 0x00;
    float absVal = (mps < 0.0f) ? -mps : mps;
    uint8_t val = (uint8_t)(absVal * 2.0f + 0.5f);
    if (val > 127) val = 127;
    return dir | val;
}

// ---------- 数据包构建 ----------

void gb46750_buildPacket(GB46750Packet& pkt, const FlightData& fd,
                          const char* uasId, const char* realNameId,
                          uint8_t opCategory, uint8_t uaClass,
                          uint8_t opLocType, uint8_t coordSys,
                          uint8_t horizAcc, uint8_t vertAcc,
                          uint8_t speedAcc, uint8_t tsAcc,
                          uint64_t timestampMs) {
    memset(&pkt, 0, sizeof(pkt));

    pkt.dataType = GB46750_DATA_TYPE;
    pkt.version  = GB46750_VERSION;

    // 数据标识 (3 bytes, 根据有效字段动态生成)
    // Byte 0: 静态字段始终置位, 动态字段依 validMask
    pkt.dataId[0] = DID_UPIC | DID_REALNAME | DID_OP_CATEGORY | DID_UA_CLASS
                   | DID_OP_LOC_TYPE | DID_EXT_FLAG;
    if (fd.validMask & FLD_OP_POS)  pkt.dataId[0] |= DID_OP_LOC;
    if (fd.validMask & FLD_OP_ALT)  pkt.dataId[0] |= DID_OP_ALT;

    // Byte 1: 全为飞行数据动态字段
    pkt.dataId[1] = DID_EXT_FLAG;
    if (fd.validMask & FLD_POS)        pkt.dataId[1] |= DID_UA_POS;
    if (fd.validMask & FLD_HEADING)    pkt.dataId[1] |= DID_TRACK_ANGLE;
    if (fd.validMask & FLD_SPEED)      pkt.dataId[1] |= DID_GROUND_SPEED;
    if (fd.validMask & FLD_HEIGHT_AGL) pkt.dataId[1] |= DID_REL_HEIGHT;
    if (fd.validMask & FLD_VSPEED)     pkt.dataId[1] |= DID_VERT_SPEED;
    if (fd.validMask & FLD_GEO_ALT)    pkt.dataId[1] |= DID_GEO_ALT;
    if (fd.validMask & FLD_BARO_ALT)   pkt.dataId[1] |= DID_BARO_ALT;

    // Byte 2
    pkt.dataId[2] = DID_COORD_SYS | DID_HORIZ_ACC | DID_VERT_ACC
                   | DID_SPEED_ACC | DID_TIMESTAMP | DID_TS_ACC;
    if (fd.validMask & FLD_OP_STATUS)  pkt.dataId[2] |= DID_OP_STATUS;

    pkt.dataIdLen = 3;

    uint8_t* c = pkt.content;
    uint16_t pos = 0;

    // 001 唯一产品识别码 (20 bytes, always)
    size_t uasLen = strlen(uasId);
    for (int i = 0; i < 20; i++) {
        c[pos++] = (i < (int)uasLen) ? (uint8_t)uasId[i] : 0x00;
    }

    // 002 实名登记标志 (8 bytes, always)
    size_t rnLen = strlen(realNameId);
    for (int i = 0; i < 8; i++) {
        c[pos++] = (i < (int)rnLen) ? (uint8_t)realNameId[i] : 0x00;
    }

    // 003 运行类别 (always)
    c[pos++] = opCategory;

    // 004 无人机分类 (always)
    c[pos++] = uaClass;

    // 005 遥控站位置类型 (always)
    c[pos++] = opLocType;

    // 006 遥控站位置 (conditional on DID_OP_LOC)
    if (fd.validMask & FLD_OP_POS) {
        encodeLatLon(c + pos, fd.opLat, fd.opLon);
        pos += 8;
    }

    // 007 遥控站高度 (conditional on DID_OP_ALT)
    if (fd.validMask & FLD_OP_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.opAlt));
        pos += 2;
    }

    // 008 无人机位置 (conditional on DID_UA_POS)
    if (fd.validMask & FLD_POS) {
        encodeLatLon(c + pos, fd.lat, fd.lon);
        pos += 8;
    }

    // 009 航迹角 (conditional on DID_TRACK_ANGLE)
    if (fd.validMask & FLD_HEADING) {
        writeU16LE(c + pos, encodeHeading(fd.heading));
        pos += 2;
    }

    // 010 地速 (conditional on DID_GROUND_SPEED)
    if (fd.validMask & FLD_SPEED) {
        writeU16LE(c + pos, encodeSpeed(fd.speed));
        pos += 2;
    }

    // 011 相对高度 (O, conditional on DID_REL_HEIGHT)
    if (fd.validMask & FLD_HEIGHT_AGL) {
        writeU16LE(c + pos, encodeRelHeight(fd.heightAgl));
        pos += 2;
    }

    // 012 垂直速度 (O, conditional on DID_VERT_SPEED)
    if (fd.validMask & FLD_VSPEED) {
        c[pos++] = encodeVSpeed(fd.vspeed);
    }

    // 013 大地高度 (conditional on DID_GEO_ALT)
    if (fd.validMask & FLD_GEO_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.geoAlt));
        pos += 2;
    }

    // 014 气压高度 (O, conditional on DID_BARO_ALT)
    if (fd.validMask & FLD_BARO_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.baroAlt));
        pos += 2;
    }

    // 015 运行状态 (conditional on DID_OP_STATUS)
    if (fd.validMask & FLD_OP_STATUS) {
        c[pos++] = fd.opStatus;
    }

    // 016 坐标系类型 (always)
    c[pos++] = coordSys;

    // 017-019 精度 (always)
    c[pos++] = horizAcc;
    c[pos++] = vertAcc;
    c[pos++] = speedAcc;

    // 020 时间戳 (6 bytes LE, always)
    writeTimestamp(c + pos, timestampMs);
    pos += 6;

    // 021 时间戳精度 (always)
    c[pos++] = tsAcc;

    pkt.contentLen = pos;
    pkt.dataLength = pos;
    pkt.totalLen = 1 + 1 + 1 + pkt.dataIdLen + pos;
}

uint16_t gb46750_serialize(const GB46750Packet& pkt, uint8_t* out, uint16_t maxLen) {
    uint16_t len = pkt.totalLen;
    if (len > maxLen) return 0;

    uint16_t pos = 0;
    out[pos++] = pkt.dataType;
    out[pos++] = pkt.version;
    out[pos++] = pkt.dataLength;
    for (int i = 0; i < pkt.dataIdLen; i++) {
        out[pos++] = pkt.dataId[i];
    }
    memcpy(out + pos, pkt.content, pkt.contentLen);
    pos += pkt.contentLen;

    return pos;
}

bool gb46750_packetVerify(const GB46750Packet& pkt) {
    if (pkt.dataType != GB46750_DATA_TYPE) return false;
    if (pkt.version  != GB46750_VERSION)  return false;
    if (pkt.dataLength != pkt.contentLen) return false;
    if (pkt.contentLen == 0)              return false;
    if (pkt.dataIdLen == 0 || pkt.dataIdLen > 8) return false;
    if (pkt.totalLen != 1 + 1 + 1 + pkt.dataIdLen + pkt.contentLen) return false;
    return true;
}
