package com.ridcheck

import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** 设备注册表测试：多设备分离、字段更新、有序、每设备独立健康判定。 */
class DeviceRegistryTest {

    private fun pkt(address: String, rssi: Int, atMs: Long) =
        Decoder.decodeGbPacket(
            PacketBuilder.buildPacket(timestampMs = System.currentTimeMillis()),
            address = address,
            rssi = rssi,
            receivedAtMs = atMs
        )

    @Test
    fun multipleDevicesKeptSeparately() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.onPacket(pkt("AA:BB:CC:DD:EE:02", -70, 2000), nowMs = 2000)

        assertEquals(2, reg.size)
        assertEquals(
            listOf("AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"),
            reg.list.map { it.address }
        )
    }

    @Test
    fun fieldsUpdatedOnEachPacket() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -60, 2000), nowMs = 2000)

        assertEquals(1, reg.size)
        assertEquals(2, a.packetCount)
        assertEquals(-60, a.rssi)
        assertEquals(2000, a.lastSeenMs)
        assertEquals(1000, a.firstSeenMs)
        assertTrue(a.lastPkt != null)
        assertTrue(a.lastRaw.isNotEmpty())
    }

    @Test
    fun assessorIndependentPerDevice() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        val b = reg.onPacket(pkt("AA:BB:CC:DD:EE:02", -70, 1000), nowMs = 1000)

        // 两个 assessor 各累计 1 包，互不影响
        assertEquals(1, a.assessor.report().packetsSeen)
        assertEquals(1, b.assessor.report().packetsSeen)
    }

    @Test
    fun repeatedPacketsFromSameDeviceReuseEntry() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1500), nowMs = 1500)
        reg.onPacket(pkt("AA:BB:CC:DD:EE:02", -70, 2000), nowMs = 2000)

        assertEquals(2, reg.size)
        assertEquals(2, reg.list[0].packetCount)
        assertEquals(1, reg.list[1].packetCount)
    }

    @Test
    fun clearEmptiesList() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.clear()
        assertEquals(0, reg.size)
    }

    @Test
    fun sampleAllAppendsOnePerSecondAndCaps() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)

        // 每秒一步，共 700 步 → 只保留最近 600 条
        repeat(700) { reg.sampleAll(1000 + (it + 1) * 1000L) }

        val e = reg.list.single()
        assertEquals(600, e.samples.size)
        assertEquals(701000, e.samples.last().timeMs) // 最后一步 nowMs = 1000+700*1000
        assertEquals(102000, e.samples.first().timeMs) // 裁掉最旧 100 条后
        assertEquals(-55, e.samples.last().rssi)
    }

    @Test
    fun sampleAllThrottlesWithinSameSecond() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)

        reg.sampleAll(2000) // 首采
        reg.sampleAll(2500) // 距上次 500ms → 跳过
        reg.sampleAll(3000) // 距上次 1000ms → 再采
        assertEquals(2, reg.list.single().samples.size)
    }

    @Test
    fun framesArchivedWithFieldSeenCounts() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -60, 2000), nowMs = 2000)

        assertEquals(2, a.frames.size)
        assertEquals(2000, a.frames.last().timeMs)
        assertEquals(-60, a.frames.last().rssi)
        assertTrue(a.frames.last().raw.isNotEmpty())
        // 表3 字段携带计数：默认包携带全部 21 字段
        assertEquals(2, a.fieldSeen[1])
        assertEquals(2, a.fieldSeen[21])
        assertEquals(0, a.fieldSeen[0])
    }

    @Test
    fun framesCappedAtLimit() {
        val reg = DeviceRegistry()
        reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        val e = reg.list.single()
        repeat(DeviceRegistry.FRAME_CAP + 100) { i ->
            reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 2000L + i), nowMs = 2000L + i)
        }
        assertEquals(DeviceRegistry.FRAME_CAP, e.frames.size)
        // 最旧被裁：首帧 timeMs=1000 应已移出
        assertTrue(e.frames.none { it.timeMs == 1000L })
    }

    @Test
    fun statusLogAndStructErrTracked() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000) // opStatus 2（默认）
        assertEquals(1, a.statusLog.size)
        assertEquals(2, a.statusLog.last().opStatus)

        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(opStatus = 4),
                address = "AA:BB:CC:DD:EE:01", rssi = -55, receivedAtMs = 2000
            ),
            nowMs = 2000
        )
        assertEquals(2, a.statusLog.size)
        assertEquals(4, a.statusLog.last().opStatus)

        // 结构错误包 → structErrCount 累计
        reg.onPacket(
            Decoder.decodeGbPacket(
                byteArrayOf(0xFE.toByte(), 0x20, 5),
                address = "AA:BB:CC:DD:EE:01", rssi = -55, receivedAtMs = 3000
            ),
            nowMs = 3000
        )
        assertEquals(1, a.structErrCount)
    }

    @Test
    fun sessionStatsRecordedPerPacket() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        assertEquals(1, a.sessionStats.accums[10].seen) // 地速
        assertTrue(a.sessionStats.fieldSummary(10).contains("3.5 m/s"))
    }

    @Test
    fun trackSampledFromValidPosition() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.sampleAll(2000)
        assertEquals(1, a.track.size)
        assertEquals(0.0, a.track.first().relN, 1e-6) // 首点为原点
        assertEquals(0.0, a.track.first().relE, 1e-6)
        reg.sampleAll(3000)
        assertEquals(2, a.track.size)
    }

    @Test
    fun trackSkippedWhenPositionUnknown() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(uaPosUnknown = true),
                address = "AA:BB:CC:DD:EE:01", rssi = -55, receivedAtMs = 1000
            ),
            nowMs = 1000
        )
        reg.sampleAll(2000)
        assertEquals(0, a.track.size)
    }

    @Test
    fun issueTimelineFedBySampling() {
        val reg = DeviceRegistry()
        val a = reg.onPacket(pkt("AA:BB:CC:DD:EE:01", -55, 1000), nowMs = 1000)
        reg.sampleAll(2000)
        // 健康包无问题 → 时段为空
        assertEquals(0, a.issueTimeline.size)
    }
}
