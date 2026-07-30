#include "mavlink_parser.h"
#include "mavlink_crc.h"
#include "config.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"

#if CONFIG_RID_VERBOSE_LOG
static const char* TAG = "MAVLINK";
#endif

// ================= 消息解码 =================

static void decode_heartbeat(MavlinkParser& p, const uint8_t* payload, uint64_t nowMs) {
    // HEARTBEAT: custom_mode(4) + type(1) + autopilot(1) + base_mode(1) + system_status(1) + mavlink_version(1) = 9 bytes
    // base_mode bit 7: armed flag
    p.armed = (payload[6] & 0x80) != 0;
    p.systemStatus = payload[7];
    p.lastHeartbeatMs = nowMs;

    #if CONFIG_RID_VERBOSE_LOG
    ESP_LOGI(TAG, "HEARTBEAT: type=%d armed=%d status=%d",
             payload[4], p.armed, p.systemStatus);
    #endif
}

static void decode_gps_raw_int(MavlinkParser& p, const uint8_t* payload, uint64_t nowMs) {
    // This FC's GPS_RAW_INT uses a non-standard dialect: fix_type is at offset 28 (after cog),
    // not at offset 8 (before lat) as in common.xml. All other fields shift left by 1.
    // Layout: time_usec(8)+lat(4)+lon(4)+alt(4)+eph(2)+epv(2)+vel(2)+cog(2)+fix_type(1)+sats(1)
    // 注意: 仅更新 GPS 状态字段，不覆写 GLOBAL_POSITION_INT 的位置数据
    p.gpsFixType = payload[28];
    p.gpsEph = (uint16_t)(payload[20] | (payload[21] << 8)) / 100.0f;
    p.gpsEpv = (uint16_t)(payload[22] | (payload[23] << 8)) / 100.0f;
    p.gpsSats = payload[29];

    p.lastGpsMs = nowMs;

    #if CONFIG_RID_VERBOSE_LOG
    float lat = (int32_t)(payload[8] | (payload[9] << 8) | (payload[10] << 16) | (payload[11] << 24)) / 1e7f;
    float lon = (int32_t)(payload[12] | (payload[13] << 8) | (payload[14] << 16) | (payload[15] << 24)) / 1e7f;
    ESP_LOGI(TAG, "GPS: fix=%d lat=%.7f lon=%.7f sats=%d eph=%.1f (raw only, pos from GLOBAL_POSITION_INT)",
             p.gpsFixType, lat, lon, p.gpsSats, p.gpsEph);
    #endif
}

static void decode_attitude(MavlinkParser& p, const uint8_t* payload) {
    // ATTITUDE: time_boot_ms(4) + roll(4) + pitch(4) + yaw(4) + rollspeed(4) + pitchspeed(4) + yawspeed(4) = 28 bytes
    // 暂不存储, 可用于诊断

    #if CONFIG_RID_VERBOSE_LOG
    float roll, pitch, yaw;
    memcpy(&roll,  payload + 4,  4); roll  *= 180.0f / M_PI;
    memcpy(&pitch, payload + 8,  4); pitch *= 180.0f / M_PI;
    memcpy(&yaw,   payload + 12, 4); yaw   *= 180.0f / M_PI;
    ESP_LOGI(TAG, "ATTITUDE: roll=%.1f pitch=%.1f yaw=%.1f deg", roll, pitch, yaw);
    #endif
}

static void decode_global_position_int(MavlinkParser& p, const uint8_t* payload, uint64_t nowMs) {
    // GLOBAL_POSITION_INT: time_boot_ms(4) + lat(4) + lon(4) + alt(4) + relative_alt(4) + vx(2) + vy(2) + vz(2) + hdg(2) = 28 bytes
    // Some FC frames are 26-27 bytes (truncated hdg), handled by payloadLen check below
    p.lat = (int32_t)(payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24)) / 1e7f;
    p.lon = (int32_t)(payload[8] | (payload[9] << 8) | (payload[10] << 16) | (payload[11] << 24)) / 1e7f;
    p.altMsl = (int32_t)(payload[12] | (payload[13] << 8) | (payload[14] << 16) | (payload[15] << 24)) / 1000.0f;
    p.altRel = (int32_t)(payload[16] | (payload[17] << 8) | (payload[18] << 16) | (payload[19] << 24)) / 1000.0f;
    p.velN = (int16_t)(payload[20] | (payload[21] << 8)) / 100.0f;
    p.velE = (int16_t)(payload[22] | (payload[23] << 8)) / 100.0f;
    p.velD = (int16_t)(payload[24] | (payload[25] << 8)) / 100.0f;

    if (p.payloadLen >= 28) {
        uint16_t hdg_raw = payload[26] | (payload[27] << 8);
        if (hdg_raw != 65535) {
            p.heading = hdg_raw / 100.0f;
        } else {
            p.heading = NAN;
        }
    } else if (p.payloadLen == 27) {
        p.heading = payload[26] / 100.0f;  // single-byte hdg (hi byte=0)
    } else {
        p.heading = NAN;  // no hdg data
    }

    p.lastPositionMs = nowMs;

    #if CONFIG_RID_VERBOSE_LOG
    ESP_LOGI(TAG, "POS: lat=%.7f lon=%.7f alt=%.1f rel=%.1f v=(%.2f,%.2f,%.2f) hdg=%.1f",
             p.lat, p.lon, p.altMsl, p.altRel, p.velN, p.velE, p.velD, p.heading);
    #endif
}

static void decode_vfr_hud(MavlinkParser& p, const uint8_t* payload) {
    // VFR_HUD: airspeed(4) + groundspeed(4) + alt(4) + climb(4) + heading(2) + throttle(2) = 20 bytes
    memcpy(&p.groundspeed, payload + 4, 4);
    memcpy(&p.climbRate, payload + 12, 4);

    // heading 是 int16, 如果为 -1 表示未知
    int16_t hdg = (int16_t)(payload[16] | (payload[17] << 8));
    if (hdg >= 0) {
        p.heading = (float)hdg;
    }
    // 如果 GLOBAL_POSITION_INT 已有航向, 不覆盖

    #if CONFIG_RID_VERBOSE_LOG
    ESP_LOGI(TAG, "VFR: spd=%.2f alt=%.1f climb=%.2f hdg=%d",
             p.groundspeed, p.altMsl, p.climbRate, hdg);
    #endif
}

static void decode_home_position(MavlinkParser& p, const uint8_t* payload) {
    // HOME_POSITION: lat(4) + lon(4) + alt(4) + ... = 至少 12 bytes
    p.homeLat = (int32_t)(payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24)) / 1e7f;
    p.homeLon = (int32_t)(payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24)) / 1e7f;
    p.homeAlt = (int32_t)(payload[8] | (payload[9] << 8) | (payload[10] << 16) | (payload[11] << 24)) / 1000.0f;
    p.homeValid = true;

    #if CONFIG_RID_VERBOSE_LOG
    ESP_LOGI(TAG, "HOME: lat=%.7f lon=%.7f alt=%.1f", p.homeLat, p.homeLon, p.homeAlt);
    #endif
}

// ================= 帧处理 =================

static void handle_frame(MavlinkParser& p, uint64_t nowMs) {
    uint16_t msgid;
    const uint8_t* payload;
    if (p.isV2) {
        msgid = p.buffer[7] | (p.buffer[8] << 8) | (p.buffer[9] << 16);
        payload = p.buffer + MAVLINK_V2_HEADER_LEN;
    } else {
        msgid = p.buffer[5];  // v1: single-byte msgid
        payload = p.buffer + MAVLINK_V1_HEADER_LEN;
    }

    switch (msgid) {
        case MAVLINK_MSG_HEARTBEAT:
            if (p.payloadLen >= 9) decode_heartbeat(p, payload, nowMs);
            break;
        case MAVLINK_MSG_GPS_RAW_INT:
            if (p.payloadLen >= 30) decode_gps_raw_int(p, payload, nowMs);
            break;
        case MAVLINK_MSG_ATTITUDE:
            if (p.payloadLen >= 28) decode_attitude(p, payload);
            break;
        case MAVLINK_MSG_GLOBAL_POSITION_INT:
            if (p.payloadLen >= 26) decode_global_position_int(p, payload, nowMs);
            break;
        case MAVLINK_MSG_VFR_HUD:
            if (p.payloadLen >= 20) decode_vfr_hud(p, payload);
            break;
        case MAVLINK_MSG_HOME_POSITION:
            if (p.payloadLen >= 12) decode_home_position(p, payload);
            break;
        default:
            #if CONFIG_RID_VERBOSE_LOG
            ESP_LOGD(TAG, "Unknown msgid=%d len=%d", msgid, p.payloadLen);
            #endif
            break;
    }
}

// ================= API 实现 =================

void mavlink_init(MavlinkParser& p) {
    memset(&p, 0, sizeof(p));
    p.state = PARSE_STATE_IDLE;
    p.heading = NAN;
    p.consecutiveCrcErrors = 0;
    p.lastValidFrameMs = 0;
}

bool mavlink_parseByte(MavlinkParser& p, uint8_t byte, uint64_t nowMs) {
    switch (p.state) {
        case PARSE_STATE_IDLE:
            if (byte == MAVLINK_V2_MAGIC) {
                p.buffer[0] = byte;
                p.bufferIdx = 1;
                p.isV2 = true;
                p.state = PARSE_STATE_HEADER;
                // 预填充 v2 不同字段的偏移: INCOMPAT=0, COMPAT=0
                // 后续 header 字节会覆盖实际值
            } else if (byte == MAVLINK_V1_MAGIC) {
                p.buffer[0] = byte;
                p.bufferIdx = 1;
                p.isV2 = false;
                p.state = PARSE_STATE_HEADER;
            }
            break;

        case PARSE_STATE_HEADER: {
            uint8_t headerLen = p.isV2 ? MAVLINK_V2_HEADER_LEN : MAVLINK_V1_HEADER_LEN;
            p.buffer[p.bufferIdx++] = byte;
            if (p.bufferIdx >= headerLen) {
                p.payloadLen = p.buffer[1];
                if (p.payloadLen > (p.isV2 ? MAVLINK_V2_MAX_PAYLOAD : MAVLINK_V1_MAX_PAYLOAD)) {
                    // payload 长度非法，丢弃
                    p.parseErrors++;
                    p.state = PARSE_STATE_IDLE;
                    break;
                }
                p.expectedLen = headerLen + p.payloadLen + MAVLINK_V2_CRC_LEN;
                p.state = PARSE_STATE_PAYLOAD;
            }
            break;
        }

        case PARSE_STATE_PAYLOAD: {
            uint8_t headerLen = p.isV2 ? MAVLINK_V2_HEADER_LEN : MAVLINK_V1_HEADER_LEN;
            p.buffer[p.bufferIdx++] = byte;
            if (p.bufferIdx >= headerLen + p.payloadLen) {
                p.state = PARSE_STATE_CRC;
            }
            break;
        }

        case PARSE_STATE_CRC:
            p.buffer[p.bufferIdx++] = byte;
            if (p.bufferIdx >= p.expectedLen) {
                uint16_t crcReceived = p.buffer[p.expectedLen - 2] |
                                    (p.buffer[p.expectedLen - 1] << 8);

                // CRC 计算范围: 从 LEN(byte 1) 到 payload 末尾 (不含 STX 和 CRC)
                uint8_t headerLen = p.isV2 ? MAVLINK_V2_HEADER_LEN : MAVLINK_V1_HEADER_LEN;
                uint16_t crcDataLen = headerLen - 1 + p.payloadLen;  // LEN 到 payload 结束
                uint16_t crcCalc = mavlink_crc_calculate(p.buffer + 1, crcDataLen);

                // 加上 CRC extra byte
                uint16_t msgid;
                if (p.isV2) {
                    msgid = p.buffer[7] | (p.buffer[8] << 8) | (p.buffer[9] << 16);
                } else {
                    msgid = p.buffer[5];
                }
                uint8_t crcExtra = mavlink_crc_extra(msgid);
                crcCalc = mavlink_crc_accumulate(crcExtra, crcCalc);

                if (crcCalc == crcReceived) {
                    p.totalFrames++;
                    if (p.isV2) p.totalV2Frames++;
                    else p.totalV1Frames++;
                    p.consecutiveCrcErrors = 0;
                    p.lastValidFrameMs = nowMs;
                    handle_frame(p, nowMs);
                    p.state = PARSE_STATE_IDLE;
                    return true;
                } else {
                    p.crcErrors++;
                    p.consecutiveCrcErrors++;
                    #if CONFIG_RID_VERBOSE_LOG
                    ESP_LOGW(TAG, "CRC fail: %s msgid=%d recv=0x%04X calc=0x%04X",
                             p.isV2 ? "v2" : "v1", msgid, crcReceived, crcCalc);
                    #endif
                    p.state = PARSE_STATE_IDLE;
                }
            }
            break;
    }
    return false;
}

bool mavlink_fillFlightData(const MavlinkParser& p, FlightData& fd, uint64_t nowMs) {
    // 检查数据有效性
    if (p.gpsFixType < 2) {
        // GPS 无定位或 2D, 不输出位置
        return false;
    }

    // 填充位置
    fd.lat = p.lat;
    fd.lon = p.lon;
    fd.geoAlt = p.altMsl;
    fd.heightAgl = p.altRel;  // 相对高度 (AGL)
    fd.baroAlt = p.altMsl;    // 简化: 用 MSL 代替气压高度

    // 填充速度
    fd.speed = sqrtf(p.velN * p.velN + p.velE * p.velE);
    fd.vspeed = -p.velD;  // MAVLink velD 正=向下, 我们定义 正=上升

    // 填充航向
    if (!isnan(p.heading)) {
        fd.heading = p.heading;
    } else {
        // 从速度计算航向
        fd.heading = atan2f(p.velE, p.velN) * 180.0f / M_PI;
        if (fd.heading < 0) fd.heading += 360.0f;
    }

    // 填充运行状态
    if (!p.armed) {
        fd.opStatus = STATUS_GROUND;
    } else if (p.systemStatus == 6) {  // EMERGENCY
        fd.opStatus = STATUS_EMERGENCY;
    } else if (p.systemStatus == 5) {  // CRITICAL
        fd.opStatus = STATUS_FAIL_SAFE;
    } else {
        fd.opStatus = STATUS_AIRBORNE;
    }

    // 操作员位置: 使用 HOME 位置 (如果有), 否则用当前位置
    // TODO: 实际应用中, 操作员位置应该来自遥控器的 GPS 或手动设置
    if (p.homeValid) {
        fd.opLat = p.homeLat;
        fd.opLon = p.homeLon;
        fd.opAlt = p.homeAlt;
    } else {
        fd.opLat = p.lat;
        fd.opLon = p.lon;
        fd.opAlt = p.altMsl;
    }

    // 设置有效标志
    fd.validMask = FLD_ALL;

    // 设置时间戳
    fd.ts_pos      = p.lastPositionMs;
    fd.ts_geoAlt   = p.lastPositionMs;
    fd.ts_speed    = p.lastPositionMs;
    fd.ts_heading  = p.lastPositionMs;
    fd.ts_opStatus = p.lastHeartbeatMs;
    fd.ts_opPos    = p.lastPositionMs;

    // 数据质量
    fd.freshness = FRESH_OK;
    fd.validationFlags = 0;

    return true;
}

bool mavlink_isDataStale(const MavlinkParser& p, uint64_t nowMs, uint64_t timeoutMs) {
    if (nowMs - p.lastPositionMs > timeoutMs) {
        return true;
    }
    if (nowMs - p.lastGpsMs > timeoutMs) {
        return true;
    }
    return false;
}

void mavlink_getStatus(const MavlinkParser& p, char* buf, uint16_t bufLen) {
    snprintf(buf, bufLen,
             "frames=%lu(v1=%lu,v2=%lu) crc_err=%lu armed=%d status=%d "
             "fix=%d sats=%d lat=%.6f lon=%.6f alt=%.1f",
             (unsigned long)p.totalFrames,
             (unsigned long)p.totalV1Frames,
             (unsigned long)p.totalV2Frames,
             (unsigned long)p.crcErrors,
             p.armed, p.systemStatus,
             p.gpsFixType, p.gpsSats,
             p.lat, p.lon, p.altMsl);
}

bool mavlink_needsRecovery(const MavlinkParser& p, uint64_t nowMs, uint32_t errorLimit) {
    // 从未收到过有效帧 → 设备可能尚未连接，不触发恢复
    if (p.lastValidFrameMs == 0) return false;
    // 连续 CRC 失败超过阈值 → 数据流已损坏，需要恢复
    return p.consecutiveCrcErrors >= errorLimit;
}
