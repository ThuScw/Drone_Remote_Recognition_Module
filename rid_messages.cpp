#include <Arduino.h>
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
    return (uint16_t)(encoded + 0.5f);  // 四舍五入
}

// 相对高度: uint16 LE, (val + 9000) * 2, 分辨率 0.5m
// GB 46750-2025 Table 3-011 — 注意偏移是 9000 不是 1000
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
    uint8_t val = (uint8_t)(absVal * 2.0f + 0.5f);  // 四舍五入
    if (val > 127) val = 127;
    return dir | val;
}

// ---------- 数据包构建 ----------

void gb46750_buildPacket(GB46750Packet& pkt, const FlightData& fd,
                          const char* uasId, const char* realNameId,
                          uint8_t opCategory, uint8_t uaClass,
                          uint8_t opLocType, uint8_t coordSys,
                          uint8_t horizAcc, uint8_t vertAcc,
                          uint8_t speedAcc, uint8_t tsAcc) {
    memset(&pkt, 0, sizeof(pkt));

    pkt.dataType = GB46750_DATA_TYPE;
    pkt.version  = GB46750_VERSION;

    // 数据标识 (3 bytes, 包含所有 M + O 字段)
    pkt.dataId[0] = DID_UPIC | DID_REALNAME | DID_OP_CATEGORY | DID_UA_CLASS
                   | DID_OP_LOC_TYPE | DID_OP_LOC | DID_OP_ALT | DID_EXT_FLAG;
    pkt.dataId[1] = DID_UA_POS | DID_TRACK_ANGLE | DID_GROUND_SPEED
                   | DID_REL_HEIGHT | DID_VERT_SPEED | DID_GEO_ALT
                   | DID_BARO_ALT | DID_EXT_FLAG;
    pkt.dataId[2] = DID_OP_STATUS | DID_COORD_SYS | DID_HORIZ_ACC
                   | DID_VERT_ACC | DID_SPEED_ACC | DID_TIMESTAMP | DID_TS_ACC;
    pkt.dataIdLen = 3;

    uint8_t* c = pkt.content;
    uint16_t pos = 0;

    // 001 唯一产品识别码 (20 bytes, big-endian ASCII)
    size_t uasLen = strlen(uasId);
    for (int i = 0; i < 20; i++) {
        c[pos++] = (i < (int)uasLen) ? (uint8_t)uasId[i] : 0x00;
    }

    // 002 实名登记标志 (8 bytes, big-endian ASCII)
    size_t rnLen = strlen(realNameId);
    for (int i = 0; i < 8; i++) {
        c[pos++] = (i < (int)rnLen) ? (uint8_t)realNameId[i] : 0x00;
    }

    // 003 运行类别
    c[pos++] = opCategory;

    // 004 无人机分类
    c[pos++] = uaClass;

    // 005 遥控站位置类型
    c[pos++] = opLocType;

    // 006 遥控站位置 (8 bytes LE: int32 lat, int32 lon)
    encodeLatLon(c + pos, fd.opLat, fd.opLon);
    pos += 8;

    // 007 遥控站高度 (2 bytes LE)
    writeU16LE(c + pos, encodeAlt1000(fd.opAlt));
    pos += 2;

    // 008 无人机位置 (8 bytes LE)
    encodeLatLon(c + pos, fd.lat, fd.lon);
    pos += 8;

    // 009 航迹角 (2 bytes LE)
    writeU16LE(c + pos, encodeHeading(fd.heading));
    pos += 2;

    // 010 地速 (2 bytes LE)
    writeU16LE(c + pos, encodeSpeed(fd.speed));
    pos += 2;

    // 011 相对高度 (2 bytes LE)
    writeU16LE(c + pos, encodeRelHeight(fd.heightAgl));
    pos += 2;

    // 012 垂直速度 (1 byte)
    c[pos++] = encodeVSpeed(fd.vspeed);

    // 013 大地高度 (2 bytes LE)
    writeU16LE(c + pos, encodeAlt1000(fd.geoAlt));
    pos += 2;

    // 014 气压高度 (2 bytes LE)
    writeU16LE(c + pos, encodeAlt1000(fd.baroAlt));
    pos += 2;

    // 015 运行状态
    c[pos++] = fd.opStatus;

    // 016 坐标系类型
    c[pos++] = coordSys;

    // 017 水平精度
    c[pos++] = horizAcc;

    // 018 垂直精度
    c[pos++] = vertAcc;

    // 019 速度精度
    c[pos++] = speedAcc;

    // 020 时间戳 (6 bytes LE, Unix ms)
    // Stage 1 使用 millis() 模拟; Stage 2 替换为 GPS/NTP 时间
    uint64_t unixMs = (uint64_t)millis();
    writeTimestamp(c + pos, unixMs);
    pos += 6;

    // 021 时间戳精度
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
