// CRC-16/MCRF4XX: verified against real MAVLink flight data
#include "test_common.h"
#include "mavlink_crc.h"
#include "test_data.h"

void test_mavlink_crc() {
    printf("--- CRC-16/MCRF4XX (real flight data) ---\n");

    // 1. CRC extra byte constants — MAVLink spec
    CHECK_EQ(mavlink_crc_extra(0),   50);   // HEARTBEAT
    CHECK_EQ(mavlink_crc_extra(24),  24);   // GPS_RAW_INT
    CHECK_EQ(mavlink_crc_extra(30),  39);   // ATTITUDE
    CHECK_EQ(mavlink_crc_extra(33),  104);  // GLOBAL_POSITION_INT
    CHECK_EQ(mavlink_crc_extra(74),  20);   // VFR_HUD
    CHECK_EQ(mavlink_crc_extra(36),  222);  // SERVO_OUTPUT_RAW
    CHECK_EQ(mavlink_crc_extra(105), 93);   // HIGHRES_IMU
    CHECK_EQ(mavlink_crc_extra(242), 104);  // HOME_POSITION
    CHECK_EQ(mavlink_crc_extra(2),   137);  // SYSTEM_TIME

    // 2. Empty data → 0xFFFF
    CHECK_EQ(mavlink_crc_calculate(nullptr, 0), 0xFFFF);

    // 3. Real frames: CRC of every frame must match known-good CRC from pymavlink
    {
        int matches = 0, mismatches = 0;
        for (int i = 0; i < kNumTestFrames; i++) {
            const TestFrame& tf = kTestFrames[i];

            uint8_t stxLen = (tf.ver == 2) ? 1 : 1;
            uint8_t hdrCrcLen = (tf.ver == 2) ? 9 : 5;
            uint16_t payLen = tf.raw[1];

            uint16_t crcData = mavlink_crc_calculate(tf.raw + stxLen, hdrCrcLen + payLen);
            uint16_t crcFull = mavlink_crc_accumulate(mavlink_crc_extra(tf.msgid), crcData);

            if (crcFull != tf.crc) {
                fprintf(stderr, "  CRC mismatch frame %d (src=%s msgid=%d ver=%d): "
                        "calc=0x%04X expected=0x%04X\n",
                        i, tf.source, tf.msgid, tf.ver, crcFull, tf.crc);
                mismatches++;
            } else {
                matches++;
            }
        }
        CHECK_EQ(mismatches, 0);
        CHECK_EQ(matches, kNumTestFrames);
        printf("  %d/%d frames CRC-verified against pymavlink\n", matches, kNumTestFrames);
    }

    // 4. Determinism
    uint8_t buf[] = {0xFD, 0x09, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
    CHECK_EQ(mavlink_crc_calculate(buf, 10), mavlink_crc_calculate(buf, 10));

    // 5. Different inputs → different CRC
    uint8_t a[] = {0x00, 0x01, 0x02};
    uint8_t b[] = {0x00, 0x01, 0x03};
    CHECK(mavlink_crc_calculate(a, 3) != mavlink_crc_calculate(b, 3));
}
