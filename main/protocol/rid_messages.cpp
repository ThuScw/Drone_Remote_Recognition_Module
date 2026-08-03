// ---------- GB 46750-2025 协议编码 ----------
// 实现运行识别数据包的构建、序列化、校验、数据验证和新鲜度检查

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <esp_log.h>
#include "rid_messages.h"

static const char* TAG = "RID_PROTO";

// ======================== 小端序写入辅助 ========================

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

// ======================== 编码函数 (GB 46750-2025 Table 3) ========================

// 经纬度: int32 LE, deg * 1e7
// NaN/Inf 或超出物理范围 → 表3未知哨兵 (0xFFFFFFFF), 而非未定义行为的垃圾强转
static void encodeLatLon(uint8_t* buf, float lat, float lon) {
    int32_t lat_i = (isfinite(lat) && lat >= -90.0f && lat <= 90.0f)
                    ? (int32_t)(lat * 10000000.0f) : -1;
    int32_t lon_i = (isfinite(lon) && lon >= -180.0f && lon <= 180.0f)
                    ? (int32_t)(lon * 10000000.0f) : -1;
    writeI32LE(buf, lat_i);
    writeI32LE(buf + 4, lon_i);
}

// 大地/气压高度: uint16 LE, (val + 1000) * 2, 分辨率 0.5m
// GB 46750-2025 Table 3: 编码值 0 表示 "未知或不可用"
// NaN/Inf → 0 (未知), 避免 NaN 比较全假导致钳位失效、强转未定义行为
static uint16_t encodeAlt1000(float alt) {
    if (!isfinite(alt)) return 0;
    float encoded = (alt + 1000.0f) * 2.0f;
    if (encoded < 0.0f)       encoded = 0.0f;
    if (encoded > 65535.0f)   encoded = 65535.0f;
    return (uint16_t)(encoded + 0.5f);
}

// 相对高度: uint16 LE, (val + 9000) * 2, 分辨率 0.5m
static uint16_t encodeRelHeight(float h) {
    if (!isfinite(h)) return 0;
    float encoded = (h + 9000.0f) * 2.0f;
    if (encoded < 0.0f)       encoded = 0.0f;
    if (encoded > 65535.0f)   encoded = 65535.0f;
    return (uint16_t)(encoded + 0.5f);
}

// 航迹角: uint16 LE, val * 10, 范围 0~3599, 分辨率 0.1°
// NaN/Inf → 0xFFFF (未知)。解析器在无航向时置 NAN, 若不加保护会被强转为垃圾值
static uint16_t encodeHeading(float deg) {
    if (!isfinite(deg)) return 0xFFFF;
    if (deg < 0.0f)      deg = 0.0f;
    if (deg >= 360.0f)   deg = 359.9f;
    uint16_t val = (uint16_t)(deg * 10.0f + 0.5f);
    if (val > 3599)      val = 3599;
    return val;
}

// 地速: uint16 LE, val * 10, 分辨率 0.1 m/s
// NaN/Inf → 0xFFFF (未知)
static uint16_t encodeSpeed(float mps) {
    if (!isfinite(mps)) return 0xFFFF;
    if (mps < 0.0f)        mps = 0.0f;
    if (mps > 6553.5f)     mps = 6553.5f;
    return (uint16_t)(mps * 10.0f + 0.5f);
}

// 垂直速度: 1 byte, bit7=direction(0=上升, 1=下降), bit6-0=val*2
// NaN/Inf → 0xFF (未知哨兵, 表3-012)
static uint8_t encodeVSpeed(float mps) {
    if (!isfinite(mps)) return 0xFF;
    uint8_t dir = (mps < 0.0f) ? 0x80 : 0x00;
    float absVal = (mps < 0.0f) ? -mps : mps;
    float encoded = absVal * 2.0f;
    if (encoded > 127.0f) encoded = 127.0f;
    uint8_t val = (uint8_t)(encoded + 0.5f);
    return dir | val;
}

// ======================== 数据包构建 ========================
//
// P0 合规修复 (GB 46750-2025):
//   - M (必填) 字段的数据标识位始终置 1，即使数据不可用也发送
//   - 数据不可用时编码为表3规定的未知哨兵值:
//     006/008 位置 → 0xFFFFFFFF; 009/010 航迹/速度 → 0xFFFF; 高度/时间戳/运行状态 → 0
//   - O (可选) 字段的数据标识位根据 validMask 条件置位
//   - 这确保每个包都是新包，接收方始终能获取最新状态

void gb46750_buildPacket(GB46750Packet& pkt, const FlightData& fd,
                          const char* uasId, const char* realNameId,
                          uint8_t opCategory, uint8_t uaClass,
                          uint8_t opLocType, uint8_t coordSys,
                          uint8_t horizAcc, uint8_t vertAcc,
                          uint8_t speedAcc, uint8_t tsAcc,
                          uint64_t timestampMs) {
    if (!uasId || !realNameId) return;
    memset(&pkt, 0, sizeof(pkt));

    pkt.dataType = GB46750_DATA_TYPE;
    pkt.version  = GB46750_VERSION;

    // --- 数据标识 (3 bytes) ---
    // M 字段: 标识位始终置 1 (即使数据不可用，字段仍存在，值为 0)
    // O 字段: 标识位根据 validMask 条件置位

    // Byte 0: 静态字段 + 遥控站位置/高度 (M)
    pkt.dataId[0] = DID_UPIC | DID_REALNAME | DID_UA_CLASS
                   | DID_OP_LOC_TYPE | DID_OP_LOC | DID_OP_ALT
                   | DID_EXT_FLAG;
    // 003 运行类别 (O) — 始终有值 (config)
    pkt.dataId[0] |= DID_OP_CATEGORY;

    // Byte 1: 飞行数据字段
    pkt.dataId[1] = DID_UA_POS | DID_TRACK_ANGLE | DID_GROUND_SPEED
                   | DID_GEO_ALT | DID_EXT_FLAG;
    // O 字段: 条件置位
    if (fd.validMask & FLD_HEIGHT_AGL) pkt.dataId[1] |= DID_REL_HEIGHT;
    if (fd.validMask & FLD_VSPEED)     pkt.dataId[1] |= DID_VERT_SPEED;
    if (fd.validMask & FLD_BARO_ALT)   pkt.dataId[1] |= DID_BARO_ALT;

    // Byte 2: 状态/精度/时间字段
    pkt.dataId[2] = DID_OP_STATUS | DID_COORD_SYS | DID_HORIZ_ACC
                   | DID_VERT_ACC | DID_SPEED_ACC | DID_TIMESTAMP | DID_TS_ACC;

    pkt.dataIdLen = 3;

    // --- 数据内容项编码 ---
    uint8_t* c = pkt.content;
    uint16_t pos = 0;

    // 001 唯一产品识别码 (20 bytes, M, always)
    size_t uasLen = strlen(uasId);
    for (int i = 0; i < 20; i++) {
        c[pos++] = (i < (int)uasLen) ? (uint8_t)uasId[i] : 0x00;
    }

    // 002 实名登记标志 (8 bytes, M, always)
    size_t rnLen = strlen(realNameId);
    for (int i = 0; i < 8; i++) {
        c[pos++] = (i < (int)rnLen) ? (uint8_t)realNameId[i] : 0x00;
    }

    // 003 运行类别 (1 byte, O, always present from config)
    c[pos++] = opCategory;

    // 004 无人机分类 (1 byte, M, always)
    c[pos++] = uaClass;

    // 005 遥控站位置类型 (1 byte, M, always)
    c[pos++] = opLocType;

    // 006 遥控站位置 (8 bytes, M, always — unknown=0xFFFFFFFF if missing)
    if (fd.validMask & FLD_OP_POS) {
        encodeLatLon(c + pos, fd.opLat, fd.opLon);
    } else {
        writeI32LE(c + pos, -1);      // lat unknown (0xFFFFFFFF)
        writeI32LE(c + pos + 4, -1);  // lon unknown (0xFFFFFFFF)
        ESP_LOGW(TAG, "OP_POS missing, encoding unknown (0xFFFFFFFF)");
    }
    pos += 8;

    // 007 遥控站高度 (2 bytes, M, always — unknown=0 if missing)
    if (fd.validMask & FLD_OP_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.opAlt));
    } else {
        writeU16LE(c + pos, 0);  // unknown
        ESP_LOGW(TAG, "OP_ALT missing, encoding 0");
    }
    pos += 2;

    // 008 无人机位置 (8 bytes, M, always — unknown=0xFFFFFFFF if missing)
    if (fd.validMask & FLD_POS) {
        encodeLatLon(c + pos, fd.lat, fd.lon);
    } else {
        writeI32LE(c + pos, -1);      // lat unknown (0xFFFFFFFF)
        writeI32LE(c + pos + 4, -1);  // lon unknown (0xFFFFFFFF)
        ESP_LOGW(TAG, "UA_POS missing, encoding unknown (0xFFFFFFFF)");
    }
    pos += 8;

    // 009 航迹角 (2 bytes, M, always — unknown=0xFFFF if missing)
    if (fd.validMask & FLD_HEADING) {
        writeU16LE(c + pos, encodeHeading(fd.heading));
    } else {
        writeU16LE(c + pos, 0xFFFF);  // unknown
        ESP_LOGW(TAG, "HEADING missing, encoding unknown (0xFFFF)");
    }
    pos += 2;

    // 010 地速 (2 bytes, M, always — unknown=0xFFFF if missing)
    if (fd.validMask & FLD_SPEED) {
        writeU16LE(c + pos, encodeSpeed(fd.speed));
    } else {
        writeU16LE(c + pos, 0xFFFF);  // unknown
        ESP_LOGW(TAG, "SPEED missing, encoding unknown (0xFFFF)");
    }
    pos += 2;

    // 011 相对高度 (2 bytes, O — only present if valid)
    if (fd.validMask & FLD_HEIGHT_AGL) {
        writeU16LE(c + pos, encodeRelHeight(fd.heightAgl));
        pos += 2;
    }

    // 012 垂直速度 (1 byte, O — only present if valid)
    if (fd.validMask & FLD_VSPEED) {
        c[pos++] = encodeVSpeed(fd.vspeed);
    }

    // 013 大地高度 (2 bytes, M, always — unknown=0 if missing)
    if (fd.validMask & FLD_GEO_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.geoAlt));
    } else {
        writeU16LE(c + pos, 0);  // unknown
        ESP_LOGW(TAG, "GEO_ALT missing, encoding 0");
    }
    pos += 2;

    // 014 气压高度 (2 bytes, O — only present if valid)
    if (fd.validMask & FLD_BARO_ALT) {
        writeU16LE(c + pos, encodeAlt1000(fd.baroAlt));
        pos += 2;
    }

    // 015 运行状态 (1 byte, M, always — unknown=0 if missing)
    if (fd.validMask & FLD_OP_STATUS) {
        c[pos++] = fd.opStatus;
    } else {
        c[pos++] = STATUS_UNREPORTED;  // 0: 未报告
        ESP_LOGW(TAG, "OP_STATUS missing, encoding UNREPORTED");
    }

    // 016 坐标系类型 (1 byte, M, always)
    c[pos++] = coordSys;

    // 017-019 精度 (3 bytes, M, always — from config)
    c[pos++] = horizAcc;
    c[pos++] = vertAcc;
    c[pos++] = speedAcc;

    // 020 时间戳 (6 bytes, M, always)
    writeTimestamp(c + pos, timestampMs);
    pos += 6;

    // 021 时间戳精度 (1 byte, M, always)
    c[pos++] = tsAcc;

    pkt.contentLen = pos;
    pkt.dataLength = pos;
    pkt.totalLen = 1 + 1 + 1 + pkt.dataIdLen + pos;
}

// ======================== 序列化 ========================

uint16_t gb46750_serialize(const GB46750Packet& pkt, uint8_t* out, uint16_t maxLen) {
    if (!out) return 0;
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

// ======================== 数据包结构校验 ========================

bool gb46750_packetVerify(const GB46750Packet& pkt) {
    if (pkt.dataType != GB46750_DATA_TYPE) return false;
    if (pkt.version  != GB46750_VERSION)  return false;
    if (pkt.dataLength != pkt.contentLen) return false;
    if (pkt.contentLen == 0)              return false;
    if (pkt.dataIdLen == 0 || pkt.dataIdLen > 8) return false;
    if (pkt.totalLen != 1 + 1 + 1 + pkt.dataIdLen + pkt.contentLen) return false;
    return true;
}

// ======================== 数据验证 ========================

// 验证飞行数据范围 (物理边界检查)
// 返回 true 表示所有 M 字段范围有效
// validationFlags: 每位置位表示对应字段超出物理范围
bool gb46750_validateFlightData(const FlightData& fd, uint32_t& validationFlags) {
    validationFlags = 0;

    // 注意: NaN 与任何值比较均为 false, 会绕过下面的范围判断,
    // 因此必须先用 isfinite() 显式排除 NaN/Inf, 否则校验漏判。
    // 纬度: -90 ~ +90
    if (!isfinite(fd.lat) || fd.lat < -90.0f || fd.lat > 90.0f) {
        validationFlags |= FLD_POS;
    }
    // 经度: -180 ~ +180
    if (!isfinite(fd.lon) || fd.lon < -180.0f || fd.lon > 180.0f) {
        validationFlags |= FLD_POS;
    }
    // 大地高度: -1000 ~ 10000 m
    if (!isfinite(fd.geoAlt) || fd.geoAlt < -1000.0f || fd.geoAlt > 10000.0f) {
        validationFlags |= FLD_GEO_ALT;
    }
    // 地速: 0 ~ 6553.5 m/s
    if (!isfinite(fd.speed) || fd.speed < 0.0f || fd.speed > 6553.5f) {
        validationFlags |= FLD_SPEED;
    }
    // 航迹角: 0 ~ 359.9°
    if (!isfinite(fd.heading) || fd.heading < 0.0f || fd.heading >= 360.0f) {
        validationFlags |= FLD_HEADING;
    }
    // 运行状态: 0-5 (Table 3-015)
    if (fd.opStatus > STATUS_FAIL_EMERG) {
        validationFlags |= FLD_OP_STATUS;
    }
    // 操作员位置: 同纬度/经度范围
    if (!isfinite(fd.opLat) || fd.opLat < -90.0f || fd.opLat > 90.0f ||
        !isfinite(fd.opLon) || fd.opLon < -180.0f || fd.opLon > 180.0f) {
        validationFlags |= FLD_OP_POS;
    }

    // 返回 true 表示所有 M 字段范围有效
    return (validationFlags & FLD_ALL_M) == 0;
}

// ======================== 数据新鲜度检查 ========================
//
// 返回整体新鲜度等级 (取最差值):
//   FRESH_OK      — 所有 M 字段时间戳在阈值内
//   FRESH_STALE   — 至少一个 M 字段超过阈值 (数据过期但仍可广播)
//   FRESH_INVALID — 至少一个 M 字段完全缺失 (validMask 未置位)
//
// 注意: 新鲜度仅用于日志告警，不阻止广播 (P0 修复)

DataFreshness gb46750_checkFreshness(const FlightData& fd, uint64_t nowMs, uint64_t thresholdMs) {
    DataFreshness worst = FRESH_OK;

    // 检查所有 M 字段的时间戳
    struct { uint32_t flag; uint64_t ts; const char* name; } checks[] = {
        { FLD_POS,       fd.ts_pos,      "POS" },
        { FLD_GEO_ALT,   fd.ts_geoAlt,   "GEO_ALT" },
        { FLD_SPEED,     fd.ts_speed,    "SPEED" },
        { FLD_HEADING,   fd.ts_heading,  "HEADING" },
        { FLD_OP_STATUS, fd.ts_opStatus, "OP_STATUS" },
        { FLD_OP_POS,    fd.ts_opPos,    "OP_POS" },
    };

    for (auto& ch : checks) {
        if (!(fd.validMask & ch.flag)) {
            // M 字段完全缺失
            if (worst < FRESH_INVALID) worst = FRESH_INVALID;
        } else if (nowMs - ch.ts > thresholdMs) {
            // M 字段存在但过期
            if (worst < FRESH_STALE) worst = FRESH_STALE;
        }
    }

    return worst;
}

// ======================== 过期字段老化 ========================
//
// GB 46750-2025 表3: 位置(008)、高度(013)、航迹(009)、速度(010) 在"未知或不可用"时
// 应编码为对应哨兵值 (0xFFFFFFFF / 0xFFFF / 0)，而非广播过期坐标。这避免监管设备
// 依据过期位置做禁飞区/冲突判断时产生安全事故。
// opStatus/opPos 不老化: 运行状态由状态机消抖驱动，起飞点/Home 位置是静态语义。
// 时间戳 (表3-020) 与位置帧同源 (unix = SYSTEM_TIME 偏移 + 位置帧 boot_ms):
// 位置过期后时间戳同步过期 — 置未知(0)，避免接收方得到"新包旧时间戳"的误导
// (与 boot_ms 回绕输出未知同哲学)。tsAcc 由调用方按 unixTimestampMs==0 归 0。

void gb46750_expireStaleFields(FlightData& fd, uint64_t nowMs, uint64_t thresholdMs) {
    struct { uint32_t flag; uint64_t ts; } checks[] = {
        { FLD_POS,      fd.ts_pos     },
        { FLD_GEO_ALT,  fd.ts_geoAlt  },
        { FLD_SPEED,    fd.ts_speed   },
        { FLD_HEADING,  fd.ts_heading },
    };
    for (auto& ch : checks) {
        if ((fd.validMask & ch.flag) && nowMs > ch.ts && nowMs - ch.ts > thresholdMs) {
            fd.validMask &= ~ch.flag;
            if (ch.flag == FLD_POS) fd.unixTimestampMs = 0;
        }
    }
}

// GB 46750-2025 精度枚举映射 (Table 3-017/018)
// GPS eph/epv 单位为米 (标准差); ≤0 返回 0 (unknown)

uint8_t gb46750_mapHorizAcc(float ephM) {
    if (ephM <= 0.0f)   return 0;
    if (ephM < 1.0f)    return 12;  // <1m
    if (ephM < 3.0f)    return 11;  // <3m
    if (ephM < 10.0f)   return 10;  // <10m
    if (ephM < 30.0f)   return 9;   // <30m
    if (ephM < 92.6f)   return 8;   // <92.6m
    if (ephM < 185.0f)  return 7;   // <185m
    if (ephM < 556.0f)  return 6;   // <556m
    if (ephM < 926.0f)  return 5;   // <926m
    if (ephM < 1852.0f) return 4;   // <1852m
    if (ephM < 3700.0f) return 3;   // <3.70km
    if (ephM < 7410.0f) return 2;   // <7.41km
    if (ephM <= 18520.0f) return 1;  // <=18.52km
    return 0;
}

uint8_t gb46750_mapVertAcc(float epvM) {
    if (epvM <= 0.0f)  return 0;
    if (epvM < 1.0f)   return 6;  // <1m
    if (epvM < 3.0f)   return 5;  // <3m
    if (epvM < 10.0f)  return 4;  // <10m
    if (epvM < 25.0f)  return 3;  // <25m
    if (epvM < 45.0f)  return 2;  // <45m
    if (epvM <= 150.0f) return 1;  // <=150m
    return 0;
}
