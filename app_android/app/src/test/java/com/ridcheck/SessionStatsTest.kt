package com.ridcheck

import com.ridcheck.core.DecodedPacket
import com.ridcheck.core.Decoder
import com.ridcheck.core.SessionStats
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** 表3 21 项会话统计测试：数值范围/均值、枚举占比、身份恒定/变化、坐标跨度、未上报、精度未知、时间戳。 */
class SessionStatsTest {

    private fun pkt(
        uasId: String = "CPNYMDL001234567890A",
        realname: String = "12345678",
        opCategory: Int = 1,
        opLocType: Int = 0,
        opLat: Double = 31.2304,
        opLon: Double = 121.4737,
        uaLat: Double = 31.2305,
        uaLon: Double = 121.4738,
        speed: Double = 3.5,
        relHeight: Double? = 120.0,
        opStatus: Int = 2,
        horizAcc: Int = 10,
        timestampMs: Long = 1700000000000L,
        atMs: Long = 1000
    ): DecodedPacket = Decoder.decodeGbPacket(
        PacketBuilder.buildPacket(
            uasId = uasId,
            realname = realname,
            opCategory = opCategory,
            opLocType = opLocType,
            opLat = opLat,
            opLon = opLon,
            uaLat = uaLat,
            uaLon = uaLon,
            speed = speed,
            relHeight = relHeight,
            opStatus = opStatus,
            horizAcc = horizAcc,
            timestampMs = timestampMs
        ),
        address = "AA:BB:CC:DD:EE:01",
        rssi = -55,
        receivedAtMs = atMs
    )

    @Test
    fun numericRangeTracksMinMaxMean() {
        val s = SessionStats()
        s.record(pkt(speed = 1.0, atMs = 1000))
        s.record(pkt(speed = 5.0, atMs = 2000))
        s.record(pkt(speed = 3.0, atMs = 3000))
        val t = s.fieldSummary(10)
        assertTrue(t, t.contains("1.0~5.0 m/s"))
        assertTrue(t, t.contains("均值 3.0"))
    }

    @Test
    fun identityConstantWhenSameId() {
        val s = SessionStats()
        s.record(pkt())
        s.record(pkt())
        assertTrue(s.fieldSummary(1).contains("恒定：CPNYMDL001234567890A"))
    }

    @Test
    fun identityChangeFlagged() {
        val s = SessionStats()
        s.record(pkt())
        s.record(pkt(uasId = "CPNYMDL009999999999Z"))
        val t = s.fieldSummary(1)
        assertTrue(t, t.contains("变化"))
        assertTrue(t, t.contains("2 个不同值"))
    }

    @Test
    fun realnameDefaultFlagged() {
        val s = SessionStats()
        s.record(pkt(realname = "00000000"))
        assertTrue(s.fieldSummary(2).contains("默认值"))
    }

    @Test
    fun enumPercentForCategory() {
        val s = SessionStats()
        s.record(pkt(opCategory = 1, atMs = 1000))
        s.record(pkt(opCategory = 1, atMs = 2000))
        s.record(pkt(opCategory = 2, atMs = 3000))
        val t = s.fieldSummary(3)
        assertTrue(t, t.contains("开放类 67%"))
        assertTrue(t, t.contains("特定类 33%"))
    }

    @Test
    fun operatorPosMarkedAsTakeoffWhenAlwaysTakeoff() {
        val s = SessionStats()
        s.record(pkt(opLocType = 0, atMs = 1000))
        s.record(pkt(opLocType = 0, atMs = 2000))
        val t = s.fieldSummary(6)
        assertTrue(t, t.contains("起飞点，恒定"))
    }

    @Test
    fun uaPosSpanComputed() {
        val s = SessionStats()
        s.record(pkt(uaLat = 31.2305, uaLon = 121.4738, atMs = 1000))
        s.record(pkt(uaLat = 31.2310, uaLon = 121.4740, atMs = 2000))
        val t = s.fieldSummary(8)
        assertTrue(t, t.contains("跨度"))
    }

    @Test
    fun optionalFieldNotBroadcastIsMarked() {
        val s = SessionStats()
        s.record(pkt(relHeight = null)) // 该帧未携带 011
        assertEquals("未上报", s.fieldSummary(11))
    }

    @Test
    fun accuracyUnknownRatioAndDistribution() {
        val s = SessionStats()
        s.record(pkt(horizAcc = 0, atMs = 1000))  // 0 = 未知/超最大档
        s.record(pkt(horizAcc = 10, atMs = 2000)) // <10m
        val t = s.fieldSummary(17)
        assertTrue(t, t.contains("未知 1/2 次(50%)"))
        assertTrue(t, t.contains("<10m 1 次"))
    }

    @Test
    fun tsSummaryShowsRangeAndSynced() {
        val s = SessionStats()
        s.record(pkt(timestampMs = 1700000000000L, atMs = 1000))
        s.record(pkt(timestampMs = 1700000030000L, atMs = 3000))
        val t = s.fieldSummary(20)
        assertTrue(t, t.contains("已授时，设备 UTC"))
        assertTrue(t, t.contains("0:30"))
    }

    @Test
    fun tsUnsyncedAllZero() {
        val s = SessionStats()
        s.record(pkt(timestampMs = 0L, atMs = 1000))
        val t = s.fieldSummary(20)
        assertTrue(t, t.contains("未授时"))
    }

    @Test
    fun statusUsesProvidedTimeline() {
        val s = SessionStats()
        s.record(pkt(opStatus = 1, atMs = 1000))
        s.record(pkt(opStatus = 2, atMs = 2000))
        assertEquals("地面(00:10)→空中(00:20)", s.fieldSummary(15, "地面(00:10)→空中(00:20)"))
    }
}
