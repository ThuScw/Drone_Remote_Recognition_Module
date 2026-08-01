// MAVLink v1/v2 parser: verified against real flight data
#include "test_common.h"
#include "mavlink_parser.h"
#include "mavlink_crc.h"
#include "test_data.h"

// Feed raw frame bytes to parser
static bool feed_frame(MavlinkParser& p, const uint8_t* frame, uint16_t len, uint64_t nowMs) {
    bool gotFrame = false;
    for (uint16_t i = 0; i < len; i++) {
        if (mavlink_parseByte(p, frame[i], nowMs)) gotFrame = true;
    }
    return gotFrame;
}

void test_mavlink_parser() {
    printf("--- MAVLink Parser (real flight data) ---\n");

    // ==== 1. Feed all real frames through parser, verify CRC passes ====
    {
        MavlinkParser p;
        mavlink_init(p);

        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            feed_frame(p, tf.raw, tf.rawLen, (uint64_t)i * 100);
        }

        // Every frame in kTestFrames was CRC-verified by Python
        CHECK_EQ(p.totalFrames, (uint32_t)kNumTestFrames);
        CHECK_EQ(p.crcErrors, 0u);
    }

    // ==== 2. Verify HEARTBEAT decoding from real data ====
    {
        MavlinkParser p;
        mavlink_init(p);

        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            if (tf.msgid != 0) continue;
            if (tf.expected.armed < 0) continue;

            mavlink_init(p);
            feed_frame(p, tf.raw, tf.rawLen, 1000);

            bool expectedArmed = (tf.expected.armed == 1);
            CHECK_EQ((int)p.armed, (int)expectedArmed);
            CHECK_EQ((int)p.systemStatus, tf.expected.status);
        }
    }

    // ==== 3. Verify GLOBAL_POSITION_INT decoding from real data ====
    {
        MavlinkParser p;
        mavlink_init(p);

        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            if (tf.msgid != 33) continue;
            if (tf.expected.hdg < 0) continue;  // skip if hdg=UINT16_MAX (invalid)

            mavlink_init(p);
            feed_frame(p, tf.raw, tf.rawLen, 5000);

            CHECK_CLOSE(p.lat,    tf.expected.lat,    0.0001);
            CHECK_CLOSE(p.lon,    tf.expected.lon,    0.0001);
            CHECK_CLOSE(p.altMsl, tf.expected.alt,    0.01);
            CHECK_CLOSE(p.altRel, tf.expected.relAlt, 0.01);
            CHECK_CLOSE(p.velN,   tf.expected.vx,     0.01);
            CHECK_CLOSE(p.velE,   tf.expected.vy,     0.01);
            CHECK_CLOSE(p.velD,   tf.expected.vz,     0.01);
            CHECK_CLOSE(p.heading, tf.expected.hdg,   0.1);
        }
    }

    // ==== 4. Verify GPS_RAW_INT decoding from real data ====
    {
        MavlinkParser p;
        mavlink_init(p);

        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            if (tf.msgid != 24) continue;
            if (tf.expected.gpsFix < 0) continue;

            mavlink_init(p);
            feed_frame(p, tf.raw, tf.rawLen, 5000);

            CHECK_EQ((int)p.gpsFixType, tf.expected.gpsFix);
            CHECK_EQ((int)p.gpsSats,    tf.expected.gpsSats);
        }
    }

    // ==== 5. fillFlightData with real v2 armed HEARTBEAT + GPS + Position ====
    {
        // Find frames: v2 armed HB, GPS with fix>=3, and GLOBAL_POSITION_INT
        const TestFrame* hb_armed = nullptr;
        const TestFrame* gps_3d = nullptr;
        const TestFrame* pos = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (!hb_armed && kTestFrames[i].msgid == 0 && kTestFrames[i].expected.armed == 1
                && kTestFrames[i].ver == 2)
                hb_armed = &kTestFrames[i];
            if (!gps_3d && kTestFrames[i].msgid == 24 && kTestFrames[i].expected.gpsFix >= 3)
                gps_3d = &kTestFrames[i];
            if (!pos && kTestFrames[i].msgid == 33 && kTestFrames[i].expected.hdg >= 0)
                pos = &kTestFrames[i];
        }

        if (hb_armed && gps_3d && pos) {
            MavlinkParser p;
            mavlink_init(p);

            feed_frame(p, hb_armed->raw, hb_armed->rawLen, 1000);
            feed_frame(p, pos->raw, pos->rawLen, 1000);
            feed_frame(p, gps_3d->raw, gps_3d->rawLen, 1000);

            FlightData fd;
            memset(&fd, 0, sizeof(fd));
            CHECK(mavlink_fillFlightData(p, fd, 1000));

            CHECK_CLOSE(fd.lat, pos->expected.lat, 0.0001);
            CHECK_CLOSE(fd.lon, pos->expected.lon, 0.0001);
            CHECK(p.armed);
            CHECK_EQ(fd.opStatus, STATUS_AIRBORNE);
            // 无气压计数据源: BARO_ALT (O 字段) 不置位, 避免伪造数值 (旧6)
            CHECK_EQ((int)(fd.validMask & FLD_BARO_ALT), 0);
        }
    }

    // ==== 6. CRC error detection (corrupt known-good frame) ====
    {
        MavlinkParser p;
        mavlink_init(p);

        // Copy a real frame and corrupt the last byte (CRC)
        uint8_t corrupt[64];
        memcpy(corrupt, kTestFrames[0].raw, kTestFrames[0].rawLen);
        corrupt[kTestFrames[0].rawLen - 1] ^= 0xFF;

        feed_frame(p, corrupt, kTestFrames[0].rawLen, 5000);
        CHECK_EQ(p.crcErrors, 1u);
        CHECK_EQ(p.consecutiveCrcErrors, 1u);
        CHECK_EQ(p.totalFrames, 0u);
    }

    // ==== 7. consecutiveCrcErrors reset on valid frame ====
    {
        MavlinkParser p;
        mavlink_init(p);

        uint8_t corrupt[64];
        memcpy(corrupt, kTestFrames[0].raw, kTestFrames[0].rawLen);
        corrupt[kTestFrames[0].rawLen - 1] ^= 0xFF;
        feed_frame(p, corrupt, kTestFrames[0].rawLen, 5000);
        CHECK(p.consecutiveCrcErrors > 0);

        feed_frame(p, kTestFrames[0].raw, kTestFrames[0].rawLen, 5000);
        CHECK_EQ(p.consecutiveCrcErrors, 0u);
    }

    // ==== 8. Invalid STX bytes are skipped ====
    {
        MavlinkParser p;
        mavlink_init(p);

        uint8_t garbage[] = {0x00, 0xAA, 0x55, 0x10, 0x20};
        for (size_t i = 0; i < sizeof(garbage); i++) {
            mavlink_parseByte(p, garbage[i], 0);
        }
        CHECK_EQ(p.totalFrames, 0u);
        CHECK_EQ(p.parseErrors, 0u);
    }

    // ==== 9. Data staleness ====
    {
        MavlinkParser p;
        mavlink_init(p);

        // Feed a real GPS frame + position frame at t=5000
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 33)
                feed_frame(p, kTestFrames[i].raw, kTestFrames[i].rawLen, 5000);
            if (kTestFrames[i].msgid == 24)
                feed_frame(p, kTestFrames[i].raw, kTestFrames[i].rawLen, 5000);
        }

        CHECK(!mavlink_isDataStale(p, 5000, 2000));
        CHECK(mavlink_isDataStale(p, 8000, 2000));
    }

    // ==== 10. mavlink_needsRecovery: both branches ----
    {
        MavlinkParser p;
        mavlink_init(p);

        // Never received a valid frame → no recovery needed (device may not be connected)
        CHECK(!mavlink_needsRecovery(p, 0, 10));

        // Feed a valid frame, then simulate consecutive CRC errors
        feed_frame(p, kTestFrames[0].raw, kTestFrames[0].rawLen, 5000);
        p.consecutiveCrcErrors = 200;

        CHECK(mavlink_needsRecovery(p, 5000, 200));
        CHECK(!mavlink_needsRecovery(p, 5000, 201));  // below limit
    }

    // ==== 11. mavlink_getStatus: output is non-empty ----
    {
        MavlinkParser p;
        mavlink_init(p);

        char buf[256];
        mavlink_getStatus(p, buf, sizeof(buf));
        CHECK(strlen(buf) > 0);
    }

    // ==== 12. fillFlightData: GPS fix < 2 outputs data with STALE flag ----
    {
        MavlinkParser p;
        mavlink_init(p);

        // Feed frames but set gpsFixType to 1 (no fix) after GPS frame
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 24) {
                feed_frame(p, kTestFrames[i].raw, kTestFrames[i].rawLen, 5000);
                break;
            }
        }
        // Feed a position frame
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 33) {
                feed_frame(p, kTestFrames[i].raw, kTestFrames[i].rawLen, 5000);
                break;
            }
        }

        p.gpsFixType = 1;  // simulate no-fix GPS
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        // 新行为: 有位置数据时就输出，GPS fix 不足仅标记为 STALE
        CHECK(mavlink_fillFlightData(p, fd, 5000));
        CHECK_EQ((int)fd.freshness, (int)FRESH_STALE);
        CHECK(fd.validationFlags & (1 << 0));  // bit 0: GPS fix 不足
    }

    // ==== 13. VFR_HUD decoding from real flight data ----
    {
        // Pick first VFR_HUD frame (msgid=74) from test data
        const TestFrame* hud = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 74) {
                hud = &kTestFrames[i];
                break;
            }
        }
        if (hud) {
            MavlinkParser p;
            mavlink_init(p);
            feed_frame(p, hud->raw, hud->rawLen, 5000);

            // Decode expected values from raw payload bytes
            // VFR_HUD payload: airspeed(4) groundspeed(4) alt(4) climb(4) heading(2) throttle(2)
            uint8_t payOff = (hud->ver == 2) ? 10 : 6;
            const uint8_t* payload = hud->raw + payOff;
            float expGs, expClimb;
            memcpy(&expGs, payload + 4, 4);
            memcpy(&expClimb, payload + 12, 4);
            int16_t expHdg = (int16_t)(payload[16] | (payload[17] << 8));

            CHECK_CLOSE(p.groundspeed, expGs, 0.01);
            CHECK_CLOSE(p.climbRate, expClimb, 0.01);
            if (expHdg >= 0) {
                CHECK_CLOSE(p.heading, (float)expHdg, 0.1);
            }
        }
    }

    // ==== 14. HOME_POSITION decoding from real flight data ----
    {
        // Pick first HOME_POSITION frame (msgid=105) with valid home from test data
        const TestFrame* home = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 105 && kTestFrames[i].expected.homeValid) {
                home = &kTestFrames[i];
                break;
            }
        }
        if (home) {
            MavlinkParser p;
            mavlink_init(p);
            feed_frame(p, home->raw, home->rawLen, 5000);

            CHECK(p.homeValid);
            CHECK_CLOSE(p.homeLat, home->expected.homeLat, 0.0001);
            CHECK_CLOSE(p.homeLon, home->expected.homeLon, 0.0001);
            CHECK_CLOSE(p.homeAlt, home->expected.homeAlt, 0.01);
        }
    }

    // ==== 15. Unknown msgid frame does NOT count as CRC error (旧10) ----
    // 未知 msgid 无 CRC extra byte, 无法校验; 之前一律计入连续 CRC 错误会触发假恢复风暴
    {
        MavlinkParser p;
        mavlink_init(p);

        // 构造 v2 帧, msgid=129 (我们的 CRC 表不支持, crcExtra 返回 0)
        uint8_t frame[10 + 9 + 2];
        size_t n = 0;
        frame[n++] = 0xFD;
        frame[n++] = 9;                 // payload len
        frame[n++] = 0x00;              // incompat = 0 (unsigned)
        frame[n++] = 0x00;              // compat
        frame[n++] = 0x02;              // seq
        frame[n++] = 1; frame[n++] = 1; // sysid/compid
        frame[n++] = 0x81; frame[n++] = 0x00; frame[n++] = 0x00;  // msgid=129 (unknown)
        memset(frame + n, 0, 9); n += 9;
        frame[n++] = 0x00; frame[n++] = 0x00;  // CRC 任意值 (无法校验)

        feed_frame(p, frame, n, 1000);
        CHECK_EQ(p.totalFrames, 0u);        // 不是已知消息
        CHECK_EQ(p.crcErrors, 0u);          // 不计入 CRC 错误
        CHECK_EQ(p.consecutiveCrcErrors, 0u);
    }

    // ==== 16. MAVLink v2 signed frame parses correctly (问题4) ----
    // 签名帧: INCOMPAT bit0=1, CRC 后追加 link_id(1)+timestamp(6)+signature(6),
    // CRC 计算包含 link_id
    {
        MavlinkParser p;
        mavlink_init(p);

        uint8_t frame[10 + 9 + MAVLINK_V2_CRC_LEN + MAVLINK_V2_SIG_LEN];
        size_t n = 0;
        frame[n++] = 0xFD;
        frame[n++] = 9;                              // payload len
        frame[n++] = MAVLINK_IFLAG_SIGNED;           // incompat: signed
        frame[n++] = 0x00;                           // compat
        frame[n++] = 0x01;                           // seq
        frame[n++] = 1; frame[n++] = 1;              // sysid/compid
        frame[n++] = 0x00; frame[n++] = 0x00; frame[n++] = 0x00;  // msgid=0 (HEARTBEAT)

        // HEARTBEAT payload: armed (base_mode bit7) + status
        uint8_t payload[9] = {0, 0, 0, 0, 6, 2, 0x80, 4, 3};
        memcpy(frame + n, payload, 9); n += 9;

        uint8_t linkId = 0x5A;
        // CRC = LEN..payload + crcExtra(HEARTBEAT=50); 参考实现确认签名帧 CRC 不含 link_id
        uint16_t crc = mavlink_crc_calculate(frame + 1, n - 1);
        crc = mavlink_crc_accumulate(50, crc);
        frame[n++] = crc & 0xFF;
        frame[n++] = crc >> 8;
        frame[n++] = linkId;                         // link_id 在 CRC 之后 (签名块首字节)

        uint64_t ts = 0x112233445566ULL;
        for (int i = 0; i < 6; i++) frame[n++] = (ts >> (8 * i)) & 0xFF;
        for (int i = 0; i < 6; i++) frame[n++] = 0xAB;  // 任意签名

        bool got = feed_frame(p, frame, n, 1000);
        CHECK(got);
        CHECK_EQ(p.totalFrames, 1u);
        CHECK_EQ(p.crcErrors, 0u);
        CHECK(p.armed);  // 签名帧的 HEARTBEAT 正常解码
        CHECK_EQ((int)p.systemStatus, 4);
    }
}
