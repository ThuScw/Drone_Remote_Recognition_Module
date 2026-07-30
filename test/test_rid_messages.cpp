// GB 46750-2025 encoding compliance tests
#include "test_common.h"
#include "rid_messages.h"

static const char* TEST_UAS   = "CPNYMDL001234567890A";
static const char* TEST_REAL  = "00000000";

// Helper: fill FlightData with known valid values
static FlightData makeFd(float lat, float lon, float alt, float spd,
                          float hdg, uint8_t status) {
    FlightData fd;
    memset(&fd, 0, sizeof(fd));
    fd.lat = lat; fd.lon = lon;
    fd.geoAlt = alt;
    fd.heightAgl = alt * 0.5f;
    fd.baroAlt = alt;
    fd.speed = spd;
    fd.heading = hdg;
    fd.vspeed = 1.5f;
    fd.opStatus = status;
    fd.opLat = lat; fd.opLon = lon; fd.opAlt = alt;
    fd.validMask = FLD_ALL;
    fd.freshness = FRESH_OK;
    fd.validationFlags = 0;
    return fd;
}

// Helper: read int32 LE from buffer
static int32_t read_i32le(const uint8_t* b) {
    return (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

// Helper: read uint16 LE from buffer
static uint16_t read_u16le(const uint8_t* b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

// ============================================================

void test_rid_messages() {
    printf("--- GB 46750-2025 Encoding ---\n");

    // ==== 1. Packet structure ----
    {
        FlightData fd = makeFd(31.2305f, 121.4738f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);

        CHECK_EQ(pkt.dataType, 0xFF);
        CHECK_EQ(pkt.version, 0x01);
        CHECK_EQ(pkt.dataLength, pkt.contentLen);
        CHECK(pkt.contentLen > 0);
        CHECK_EQ(pkt.totalLen, 1 + 1 + 1 + pkt.dataIdLen + pkt.contentLen);
        CHECK(pkt.dataIdLen == 3 || pkt.dataIdLen > 0);  // at least some dataId bytes
    }

    // ==== 2. Packet structural verification (gb46750_packetVerify) ----
    {
        FlightData fd = makeFd(31.23f, 121.47f, 50.0f, 3.0f, 180.0f, STATUS_GROUND);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1000ULL);
        CHECK(gb46750_packetVerify(pkt));
    }

    // ==== 3. Serialize produces valid buffer ----
    {
        FlightData fd = makeFd(31.0f, 121.0f, 80.0f, 2.0f, 270.0f, STATUS_AIRBORNE);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1000000ULL);

        uint8_t buf[GB46750_MAX_PACKET];
        uint16_t len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);
        CHECK_EQ(len, pkt.totalLen);

        // Check header bytes
        CHECK_EQ(buf[0], 0xFF);  // dataType
        CHECK_EQ(buf[1], 0x01);  // version
        CHECK_EQ(buf[2], pkt.dataLength);
    }

    // ==== 4. M fields: dataId always set, value=0 when missing (GB 46750-2025 5.2.3) ----
    //    Per GB 46750-2025 Table 2, M (mandatory) fields must be included in every packet.
    //    When data is unavailable, the dataId bit stays 1 and value encodes as 0 (unknown).
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));  // validMask=0, all fields zero

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // Byte-level: all M field bits are 1, O bits are 0 when validMask=0
        CHECK_EQ(pkt.dataId[0], 0xFF);  // 8 M/O bits: UPIC+REALNAME+OP_CAT+UA_CLASS+OP_LOC_TYPE+OP_LOC+OP_ALT+EXT
        CHECK_EQ(pkt.dataId[1], 0xE5);  // UA_POS+TRACK_ANGLE+GROUND_SPEED+GEO_ALT+EXT (no O fields: REL_HEIGHT, VSPEED, BARO_ALT)
        CHECK_EQ(pkt.dataId[2], 0xFE);  // OP_STATUS+COORD_SYS+HORIZ_ACC+VERT_ACC+SPEED_ACC+TIMESTAMP+TS_ACC

        // Value encoding: M fields missing → encoded as 0
        // UA_POS at content offset 41 (after UAS_ID+REALNAME+OP_CAT+UA_CLASS+OP_LOC_TYPE+OP_POS+OP_ALT)
        int32_t uaLat_i = read_i32le(pkt.content + 41);
        int32_t uaLon_i = read_i32le(pkt.content + 45);
        CHECK_EQ(uaLat_i, 0);
        CHECK_EQ(uaLon_i, 0);

        // HEADING at offset 49, SPEED at offset 51 — both 0
        CHECK_EQ(read_u16le(pkt.content + 49), 0);
        CHECK_EQ(read_u16le(pkt.content + 51), 0);

        // OP_STATUS at offset 55 — encoded as STATUS_UNREPORTED (0)
        CHECK_EQ(pkt.content[55], (uint8_t)STATUS_UNREPORTED);

        CHECK(gb46750_packetVerify(pkt));
    }

    // ==== 5. O fields conditional on validMask ----
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.validMask = 0;  // Nothing valid

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // O fields should NOT be present when validMask bit is not set
        CHECK(!(pkt.dataId[1] & DID_REL_HEIGHT));  // 011 O
        CHECK(!(pkt.dataId[1] & DID_VERT_SPEED));  // 012 O
        CHECK(!(pkt.dataId[1] & DID_BARO_ALT));    // 014 O

        // Now set O field bits and verify they appear
        fd.validMask = FLD_HEIGHT_AGL | FLD_VSPEED | FLD_BARO_ALT;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        CHECK(pkt.dataId[1] & DID_REL_HEIGHT);
        CHECK(pkt.dataId[1] & DID_VERT_SPEED);
        CHECK(pkt.dataId[1] & DID_BARO_ALT);
    }

    // ==== 6. Lat/Lon encoding: deg * 1e7, int32 LE ----
    {
        FlightData fd = makeFd(31.2305f, 121.4738f, 50.0f, 0.0f, 0.0f, STATUS_GROUND);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // The lat/lon appear twice in content: once for opPos, once for uaPos
        // UAS_ID (20) + REALNAME (8) + OP_CATEGORY (1) + UA_CLASS (1) + OP_LOC_TYPE (1) = 31
        // OP_POS at offset 31: lat(4) + lon(4)
        int32_t opLat_i = read_i32le(pkt.content + 31);
        int32_t opLon_i = read_i32le(pkt.content + 35);
        CHECK_CLOSE((double)opLat_i / 1e7, 31.2305, 0.00001);
        CHECK_CLOSE((double)opLon_i / 1e7, 121.4738, 0.00001);

        // OP_ALT at offset 39: 2 bytes
        // UA_POS at offset 41: lat(4) + lon(4)
        int32_t uaLat_i = read_i32le(pkt.content + 41);
        int32_t uaLon_i = read_i32le(pkt.content + 45);
        CHECK_CLOSE((double)uaLat_i / 1e7, 31.2305, 0.00001);
        CHECK_CLOSE((double)uaLon_i / 1e7, 121.4738, 0.00001);
    }

    // ==== 7. Altitude encoding: (alt + 1000) * 2, resolution 0.5m ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 100.0f, 0.0f, 0.0f, STATUS_GROUND);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // OP_ALT at offset 31+8 = 39. GB 46750-2025 Table 3-013: (alt+1000)*2, 0.5m res.
        // alt=100: (100+1000)*2 = 2200
        uint16_t opAlt = read_u16le(pkt.content + 39);
        CHECK_EQ(opAlt, 2200);

        // GEO_ALT at offset 56 (after UA_POS+HDG+SPD+REL_HEIGHT+VSPEED)
        uint16_t geoAlt = read_u16le(pkt.content + 56);
        CHECK_EQ(geoAlt, 2200);

        // Boundary: alt = -500m → (-500+1000)*2 = 1000
        FlightData fd2 = makeFd(0.0f, 0.0f, -500.0f, 0.0f, 0.0f, STATUS_GROUND);
        gb46750_buildPacket(pkt, fd2, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);
        opAlt = read_u16le(pkt.content + 39);
        CHECK_EQ(opAlt, 1000);
    }

    // ==== 8. Relative height encoding: (h + 9000) * 2 ----
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.lat = 0; fd.lon = 0; fd.geoAlt = 0;
        fd.heightAgl = 15.5f;
        fd.opLat = 0; fd.opLon = 0; fd.opAlt = 0;
        fd.validMask = FLD_ALL_M | FLD_HEIGHT_AGL;  // M + relative height
        fd.freshness = FRESH_OK;

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // REL_HEIGHT at offset 53-54 (after UA_POS+HEADING+SPEED)
        // GB 46750-2025 Table 3-011: (h+9000)*2, 0.5m res.
        // h=15.5: (15.5+9000)*2 = 18031
        uint16_t relH = read_u16le(pkt.content + 53);
        CHECK_EQ(relH, 18031);
    }

    // ==== 9. Heading encoding: deg * 10, 0-3599, uint16 LE ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 123.4f, STATUS_GROUND);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // HEADING at offset 49 (after UA_POS at 41-48)
        uint16_t hdg = read_u16le(pkt.content + 49);
        CHECK_EQ(hdg, 1234);  // 123.4 * 10

        // Boundary: 359.9°
        FlightData fd2 = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 359.9f, STATUS_GROUND);
        gb46750_buildPacket(pkt, fd2, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);
        CHECK_EQ(read_u16le(pkt.content + 49), 3599);
    }

    // ==== 10. Speed encoding: m/s * 10, uint16 LE ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 15.3f, 0.0f, STATUS_GROUND);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // SPEED at offset 51 (after HEADING)
        uint16_t spd = read_u16le(pkt.content + 51);
        CHECK_EQ(spd, 153);  // 15.3 * 10

        // Zero speed
        FlightData fd2 = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_GROUND);
        gb46750_buildPacket(pkt, fd2, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);
        CHECK_EQ(read_u16le(pkt.content + 51), 0);
    }

    // ==== 11. Vertical speed encoding: bit7=direction, bit6-0=abs*2 ----
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.lat = 0; fd.lon = 0; fd.geoAlt = 0;
        fd.vspeed = 3.0f;  // climbing at 3 m/s
        fd.opLat = 0; fd.opLon = 0; fd.opAlt = 0;
        fd.validMask = FLD_ALL_M | FLD_VSPEED;
        fd.freshness = FRESH_OK;

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // VSPEED at offset 53 (after SPEED at 51-52; REL_HEIGHT not present)
        uint8_t vs = pkt.content[53];
        CHECK_EQ(vs & 0x80, 0);   // bit7=0: rising
        CHECK_EQ(vs & 0x7F, 6);   // 3.0 * 2 = 6

        // Descending
        FlightData fd2;
        memset(&fd2, 0, sizeof(fd2));
        fd2.lat = 0; fd2.lon = 0; fd2.geoAlt = 0;
        fd2.vspeed = -2.5f;  // descending
        fd2.opLat = 0; fd2.opLon = 0; fd2.opAlt = 0;
        fd2.validMask = FLD_ALL_M | FLD_VSPEED;
        fd2.freshness = FRESH_OK;

        gb46750_buildPacket(pkt, fd2, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);
        vs = pkt.content[53];
        CHECK_EQ(vs & 0x80, 0x80);  // bit7=1: descending
        CHECK_EQ(vs & 0x7F, 5);     // 2.5 * 2 = 5
    }

    // ==== 12. Timestamp encoding: 6 bytes LE, Unix ms ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_GROUND);

        uint64_t ts = 1700000000000ULL;  // A specific Unix timestamp
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, ts);

        // Timestamp is the 2nd-to-last field, at offset contentLen-7
        uint16_t off = pkt.contentLen - 7;
        uint64_t decoded = 0;
        for (int i = 0; i < 6; i++) {
            decoded |= ((uint64_t)pkt.content[off + i]) << (i * 8);
        }
        CHECK_EQ(decoded, ts);
    }

    // ==== 13. Range validation (gb46750_validateFlightData) ----
    {
        // Valid data
        FlightData fd = makeFd(31.0f, 121.0f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        uint32_t flags = 0;
        CHECK(gb46750_validateFlightData(fd, flags));
        CHECK_EQ(flags, 0u);

        // Lat out of range (>90)
        fd.lat = 91.0f;
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_POS);

        // Lon out of range (>180)
        fd = makeFd(0.0f, 181.0f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_POS);

        // Alt out of range (< -1000)
        fd = makeFd(0.0f, 0.0f, -2000.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_GEO_ALT);

        // Negative speed
        fd = makeFd(0.0f, 0.0f, 100.0f, -1.0f, 90.0f, STATUS_AIRBORNE);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_SPEED);

        // Invalid opStatus
        fd = makeFd(0.0f, 0.0f, 100.0f, 5.0f, 90.0f, 99);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_OP_STATUS);
    }

    // ==== 14. Freshness check ----
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.validMask = FLD_ALL;
        fd.freshness = FRESH_OK;
        fd.ts_pos = 5000;
        fd.ts_geoAlt = 5000;
        fd.ts_speed = 5000;
        fd.ts_heading = 5000;
        fd.ts_opStatus = 5000;
        fd.ts_opPos = 5000;

        // All fresh (now=6000, threshold=2000)
        CHECK_EQ(gb46750_checkFreshness(fd, 6000, 2000), FRESH_OK);

        // One field stale (now=8000, threshold=2000)
        CHECK_EQ(gb46750_checkFreshness(fd, 8000, 2000), FRESH_STALE);

        // One field missing (validMask bit not set)
        fd.validMask &= ~FLD_POS;
        CHECK_EQ(gb46750_checkFreshness(fd, 5000, 2000), FRESH_INVALID);
    }

    // ==== 15. Negative lat/lon encoding ----
    {
        FlightData fd = makeFd(-33.8688f, 151.2093f, 50.0f, 3.0f, 45.0f, STATUS_AIRBORNE);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // UA_POS at offset 41
        int32_t lat_i = read_i32le(pkt.content + 41);
        int32_t lon_i = read_i32le(pkt.content + 45);

        CHECK_CLOSE((double)lat_i / 1e7, -33.8688, 0.0001);
        CHECK_CLOSE((double)lon_i / 1e7, 151.2093, 0.0001);
    }

    // ==== 16. Operation status encoding in content ----
    {
        // STATUS_AIRBORNE at offset 57 (after GEO_ALT at 56-57)
        // Actually: after GEALT(2) + BARO_ALT(2 if O) → OP_STATUS at 58 or 60
        // Let's find it by checking the content after known fields.
        // With full validMask: UA_POS(8)+HDG(2)+SPD(2)+REL_H(2)+VSPD(1)+GEO_ALT(2)+BARO_ALT(2)+OP_STATUS(1)…
        // OP_STATUS at offset 41+8+2+2+2+1+2+2 = offset 60 in content

        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_EMERGENCY);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        uint8_t status = pkt.content[60];
        CHECK_EQ(status, STATUS_EMERGENCY);
    }

    // ==== 17. Precision fields present in content ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_GROUND);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // Precision fields follow OP_STATUS(1)+COORD_SYS(1) after all data fields
        // With full validMask: ...+BARO_ALT(2)+OP_STATUS(1)+COORD_SYS(1) = at offset 62
        CHECK_EQ(pkt.content[62], 10);   // HORIZ_ACC
        CHECK_EQ(pkt.content[63], 5);    // VERT_ACC
        CHECK_EQ(pkt.content[64], 3);    // SPEED_ACC
    }

    // ==== 18. packetVerify: all failure paths ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_GROUND);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);
        CHECK(gb46750_packetVerify(pkt));  // baseline

        GB46750Packet bad;

        // Wrong dataType
        bad = pkt; bad.dataType = 0xFE;
        CHECK(!gb46750_packetVerify(bad));

        // Wrong version
        bad = pkt; bad.version = 0x02;
        CHECK(!gb46750_packetVerify(bad));

        // dataLength != contentLen
        bad = pkt; bad.dataLength = 99;
        CHECK(!gb46750_packetVerify(bad));

        // contentLen == 0
        bad = pkt; bad.contentLen = 0; bad.dataLength = 0;
        CHECK(!gb46750_packetVerify(bad));

        // dataIdLen out of range (0 or >8)
        bad = pkt; bad.dataIdLen = 0;
        CHECK(!gb46750_packetVerify(bad));
        bad.dataIdLen = 9;
        CHECK(!gb46750_packetVerify(bad));

        // totalLen mismatch
        bad = pkt; bad.totalLen = 1;
        CHECK(!gb46750_packetVerify(bad));
    }

    // ==== 19. serialize: buffer overflow returns 0 ----
    {
        FlightData fd = makeFd(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, STATUS_GROUND);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // maxLen too small → returns 0
        uint8_t tinyBuf[10];
        CHECK_EQ(gb46750_serialize(pkt, tinyBuf, 10), 0);

        // maxLen just right → returns full length
        uint8_t fullBuf[GB46750_MAX_PACKET];
        uint16_t len = gb46750_serialize(pkt, fullBuf, sizeof(fullBuf));
        CHECK_EQ(len, pkt.totalLen);
    }
}
