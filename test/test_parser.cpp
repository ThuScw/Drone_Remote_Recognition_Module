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

    // ==== 12. fillFlightData: GPS fix < 2 returns false ----
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
        CHECK(!mavlink_fillFlightData(p, fd, 5000));
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
}
