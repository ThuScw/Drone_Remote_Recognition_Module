package com.ridcheck

import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceRegistry
import com.ridcheck.core.ReportBuilder
import org.junit.Assert.assertTrue
import org.junit.Test

/** 合规测试报告生成测试。 */
class ReportBuilderTest {

    private val atMs = 1700000000000L

    @Test
    fun emptyRegistryReport() {
        val text = ReportBuilder.build(emptyList(), nowMs = atMs)
        assertTrue(text.contains("RID 合规测试报告"))
        assertTrue(text.contains("GB 46750-2025"))
        assertTrue(text.contains("未发现任何设备"))
    }

    @Test
    fun healthyDeviceReportContainsVerdictAndNoIssues() {
        val reg = DeviceRegistry()
        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(timestampMs = System.currentTimeMillis()),
                address = "AA:BB:CC:DD:EE:01",
                rssi = -55,
                receivedAtMs = atMs
            ),
            nowMs = atMs
        )
        val text = ReportBuilder.build(reg.list, nowMs = atMs)
        assertTrue(text.contains("AA:BB:CC:DD:EE:01"))
        assertTrue(text.contains("判定: 正常"))
        assertTrue(text.contains("未发现问题"))
    }

    @Test
    fun issueReportContainsClauseAndLevel() {
        val reg = DeviceRegistry()
        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(version = 0x01, timestampMs = System.currentTimeMillis()),
                address = "AA:BB:CC:DD:EE:02",
                rssi = -70,
                receivedAtMs = atMs
            ),
            nowMs = atMs
        )
        val text = ReportBuilder.build(reg.list, nowMs = atMs)
        assertTrue(text.contains("AA:BB:CC:DD:EE:02"))
        assertTrue(text.contains("判定: 警告"))
        assertTrue(text.contains("STRUCT_VER"))
        assertTrue(text.contains("5.2.3 表3 数据格式"))
    }
}
