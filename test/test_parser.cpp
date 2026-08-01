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

// Deterministic xorshift32 PRNG — 乱码洪流测试用可复现噪声
static uint32_t g_rng = 0x12345678u;
static uint32_t next_u32() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// Collect up to maxN GLOBAL_POSITION_INT (msgid 33) frames with valid heading
static int collect_pos_frames(const TestFrame* out[], int maxN) {
    int n = 0;
    for (int i = 0; i < kNumTestFrames && n < maxN; i++) {
        if (kTestFrames[i].msgid == 33 && kTestFrames[i].expected.hdg >= 0) {
            out[n++] = &kTestFrames[i];
        }
    }
    return n;
}

// 帧头长度: 由 STX 判定 v1(6) / v2(10)
static uint16_t frame_header_len(const TestFrame* tf) {
    return (tf->raw[0] == 0xFD) ? 10 : 6;
}

// --- 构造带指定 payload 的 v2 帧 (CRC 实时计算) ---
static uint16_t build_v2_frame(uint16_t msgid, const uint8_t* payload, uint8_t plen,
                               uint8_t incompat, uint8_t* out) {
    size_t n = 0;
    out[n++] = 0xFD;            // STX v2
    out[n++] = plen;            // LEN
    out[n++] = incompat;        // INCOMPAT
    out[n++] = 0x00;            // COMPAT
    out[n++] = 0x01;            // SEQ
    out[n++] = 1; out[n++] = 1; // SYSID/COMPID
    out[n++] = msgid & 0xFF;
    out[n++] = (msgid >> 8) & 0xFF;
    out[n++] = (msgid >> 16) & 0xFF;
    memcpy(out + n, payload, plen); n += plen;
    uint16_t crc = mavlink_crc_calculate(out + 1, (uint16_t)(n - 1));
    crc = mavlink_crc_accumulate(mavlink_crc_extra(msgid), crc);
    out[n++] = crc & 0xFF;
    out[n++] = crc >> 8;
    return (uint16_t)n;
}

static void put_u32le(uint8_t* b, uint32_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}
static void put_u64le(uint8_t* b, uint64_t v) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

// 构造 GLOBAL_POSITION_INT (msgid 33) 载荷: boot_ms(4)+lat(4)+lon(4)+alt(4)+rel(4)+vx(2)+vy(2)+vz(2)+hdg(2)
static uint16_t build_gpi_frame(uint32_t bootMs, uint8_t* out) {
    uint8_t payload[28] = {0};
    put_u32le(payload, bootMs);
    put_u32le(payload + 4, (uint32_t)(int32_t)(34.5f * 1e7f));   // lat
    put_u32le(payload + 8, (uint32_t)(int32_t)(110.25f * 1e7f)); // lon
    put_u32le(payload + 12, (uint32_t)(int32_t)(100000));        // alt 100m
    put_u32le(payload + 16, (uint32_t)(int32_t)(50000));         // rel 50m
    return build_v2_frame(33, payload, sizeof(payload), 0, out);
}

// 构造 SYSTEM_TIME (msgid 2) 载荷: unix_usec(8)+boot_ms(4)
static uint16_t build_sys_time_frame(uint64_t unixUsec, uint32_t bootMs, uint8_t* out) {
    uint8_t payload[12] = {0};
    put_u64le(payload, unixUsec);
    put_u32le(payload + 8, bootMs);
    return build_v2_frame(2, payload, sizeof(payload), 0, out);
}

// 构造签名 HEARTBEAT 帧 (msgid 0, INCOMPAT bit0=1): 帧体 + CRC + 可选 13 字节签名
// validCrc=false 时翻转 CRC; withSig=false 时不附加签名 (模拟截断签名流)
static uint16_t build_signed_hb(uint8_t* out, bool validCrc, bool withSig) {
    size_t n = 0;
    out[n++] = 0xFD;                    // STX v2
    out[n++] = 9;                       // payload len (HEARTBEAT)
    out[n++] = MAVLINK_IFLAG_SIGNED;    // incompat: signed
    out[n++] = 0x00;                    // compat
    out[n++] = 0x01;                    // seq
    out[n++] = 1; out[n++] = 1;         // sysid/compid
    out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00;  // msgid=0
    uint8_t payload[9] = {0, 0, 0, 0, 6, 2, 0x80, 4, 3};  // armed + status=ACTIVE
    memcpy(out + n, payload, 9); n += 9;
    uint16_t crc = mavlink_crc_calculate(out + 1, (uint16_t)(n - 1));
    crc = mavlink_crc_accumulate(50, crc);   // HEARTBEAT crcExtra = 50
    if (!validCrc) crc ^= 0x00FF;
    out[n++] = crc & 0xFF;
    out[n++] = crc >> 8;
    if (withSig) {
        out[n++] = 0x5A;                // link_id (签名块首字节)
        uint64_t ts = 0x112233445566ULL;
        for (int i = 0; i < 6; i++) out[n++] = (uint8_t)((ts >> (8 * i)) & 0xFF);
        for (int i = 0; i < 6; i++) out[n++] = 0xAB;   // 任意签名
    }
    return (uint16_t)n;
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
    // 真实帧 payload 长度 16-20B 都有: 16-17B 截断帧无 heading,
    // 必须不越界 (读到 CRC 字节) 且不得把垃圾字节当航向
    {
        int lenShort = 0, lenFull = 0;
        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            if (tf.msgid != 74) continue;

            uint8_t plen = tf.raw[1];
            MavlinkParser p;
            mavlink_init(p);
            bool got = feed_frame(p, tf.raw, tf.rawLen, 5000);
            CHECK(got);

            // groundspeed/climb 在 >=16B 帧中有效 (pymavlink ground truth)
            CHECK_CLOSE(p.groundspeed, tf.expected.groundspeed, 0.01);
            CHECK_CLOSE(p.climbRate,   tf.expected.climb,     0.01);

            // 短帧 (<18B): heading 保持 NAN, 不被 CRC/垃圾字节污染
            if (plen < 18) {
                lenShort++;
                CHECK(std::isnan(p.heading));
            } else if (tf.expected.vfrHdgValid) {
                lenFull++;
                CHECK_CLOSE(p.heading, tf.expected.vfrHdg, 0.1);
            }
        }
        printf("  VFR_HUD: short(<18B)=%d full(>=18B)=%d\n", lenShort, lenFull);
        CHECK(lenShort > 0);  // 短帧回归必须有真实样本
        CHECK(lenFull > 0);
    }

    // ==== 14. HOME_POSITION decoding from real flight data ----
    {
        // Pick first HOME_POSITION frame (msgid=242) with valid home from test data
        const TestFrame* home = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 242 && kTestFrames[i].expected.homeValid) {
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

    // ==== 17. SYSTEM_TIME decoding (incl. 11-byte truncated variant) ----
    // 真实飞控发送 11B 变体 (unix_usec 8B + boot_ms 低 3 字节) 和标准 12B
    {
        int n11 = 0, n12 = 0;
        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];
            if (tf.msgid != 2) continue;

            MavlinkParser p;
            mavlink_init(p);
            bool got = feed_frame(p, tf.raw, tf.rawLen, 5000);
            CHECK(got);

            CHECK(p.unixTimeValid);
            int64_t expOffset = (int64_t)(tf.expected.unixUsec / 1000ULL)
                                - (int64_t)tf.expected.bootMs;
            CHECK_EQ(p.unixBootOffsetMs, expOffset);
            CHECK_EQ(p.lastSystemTimeMs, 5000u);

            if (tf.raw[1] == 11) n11++;
            else if (tf.raw[1] == 12) n12++;
        }
        printf("  SYSTEM_TIME: 11B=%d 12B=%d\n", n11, n12);
        CHECK(n11 > 0);  // 真实 11B 截断变体必须被解码
        CHECK(n12 > 0);  // 标准 12B 也必须被解码
    }

    // ==== 18. 粘包: 两条消息背靠背无缝拼接 ====
    // 模拟 UART/DMA 突发: HEARTBEAT 与位置帧紧挨着到达, 中间无间隔, 两帧都必须解析
    {
        const TestFrame* hb  = nullptr;
        const TestFrame* pos = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (!hb  && kTestFrames[i].msgid == 0  && kTestFrames[i].expected.armed == 1)
                hb  = &kTestFrames[i];
            if (!pos && kTestFrames[i].msgid == 33 && kTestFrames[i].expected.hdg >= 0)
                pos = &kTestFrames[i];
        }
        CHECK(hb && pos);

        uint8_t glued[256];
        uint16_t glen = 0;
        memcpy(glued + glen, hb->raw,  hb->rawLen);   glen += hb->rawLen;
        memcpy(glued + glen, pos->raw, pos->rawLen);  glen += pos->rawLen;

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, glued, glen, 1000);

        CHECK_EQ(p.totalFrames, 2u);        // 两帧都通过 CRC 并解析
        CHECK_EQ(p.crcErrors, 0u);
        CHECK(p.armed);                     // HEARTBEAT 先到
        CHECK_CLOSE(p.lat, pos->expected.lat, 0.0001);  // 位置帧后到, 数据保留
        CHECK_CLOSE(p.lon, pos->expected.lon, 0.0001);
    }

    // ==== 19. 中途丢字节 / 截断 → 重新同步 ====
    // 19a: payload 中间丢 1 字节 + 1 噪声字节 + 2 完整帧
    //      解析器必须检测到 CRC 失败, 回 IDLE 后完整解析后续两帧
    {
        const TestFrame* pos[3];
        CHECK(collect_pos_frames(pos, 3) == 3);

        uint16_t hdr = frame_header_len(pos[0]);
        uint16_t dropIdx = hdr + pos[0]->raw[1] / 2;  // payload 中间

        uint8_t stream[512];
        uint16_t slen = 0;
        memcpy(stream + slen, pos[0]->raw, dropIdx);            slen += dropIdx;
        memcpy(stream + slen, pos[0]->raw + dropIdx + 1,
               pos[0]->rawLen - dropIdx - 1);                   slen += pos[0]->rawLen - dropIdx - 1;
        stream[slen++] = 0x00;  // 1 字节噪声填补缺口
        memcpy(stream + slen, pos[1]->raw, pos[1]->rawLen);     slen += pos[1]->rawLen;
        memcpy(stream + slen, pos[2]->raw, pos[2]->rawLen);     slen += pos[2]->rawLen;

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, stream, slen, 1000);

        CHECK_EQ(p.crcErrors, 1u);          // 丢字节的帧被检测
        CHECK_EQ(p.totalFrames, 2u);        // 后续两帧完整解析
        CHECK_CLOSE(p.lat, pos[2]->expected.lat, 0.0001);
        CHECK_CLOSE(p.lon, pos[2]->expected.lon, 0.0001);
    }

    // 19b: 帧截断为一半后紧跟 20 个完整帧 → 重新锁定, 绝大多数后续帧解析成功
    {
        const TestFrame* pos[21];
        CHECK(collect_pos_frames(pos, 21) == 21);

        uint8_t stream[64 + 21 * 64];
        uint16_t slen = 0;
        uint16_t half = pos[0]->rawLen / 2;
        memcpy(stream + slen, pos[0]->raw, half);  slen += half;  // 半截帧0
        for (int i = 1; i <= 20; i++) {
            memcpy(stream + slen, pos[i]->raw, pos[i]->rawLen);
            slen += pos[i]->rawLen;
        }

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, stream, slen, 1000);

        CHECK(p.crcErrors >= 1);             // 半截帧被检测
        CHECK(p.totalFrames >= 17);          // 20 帧中绝大多数重新锁定
        CHECK_CLOSE(p.lat, pos[20]->expected.lat, 0.0001);
        CHECK_CLOSE(p.lon, pos[20]->expected.lon, 0.0001);
    }

    // ==== 20. 随机乱码洪流 → 自愈恢复 ====
    // 20a: 3000 字节全随机乱码后跟 30 个有效帧 (模拟电磁干扰位翻转 / 信道噪声)
    //      乱码大多命中未知 msgid (静默跳过, 不计 crcErrors), 真正的考验是:
    //      洪流结束后解析器能否重新锁定, 恢复出大多数后续有效帧
    {
        const TestFrame* pos[30];
        CHECK(collect_pos_frames(pos, 30) == 30);

        uint8_t storm[3000];
        g_rng = 0x12345678u;
        for (int i = 0; i < 3000; i++) storm[i] = (uint8_t)next_u32();

        uint8_t stream[3000 + 30 * 64];
        uint16_t slen = 0;
        memcpy(stream + slen, storm, 3000);  slen += 3000;
        for (int i = 0; i < 30; i++) {
            memcpy(stream + slen, pos[i]->raw, pos[i]->rawLen);
            slen += pos[i]->rawLen;
        }

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, stream, slen, 1000);

        // 单个假帧最多吞 ~265 字节 (payloadLen≤253), 30 帧里至少恢复出 20 帧
        CHECK(p.totalFrames >= 20);
    }

    // 20b: 噪声穿插在有效帧之间 (无 STX 字节的乱码), 每帧都必须解析且计数器自愈归零
    {
        const TestFrame* pos[10];
        CHECK(collect_pos_frames(pos, 10) == 10);

        uint8_t stream[10 * (100 + 64)];
        uint16_t slen = 0;
        g_rng = 0xABCDEF01u;
        for (int i = 0; i < 10; i++) {
            // 100 字节噪声 (排除 0xFD/0xFE, 否则会误启假帧)
            for (int j = 0; j < 100; j++) {
                uint8_t b;
                do { b = (uint8_t)next_u32(); } while (b == 0xFD || b == 0xFE);
                stream[slen++] = b;
            }
            memcpy(stream + slen, pos[i]->raw, pos[i]->rawLen);
            slen += pos[i]->rawLen;
        }

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, stream, slen, 1000);

        CHECK_EQ(p.totalFrames, 10u);        // 每帧都在噪声中完整解析
        CHECK_EQ(p.crcErrors, 0u);           // 无 STX 噪声不产生假帧
        CHECK_EQ(p.consecutiveCrcErrors, 0u); // 连续 CRC 计数器保持归零
    }

    // ==== 21. payload 位翻转被 CRC 拒绝 ====
    // 模拟电磁干扰位翻转 (非 CRC 字节): 篡改 payload 中间 1 位, 帧必须被 CRC 拒绝
    {
        const TestFrame* pos = nullptr;
        for (int i = 0; i < kNumTestFrames; i++) {
            if (kTestFrames[i].msgid == 33 && kTestFrames[i].expected.hdg >= 0) {
                pos = &kTestFrames[i];
                break;
            }
        }
        CHECK(pos);

        uint8_t corrupted[128];
        memcpy(corrupted, pos->raw, pos->rawLen);
        uint16_t hdr = frame_header_len(pos);
        corrupted[hdr + 5] ^= 0x04;  // payload 中某一位翻转

        MavlinkParser p;
        mavlink_init(p);
        feed_frame(p, corrupted, pos->rawLen, 1000);

        CHECK_EQ(p.totalFrames, 0u);            // 帧被拒绝
        CHECK_EQ(p.crcErrors, 1u);
        CHECK_EQ(p.consecutiveCrcErrors, 1u);
    }

    // ==== 22. FC reboot boot_ms rollover detection (GB 46750-2025 8.2.1.3.3) ----
    // 飞控重启后 boot_ms 从 0 重新计时, 若仍用旧 unixBootOffsetMs 拼时间戳会大幅倒退,
    // 违反相邻帧时间戳差 ≤1s。检测到回绕后作废时间戳 (编码侧输出表3-020 未知=0)。
    {
        uint8_t frame[64];

        // 22a: 正常单调递增 + 时间戳计算
        MavlinkParser p;
        mavlink_init(p);

        // SYSTEM_TIME 建立偏移: unix=1700000000000ms, boot=5000000ms → offset = 1699995000000
        uint64_t unixMs = 1700000000000ULL;
        uint16_t slen = build_sys_time_frame(unixMs * 1000ULL, 5000000u, frame);
        CHECK(feed_frame(p, frame, slen, 1000));
        CHECK(p.unixTimeValid);
        CHECK_EQ(p.bootRollovers, 0u);
        CHECK_EQ(p.lastPositionBootMs, 5000000u);  // SYSTEM_TIME 已重新锚定基线

        // 位置帧 boot 前进 100ms → 时间戳 = offset + 5000100 = 1700000000100
        slen = build_gpi_frame(5000100u, frame);
        CHECK(feed_frame(p, frame, slen, 2000));
        CHECK_EQ(p.bootRollovers, 0u);   // 单调递增, 不触发

        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        CHECK(mavlink_fillFlightData(p, fd, 2000));
        CHECK_EQ(fd.unixTimestampMs, unixMs + 100);   // 1700000000000 + 100

        // 22b: 飞控重启 — boot_ms 从 5000100 掉到 100 → 时间戳作废
        slen = build_gpi_frame(100u, frame);
        CHECK(feed_frame(p, frame, slen, 3000));
        CHECK_EQ(p.bootRollovers, 1u);
        CHECK(!p.unixTimeValid);
        CHECK_EQ(p.lastPositionBootMs, 100u);

        memset(&fd, 0, sizeof(fd));
        CHECK(mavlink_fillFlightData(p, fd, 3000));
        CHECK_EQ(fd.unixTimestampMs, 0ULL);   // 编码侧按表3-020 输出未知(0), 不广播错误旧时间戳

        // 22c: 新 SYSTEM_TIME 恢复时间域
        slen = build_sys_time_frame((unixMs + 5000ULL) * 1000ULL, 300u, frame);
        CHECK(feed_frame(p, frame, slen, 4000));
        CHECK(p.unixTimeValid);

        slen = build_gpi_frame(400u, frame);
        CHECK(feed_frame(p, frame, slen, 4000));
        CHECK_EQ(p.bootRollovers, 1u);            // 不新增
        CHECK_EQ(p.lastPositionBootMs, 400u);

        memset(&fd, 0, sizeof(fd));
        CHECK(mavlink_fillFlightData(p, fd, 4000));
        CHECK_EQ(fd.unixTimestampMs, unixMs + 5000ULL - 300ULL + 400ULL);  // = unixMs + 5100
    }

    // ==== 23. SYSTEM_TIME-first reboot: no false-positive rollover ----
    // 飞控重启后 SYSTEM_TIME 先到 (重新锚定基线), 后续位置帧 boot 小但同纪元, 不得误判回绕
    {
        uint8_t frame[64];
        MavlinkParser p;
        mavlink_init(p);

        // 旧状态: 位置帧 boot=5000000 (建立旧基线)
        uint16_t slen = build_gpi_frame(5000000u, frame);
        CHECK(feed_frame(p, frame, slen, 1000));

        // 重启后 SYSTEM_TIME 先到: unix=1700000100000ms, boot=800
        uint64_t unixMs = 1700000100000ULL;
        slen = build_sys_time_frame(unixMs * 1000ULL, 800u, frame);
        CHECK(feed_frame(p, frame, slen, 2000));
        CHECK(p.unixTimeValid);
        CHECK_EQ(p.lastPositionBootMs, 800u);  // 基线已重锚到新纪元

        // 重启后首帧位置: boot=1000 (新纪元小值) → 不得误判回绕
        slen = build_gpi_frame(1000u, frame);
        CHECK(feed_frame(p, frame, slen, 3000));
        CHECK_EQ(p.bootRollovers, 0u);          // 无假阳性
        CHECK(p.unixTimeValid);

        FlightData fd;
        memset(&fd, 0, sizeof(fd));
        CHECK(mavlink_fillFlightData(p, fd, 3000));
        // offset = 1700000100000 - 800; ts = offset + 1000 = unixMs + 200
        CHECK_EQ(fd.unixTimestampMs, unixMs + 200);
    }

    // ==== 24. 签名帧非阻塞: CRC 先校验 (问题 P1-6) ----
    // 签名帧的 13 字节签名可能因流中断/噪声/飞控实际不发送而永远不来。
    // 旧逻辑要等 expectedLen (含签名) 才校验 CRC → 解析器卡在 CRC 状态,
    // 截断签名还会让 CRC 在错误时机读取垃圾字节。修复后 CRC 一收齐立即校验。
    {
        // 24a: 签名帧 CRC 有效但签名未送达 (仅发 21 字节) → CRC 不被签名阻塞, 帧照常解析
        MavlinkParser p;
        mavlink_init(p);

        uint8_t frame[64];
        uint16_t len = build_signed_hb(frame, true, false);   // 有效 CRC, 无签名
        CHECK_EQ(len, 21u);                                   // 10+9+2
        bool got = feed_frame(p, frame, len, 1000);
        CHECK(got);                    // CRC 通过, 帧解析成功
        CHECK_EQ(p.totalFrames, 1u);
        CHECK_EQ(p.crcErrors, 0u);
        CHECK(p.armed);                // HEARTBEAT 正常解码

        // 补发 13 字节任意签名尾 → 消费完毕复位, 不卡死
        uint8_t filler[13] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,
                              0x17,0x18,0x19,0x1A,0x1B,0x1C};
        for (size_t i = 0; i < 13; i++) mavlink_parseByte(p, filler[i], 1000);
        CHECK_EQ((int)p.state, (int)PARSE_STATE_IDLE);   // 签名尾部消费完毕复位

        // 后续有效位置帧正常解析 (解析器未被签名等待卡死)
        uint8_t gpi[64];
        uint16_t glen = build_gpi_frame(100u, gpi);
        CHECK(feed_frame(p, gpi, glen, 2000));
        CHECK_EQ(p.totalFrames, 2u);
        CHECK_EQ(p.crcErrors, 0u);
    }

    {
        // 24b: 签名帧 CRC 错误 → 立即拒绝 (不等签名), 解析器立即复位不卡死
        MavlinkParser p;
        mavlink_init(p);

        uint8_t frame[64];
        uint16_t len = build_signed_hb(frame, false, false);  // CRC 损坏, 无签名
        feed_frame(p, frame, len, 1000);
        CHECK_EQ(p.totalFrames, 0u);
        CHECK_EQ(p.crcErrors, 1u);
        CHECK_EQ(p.consecutiveCrcErrors, 1u);
        CHECK_EQ((int)p.state, (int)PARSE_STATE_IDLE);   // 立即复位, 不等签名

        // 后续有效帧可正常解析
        uint8_t gpi[64];
        uint16_t glen = build_gpi_frame(100u, gpi);
        CHECK(feed_frame(p, gpi, glen, 2000));
        CHECK_EQ(p.totalFrames, 1u);
    }
}
