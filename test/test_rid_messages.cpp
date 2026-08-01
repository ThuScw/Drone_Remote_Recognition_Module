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
// Independent Table 3 decoder — has NO knowledge of gb46750_buildPacket
// internals. Offsets are hardcoded from the standard's fixed field order
// (GB 46750-2025 Table 3). Breaks the encoder↔test self-consistency loop:
// if the encoder shifts a field, this decoder reads the wrong bytes and
// the golden assertions below fail loudly.
// ============================================================

struct DecodedFields {
    int32_t  opLat, opLon;
    uint16_t opAlt;
    int32_t  uaLat, uaLon;
    uint16_t heading, speed;
    uint16_t relHeight;   // 0xFFFF = O-field not present
    uint8_t  vspeed;      // 0xFF = O-field not present
    uint16_t geoAlt;
    uint16_t baroAlt;     // 0xFFFF = O-field not present
    uint8_t  opStatus;
    uint8_t  coordSys;
    uint8_t  horizAcc, vertAcc, speedAcc;
    uint64_t timestamp;
    uint8_t  tsAcc;
};

static bool decodePacket(const uint8_t* buf, uint16_t len, DecodedFields& out) {
    if (!buf || len < 6) return false;
    if (buf[0] != GB46750_DATA_TYPE) return false;
    uint8_t dataIdLen = 3;  // Table 2: 3 dataId bytes for this message set
    uint8_t dlen = buf[2];
    if (len != 1 + 1 + 1 + dataIdLen + dlen) return false;

    const uint8_t* c = buf + 6;  // content start (after 1+1+1+3 header)
    uint16_t o = 0;

    // 001 UAS_ID (20) ... 005 OP_LOC_TYPE (1): fixed leading fields
    o += 20 + 8 + 1 + 1 + 1;

    // 006 OP_POS (8)
    out.opLat = read_i32le(c + o);  o += 4;
    out.opLon = read_i32le(c + o);  o += 4;
    // 007 OP_ALT (2)
    out.opAlt = read_u16le(c + o);  o += 2;
    // 008 UA_POS (8)
    out.uaLat = read_i32le(c + o);  o += 4;
    out.uaLon = read_i32le(c + o);  o += 4;
    // 009 TRACK_ANGLE (2)
    out.heading = read_u16le(c + o);  o += 2;
    // 010 GROUND_SPEED (2)
    out.speed = read_u16le(c + o);  o += 2;
    // 011 REL_HEIGHT (2, O) — presence read from dataId[1]
    out.relHeight = (buf[4] & DID_REL_HEIGHT) ? read_u16le(c + o) : 0xFFFF;
    if (buf[4] & DID_REL_HEIGHT) o += 2;
    // 012 VERT_SPEED (1, O)
    out.vspeed = (buf[4] & DID_VERT_SPEED) ? c[o] : 0xFF;
    if (buf[4] & DID_VERT_SPEED) o += 1;
    // 013 GEO_ALT (2)
    out.geoAlt = read_u16le(c + o);  o += 2;
    // 014 BARO_ALT (2, O)
    out.baroAlt = (buf[4] & DID_BARO_ALT) ? read_u16le(c + o) : 0xFFFF;
    if (buf[4] & DID_BARO_ALT) o += 2;
    // 015 OP_STATUS (1)
    out.opStatus = c[o];  o += 1;
    // 016 COORD_SYS (1)
    out.coordSys = c[o];  o += 1;
    // 017-019 precision (3)
    out.horizAcc = c[o];  o += 1;
    out.vertAcc  = c[o];  o += 1;
    out.speedAcc = c[o];  o += 1;
    // 020 TIMESTAMP (6, LE)
    out.timestamp = 0;
    for (int i = 0; i < 6; i++) out.timestamp |= ((uint64_t)c[o + i]) << (i * 8);
    o += 6;
    // 021 TS_ACC (1)
    out.tsAcc = c[o];  o += 1;

    return (o == dlen);
}

// ============================================================

void test_rid_messages() {
    printf("--- GB 46750-2025 Encoding ---\n");

    // ==== 1. Packet structure ----
    {
        FlightData fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);

        CHECK_EQ(pkt.dataType, 0xFF);
        CHECK_EQ(pkt.version, 0x20);  // V1.0 = 0b001_00000
        CHECK_EQ(pkt.dataLength, pkt.contentLen);
        CHECK(pkt.contentLen > 0);
        CHECK_EQ(pkt.totalLen, 1 + 1 + 1 + pkt.dataIdLen + pkt.contentLen);
        CHECK(pkt.dataIdLen == 3 || pkt.dataIdLen > 0);  // at least some dataId bytes
    }

    // ==== 2. Packet structural verification (gb46750_packetVerify) ----
    {
        FlightData fd = makeFd(34.5f, 110.25f, 50.0f, 3.0f, 180.0f, STATUS_GROUND);
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
        CHECK_EQ(buf[1], 0x20);  // version (V1.0 = 0b001_00000)
        CHECK_EQ(buf[2], pkt.dataLength);
    }

    // ==== 4. M fields: dataId always set, value=unknown sentinel when missing (GB 46750-2025 5.2.3/表3) ----
    //    Per GB 46750-2025 Table 2, M (mandatory) fields must be included in every packet.
    //    When data is unavailable, the dataId bit stays 1 and value encodes as the Table 3
    //    unknown sentinel: 006/008 position → 0xFFFFFFFF, 009/010 heading/speed → 0xFFFF,
    //    007/013 altitude, 015 opStatus, 020 timestamp → 0.
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

        // Value encoding: M fields missing → Table 3 unknown sentinel (validMask=0 → no O fields)
        // Content layout (validMask=0): UAS_ID(20)+REALNAME(8)+OP_CAT(1)+UA_CLASS(1)+OP_LOC_TYPE(1)
        //   +OP_POS(8)+OP_ALT(2)+UA_POS(8)+HEADING(2)+SPEED(2)+GEO_ALT(2)+OP_STATUS(1)+COORD_SYS(1)
        //   +HORIZ_ACC(1)+VERT_ACC(1)+SPEED_ACC(1)+TIMESTAMP(6)+TS_ACC(1)

        // 006 OP_POS at offset 31 — unknown = 0xFFFFFFFF
        CHECK_EQ(read_i32le(pkt.content + 31), (int32_t)0xFFFFFFFF);
        CHECK_EQ(read_i32le(pkt.content + 35), (int32_t)0xFFFFFFFF);
        // 007 OP_ALT at offset 39 — unknown = 0
        CHECK_EQ(read_u16le(pkt.content + 39), 0);

        // 008 UA_POS at offset 41 — unknown = 0xFFFFFFFF
        CHECK_EQ(read_i32le(pkt.content + 41), (int32_t)0xFFFFFFFF);
        CHECK_EQ(read_i32le(pkt.content + 45), (int32_t)0xFFFFFFFF);

        // 009 HEADING at offset 49 — unknown = 0xFFFF
        CHECK_EQ(read_u16le(pkt.content + 49), 0xFFFF);
        // 010 SPEED at offset 51 — unknown = 0xFFFF
        CHECK_EQ(read_u16le(pkt.content + 51), 0xFFFF);

        // 013 GEO_ALT at offset 53 — unknown = 0
        CHECK_EQ(read_u16le(pkt.content + 53), 0);

        // 015 OP_STATUS at offset 55 — encoded as STATUS_UNREPORTED (0)
        CHECK_EQ(pkt.content[55], (uint8_t)STATUS_UNREPORTED);

        // 020 TIMESTAMP at offset 60-65 — unknown = 0
        for (int i = 0; i < 6; i++) CHECK_EQ(pkt.content[60 + i], 0);

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
        FlightData fd = makeFd(34.5f, 110.25f, 50.0f, 0.0f, 0.0f, STATUS_GROUND);
        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 0ULL);

        // The lat/lon appear twice in content: once for opPos, once for uaPos
        // UAS_ID (20) + REALNAME (8) + OP_CATEGORY (1) + UA_CLASS (1) + OP_LOC_TYPE (1) = 31
        // OP_POS at offset 31: lat(4) + lon(4)
        int32_t opLat_i = read_i32le(pkt.content + 31);
        int32_t opLon_i = read_i32le(pkt.content + 35);
        CHECK_CLOSE((double)opLat_i / 1e7, 34.5, 0.00001);
        CHECK_CLOSE((double)opLon_i / 1e7, 110.25, 0.00001);

        // OP_ALT at offset 39: 2 bytes
        // UA_POS at offset 41: lat(4) + lon(4)
        int32_t uaLat_i = read_i32le(pkt.content + 41);
        int32_t uaLon_i = read_i32le(pkt.content + 45);
        CHECK_CLOSE((double)uaLat_i / 1e7, 34.5, 0.00001);
        CHECK_CLOSE((double)uaLon_i / 1e7, 110.25, 0.00001);
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

    // ==== 20. Golden packet: end-to-end byte-level verification ----
    // Given fixed inputs, verify EVERY byte of serialized output matches hand-calculated values.
    // This catches offset errors, field ordering mistakes, and encoding bugs.
    {
        // Fixed input: known values for all fields
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.lat = 34.500000f;       // 34.5° N
        fd.lon = 110.250000f;      // 110.25° E
        fd.geoAlt = 100.0f;        // 100m MSL
        fd.heightAgl = 50.0f;      // 50m AGL
        fd.baroAlt = 102.0f;       // 102m baro
        fd.speed = 5.5f;           // 5.5 m/s
        fd.heading = 90.0f;        // 90° (East)
        fd.vspeed = 2.0f;          // 2.0 m/s up
        fd.opStatus = STATUS_AIRBORNE;  // 2
        fd.opLat = 34.000000f;     // operator slightly south
        fd.opLon = 110.000000f;
        fd.opAlt = 10.0f;          // operator at 10m
        fd.horizAccM = 5.0f;       // GPS eph=5m → horizAcc=10 (<10m)
        fd.vertAccM = 2.0f;        // GPS epv=2m → vertAcc=5 (<3m)
        fd.validMask = FLD_ALL;    // all fields valid
        fd.freshness = FRESH_OK;
        fd.unixTimestampMs = 1700000000000ULL;  // fixed timestamp

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1,     // opCategory (open)
                           1,     // uaClass (light)
                           0,     // opLocType (takeoff point)
                           0,     // coordSys (WGS-84)
                           10,    // horizAcc
                           5,     // vertAcc
                           3,     // speedAcc
                           5,     // tsAcc
                           fd.unixTimestampMs);

        uint8_t buf[GB46750_MAX_PACKET];
        uint16_t len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);

        // Verify header
        CHECK_EQ(buf[0], 0xFF);   // dataType
        CHECK_EQ(buf[1], 0x20);   // version (V1.0 = 0b001_00000)

        // dataLength should match contentLen
        CHECK_EQ(buf[2], pkt.contentLen);

        // dataId bytes (3 bytes) - verify bitmask pattern per GB 46750 Table 2
        // Byte 0: UPIC(0x80) + REALNAME(0x40) + OP_CAT(0x20) + UA_CLASS(0x10) + OP_LOC_TYPE(0x08) + OP_LOC(0x04) + OP_ALT(0x02) + EXT(0x01)
        CHECK_EQ(buf[3], 0xFF);   // all bits set (M fields + OP_CAT(O) + EXT)

        // Byte 1: UA_POS(0x80) + TRACK(0x40) + SPEED(0x20) + REL_H(0x10) + V_SPD(0x08) + GEO_ALT(0x04) + BARO_ALT(0x02) + EXT(0x01)
        CHECK_EQ(buf[4], 0xFF);   // all bits set (all O fields present because validMask=FLD_ALL)

        // Byte 2: OP_STATUS(0x80) + COORD(0x40) + H_ACC(0x20) + V_ACC(0x10) + S_ACC(0x08) + TS(0x04) + TS_ACC(0x02)
        CHECK_EQ(buf[5], 0xFE);   // all except bit 0 (no extension)

        // Content starts at offset 6 (after 1+1+1+3 bytes header)
        uint8_t* c = buf + 6;
        int off = 0;

        // 001: UAS_ID (20 bytes)
        CHECK_EQ(memcmp(c + off, TEST_UAS, 20), 0);
        off += 20;

        // 002: REALNAME (8 bytes)
        CHECK_EQ(memcmp(c + off, TEST_REAL, 8), 0);
        off += 8;

        // 003: OP_CATEGORY (1 byte)
        CHECK_EQ(c[off], 1);
        off += 1;

        // 004: UA_CLASS (1 byte)
        CHECK_EQ(c[off], 1);
        off += 1;

        // 005: OP_LOC_TYPE (1 byte)
        CHECK_EQ(c[off], 0);
        off += 1;

        // 006: OP_POS (8 bytes: lat4 + lon4)
        int32_t opLat = read_i32le(c + off);
        int32_t opLon = read_i32le(c + off + 4);
        CHECK_CLOSE((double)opLat / 1e7, 34.000000, 0.00001);
        CHECK_CLOSE((double)opLon / 1e7, 110.000000, 0.00001);
        off += 8;

        // 007: OP_ALT (2 bytes): (10 + 1000) * 2 = 2020
        uint16_t opAlt = read_u16le(c + off);
        CHECK_EQ(opAlt, 2020);
        off += 2;

        // 008: UA_POS (8 bytes: lat4 + lon4)
        int32_t uaLat = read_i32le(c + off);
        int32_t uaLon = read_i32le(c + off + 4);
        CHECK_CLOSE((double)uaLat / 1e7, 34.500000, 0.00001);
        CHECK_CLOSE((double)uaLon / 1e7, 110.250000, 0.00001);
        off += 8;

        // 009: TRACK_ANGLE (2 bytes): 90.0 * 10 = 900
        uint16_t hdg = read_u16le(c + off);
        CHECK_EQ(hdg, 900);
        off += 2;

        // 010: GROUND_SPEED (2 bytes): 5.5 * 10 = 55
        uint16_t spd = read_u16le(c + off);
        CHECK_EQ(spd, 55);
        off += 2;

        // 011: REL_HEIGHT (2 bytes, O): (50 + 9000) * 2 = 18100
        uint16_t relH = read_u16le(c + off);
        CHECK_EQ(relH, 18100);
        off += 2;

        // 012: VERT_SPEED (1 byte, O): 2.0 m/s up → dir=0, val=4
        uint8_t vs = c[off];
        CHECK_EQ(vs & 0x80, 0);      // bit7=0: rising
        CHECK_EQ(vs & 0x7F, 4);      // 2.0 * 2 = 4
        off += 1;

        // 013: GEO_ALT (2 bytes): (100 + 1000) * 2 = 2200
        uint16_t geoAlt = read_u16le(c + off);
        CHECK_EQ(geoAlt, 2200);
        off += 2;

        // 014: BARO_ALT (2 bytes, O): (102 + 1000) * 2 = 2204
        uint16_t baroAlt = read_u16le(c + off);
        CHECK_EQ(baroAlt, 2204);
        off += 2;

        // 015: OP_STATUS (1 byte)
        CHECK_EQ(c[off], STATUS_AIRBORNE);
        off += 1;

        // 016: COORD_SYS (1 byte)
        CHECK_EQ(c[off], 0);  // WGS-84
        off += 1;

        // 017-019: Precision (3 bytes)
        CHECK_EQ(c[off], 10);     // HORIZ_ACC
        CHECK_EQ(c[off + 1], 5);  // VERT_ACC
        CHECK_EQ(c[off + 2], 3);  // SPEED_ACC
        off += 3;

        // 020: TIMESTAMP (6 bytes): 1700000000000
        uint64_t ts = 0;
        for (int i = 0; i < 6; i++) {
            ts |= ((uint64_t)c[off + i]) << (i * 8);
        }
        CHECK_EQ(ts, 1700000000000ULL);
        off += 6;

        // 021: TS_ACC (1 byte)
        CHECK_EQ(c[off], 5);
        off += 1;

        // Final check: total content length matches offset
        CHECK_EQ(pkt.contentLen, off);
        CHECK_EQ(len, 1 + 1 + 1 + 3 + off);  // header + dataId + content
    }

    // ==== 21. Independent decode of golden packet (breaks self-consistency loop) ----
    //    Round-trips through the Table 3 decoder defined above and asserts every
    //    decoded field matches the original input. The decoder has no dependency on
    //    the encoder's layout, so an encoder regression shows up as a decode mismatch.
    {
        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        fd.lat = 34.500000f;
        fd.lon = 110.250000f;
        fd.geoAlt = 100.0f;
        fd.heightAgl = 50.0f;
        fd.baroAlt = 102.0f;
        fd.speed = 5.5f;
        fd.heading = 90.0f;
        fd.vspeed = 2.0f;
        fd.opStatus = STATUS_AIRBORNE;
        fd.opLat = 34.000000f;
        fd.opLon = 110.000000f;
        fd.opAlt = 10.0f;
        fd.validMask = FLD_ALL;
        fd.unixTimestampMs = 1700000000000ULL;

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, fd.unixTimestampMs);

        uint8_t buf[GB46750_MAX_PACKET];
        uint16_t len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);

        DecodedFields d;
        CHECK(decodePacket(buf, len, d));

        CHECK_CLOSE((double)d.opLat / 1e7, 34.000000, 0.00001);
        CHECK_CLOSE((double)d.opLon / 1e7, 110.000000, 0.00001);
        CHECK_EQ(d.opAlt, 2020);              // (10+1000)*2
        CHECK_CLOSE((double)d.uaLat / 1e7, 34.500000, 0.00001);
        CHECK_CLOSE((double)d.uaLon / 1e7, 110.250000, 0.00001);
        CHECK_EQ(d.heading, 900);             // 90.0*10
        CHECK_EQ(d.speed, 55);                // 5.5*10
        CHECK_EQ(d.relHeight, 18100);         // (50+9000)*2
        CHECK_EQ(d.vspeed, 4);                // 2.0*2, rising (bit7=0)
        CHECK_EQ(d.geoAlt, 2200);             // (100+1000)*2
        CHECK_EQ(d.baroAlt, 2204);            // (102+1000)*2
        CHECK_EQ(d.opStatus, STATUS_AIRBORNE);
        CHECK_EQ(d.coordSys, 0);
        CHECK_EQ(d.horizAcc, 10);
        CHECK_EQ(d.vertAcc, 5);
        CHECK_EQ(d.speedAcc, 3);
        CHECK_EQ(d.timestamp, 1700000000000ULL);
        CHECK_EQ(d.tsAcc, 5);

        // Decoder must reject malformed buffers
        CHECK(!decodePacket(buf, 3, d));          // truncated
        uint8_t bad[8] = {0xFE, 0x20, 0, 0, 0, 0, 0, 0};
        CHECK(!decodePacket(bad, 8, d));          // wrong dataType / bad length
    }

    // ==== 22. Expire stale fields (gb46750_expireStaleFields) ----
    // GB 46750-2025 表3-008/009/010/013: 数据"未知或不可用"应编码为哨兵值
    // (位置→0xFFFFFFFF, 航迹/速度→0xFFFF, 高度→0), 而不是广播过期的旧坐标 —
    // 防止监管设备依据过期位置做禁飞区/冲突判断时产生安全事故。
    {
        // All fields fresh: nothing cleared
        FlightData fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.ts_pos = 5000; fd.ts_geoAlt = 5000; fd.ts_speed = 5000; fd.ts_heading = 5000;
        gb46750_expireStaleFields(fd, 6000, 2000);   // now=6000, threshold=2000 → age=1000ms
        CHECK(fd.validMask & FLD_POS);
        CHECK(fd.validMask & FLD_GEO_ALT);
        CHECK(fd.validMask & FLD_SPEED);
        CHECK(fd.validMask & FLD_HEADING);

        // Position stale (>threshold): FLD_POS cleared, others kept
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.ts_pos = 3000; fd.ts_geoAlt = 5000; fd.ts_speed = 5000; fd.ts_heading = 5000;
        gb46750_expireStaleFields(fd, 6000, 2000);   // pos age=3000ms > 2000
        CHECK(!(fd.validMask & FLD_POS));
        CHECK(fd.validMask & FLD_GEO_ALT);
        CHECK(fd.validMask & FLD_SPEED);
        CHECK(fd.validMask & FLD_HEADING);

        // Speed + heading stale: cleared; pos/geoAlt kept
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.ts_pos = 5000; fd.ts_geoAlt = 5000; fd.ts_speed = 3000; fd.ts_heading = 3000;
        gb46750_expireStaleFields(fd, 6000, 2000);
        CHECK(fd.validMask & FLD_POS);
        CHECK(fd.validMask & FLD_GEO_ALT);
        CHECK(!(fd.validMask & FLD_SPEED));
        CHECK(!(fd.validMask & FLD_HEADING));

        // opStatus and opPos must NOT be cleared (状态机与起飞点语义，保留上次已知状态)
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.ts_pos = 1000; fd.ts_geoAlt = 1000; fd.ts_speed = 1000; fd.ts_heading = 1000;
        fd.ts_opStatus = 1000; fd.ts_opPos = 1000;
        gb46750_expireStaleFields(fd, 60000, 2000);  // everything very old
        CHECK(fd.validMask & FLD_OP_STATUS);
        CHECK(fd.validMask & FLD_OP_POS);

        // A field already absent stays absent; at-threshold (=threshold) is NOT expired
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.validMask &= ~FLD_HEADING;
        fd.ts_pos = 4000; fd.ts_geoAlt = 4000; fd.ts_speed = 4000;
        gb46750_expireStaleFields(fd, 6000, 2000);   // age=2000 == threshold → kept
        CHECK(fd.validMask & FLD_POS);
        CHECK(fd.validMask & FLD_GEO_ALT);
        CHECK(fd.validMask & FLD_SPEED);
        CHECK(!(fd.validMask & FLD_HEADING));        // was already absent
    }

    // ==== 23. NaN/Inf defense (gb46750_validateFlightData + encode guards) ----
    // GB 46750-2025 表3: 数据"未知或不可用"应编码为哨兵值。浮点字段一旦混入
    // NaN/Inf (如飞控解析异常、传感器无效帧), 若不做防御:
    //   (a) 编码函数 (int32)(float) 直接转换 NaN 是未定义行为 → 可能广播垃圾坐标;
    //   (b) validateFlightData 的 NaN 比较全部为 false → 校验漏判。
    // 本测试同时验证校验层(置位 flags)与编码层(输出未知哨兵)两条防线。
    {
        // --- 校验层: NaN/Inf 必须被识别为非法 ---
        uint32_t flags = 0;
        FlightData fd = makeFd(NAN, NAN, NAN, NAN, NAN, STATUS_AIRBORNE);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_POS);
        CHECK(flags & FLD_GEO_ALT);
        CHECK(flags & FLD_SPEED);
        CHECK(flags & FLD_HEADING);
        CHECK(flags & FLD_OP_POS);   // makeFd 把 opLat/opLon 也设为 NAN

        // +Inf 高度同样判非法
        fd = makeFd(34.5f, 110.25f, INFINITY, 5.0f, 90.0f, STATUS_AIRBORNE);
        CHECK(!gb46750_validateFlightData(fd, flags));
        CHECK(flags & FLD_GEO_ALT);

        // --- 编码层: 字段在位(validMask 置位)但值为 NaN → 输出未知哨兵 ---
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.lat = NAN; fd.lon = NAN; fd.geoAlt = NAN;
        fd.heading = NAN; fd.speed = NAN; fd.vspeed = NAN;
        fd.opLat = NAN; fd.opLon = NAN; fd.opAlt = NAN;

        GB46750Packet pkt;
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);
        uint8_t buf[GB46750_MAX_PACKET];
        uint16_t len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);

        DecodedFields d;
        CHECK(decodePacket(buf, len, d));
        // 位置 → 0xFFFFFFFF
        CHECK_EQ(d.opLat, (int32_t)0xFFFFFFFF);
        CHECK_EQ(d.opLon, (int32_t)0xFFFFFFFF);
        CHECK_EQ(d.uaLat, (int32_t)0xFFFFFFFF);
        CHECK_EQ(d.uaLon, (int32_t)0xFFFFFFFF);
        // 高度 → 0
        CHECK_EQ(d.opAlt, 0);
        CHECK_EQ(d.geoAlt, 0);
        // 航迹/速度 → 0xFFFF
        CHECK_EQ(d.heading, 0xFFFF);
        CHECK_EQ(d.speed, 0xFFFF);
        // 垂直速度 → 0xFF
        CHECK_EQ(d.vspeed, 0xFF);
        // 状态与时间戳不受 NaN 影响
        CHECK_EQ(d.opStatus, STATUS_AIRBORNE);
        CHECK_EQ(d.timestamp, 1700000000ULL);

        // --- 编码层: +Inf 高度 → 0 (与 NaN 行为一致) ---
        fd = makeFd(34.5f, 110.25f, INFINITY, 5.0f, 90.0f, STATUS_AIRBORNE);
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);
        len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);
        CHECK(decodePacket(buf, len, d));
        CHECK_EQ(d.geoAlt, 0);
        // -Inf 速度 → 0xFFFF
        fd = makeFd(34.5f, 110.25f, 100.0f, -INFINITY, 90.0f, STATUS_AIRBORNE);
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);
        len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);
        CHECK(decodePacket(buf, len, d));
        CHECK_EQ(d.speed, 0xFFFF);

        // --- 单一 NaN 字段不影响其它字段 (隔离性) ---
        fd = makeFd(34.5f, 110.25f, 100.0f, 5.0f, 90.0f, STATUS_AIRBORNE);
        fd.speed = NAN;   // 只有速度失效
        gb46750_buildPacket(pkt, fd, TEST_UAS, TEST_REAL,
                           1, 1, 0, 0, 10, 5, 3, 5, 1700000000ULL);
        len = gb46750_serialize(pkt, buf, sizeof(buf));
        CHECK(len > 0);
        CHECK(decodePacket(buf, len, d));
        CHECK_EQ(d.speed, 0xFFFF);
        CHECK_EQ(d.heading, 900);            // 正常值不受影响
        CHECK_EQ(d.geoAlt, 2200);
        CHECK_CLOSE((double)d.uaLat / 1e7, 34.5, 0.00001);
    }

    printf("--- GB 46750-2025 Encoding: ALL PASSED ---\n");
}
