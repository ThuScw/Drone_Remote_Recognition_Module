package com.ridcheck

import com.ridcheck.core.Decoder
import com.ridcheck.core.Health
import com.ridcheck.core.HealthLevel
import com.ridcheck.core.StreamAssessor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** 健康判定测试。移植自 app/tests/test_health.py。 */
class HealthTest {

    @Test
    fun healthyPacketPasses() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(PacketBuilder.buildPacket(timestampMs = System.currentTimeMillis()))
        )
        assertTrue("issues=$issues", issues.isEmpty())
    }

    @Test
    fun missingRealnameWarns() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(
                    realname = "00000000",
                    timestampMs = System.currentTimeMillis()
                )
            )
        )
        val codes = issues.map { it.code }.toSet()
        assertTrue("REALNAME_EMPTY" in codes)
        assertTrue("非全部 WARN: $issues", issues.all { it.level == HealthLevel.WARN })
    }

    @Test
    fun positionUnknownWarns() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(
                    uaPosUnknown = true,
                    timestampMs = System.currentTimeMillis()
                )
            )
        )
        assertTrue(issues.any { it.code == "POS_UNKNOWN" })
    }

    @Test
    fun invalidStatusFails() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(opStatus = 9, timestampMs = System.currentTimeMillis())
            )
        )
        assertTrue(issues.any { it.code == "STATUS_INVALID" })
    }

    @Test
    fun moduleFaultStatusFails() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(opStatus = 5, timestampMs = System.currentTimeMillis())
            )
        )
        assertTrue(issues.any { it.code == "STATUS_FAIL" })
    }

    @Test
    fun wrongVersionWarnsWithClause() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(version = 0x01, timestampMs = System.currentTimeMillis())
            )
        )
        val ver = issues.firstOrNull { it.code == "STRUCT_VER" }
        assertTrue("STRUCT_VER 应存在: $issues", ver != null)
        assertEquals(HealthLevel.WARN, ver!!.level)
        assertEquals("5.2.3 表3 数据格式", ver.clause)
    }

    @Test
    fun uasOidForbiddenWarns() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(
                    uasId = "CPNYMDL00O123456789A",
                    timestampMs = System.currentTimeMillis()
                )
            )
        )
        assertTrue(issues.any { it.code == "UAS_OI" })
    }

    @Test
    fun timestampDriftWarns() {
        val issues = Health.assessPacket(
            Decoder.decodeGbPacket(PacketBuilder.buildPacket(timestampMs = 1700000000000L))
        )
        assertTrue(issues.any { it.code == "TS_DRIFT" })
    }

    @Test
    fun streamAssessorRateAndStaleness() {
        var clockMs = 1_000_000L
        val ass = StreamAssessor(windowS = 10.0, nowFunc = { clockMs })

        var rep = ass.report()
        assertEquals("尚未收包应为 FAIL", HealthLevel.FAIL, rep.level)

        val base = PacketBuilder.buildPacket()
        for (i in 0 until 5) {
            val pkt = Decoder.decodeGbPacket(base, receivedAtMs = clockMs)
            ass.push(pkt)
            clockMs += 800 // 1.25 包/s
        }

        rep = ass.report()
        assertNotEquals(HealthLevel.FAIL, rep.level)
        assertTrue("rate=${rep.avgRateHz}", rep.avgRateHz in 0.8..1.5)
        assertTrue("stale=${rep.staleSeconds}", rep.staleSeconds <= 0.8)

        // 静默 6s → FAIL
        clockMs += 6000
        rep = ass.report()
        assertTrue(rep.issues.any { it.code == "STALE" })
        assertEquals(HealthLevel.FAIL, rep.level)
    }
}
