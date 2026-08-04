package com.ridcheck

import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceRegistry
import com.ridcheck.core.ReportBuilder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** 单设备合规报告与历史 CSV 生成测试。 */
class ReportBuilderTest {

    private val atMs = 1700000000000L

    private fun regWithOneDevice(version: Int = 0x20): DeviceRegistry {
        val reg = DeviceRegistry()
        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(version = version, timestampMs = System.currentTimeMillis()),
                address = "AA:BB:CC:DD:EE:01",
                rssi = -55,
                receivedAtMs = atMs
            ),
            nowMs = atMs
        )
        return reg
    }

    @Test
    fun deviceReportContainsVerdictAndNoIssues() {
        val entry = regWithOneDevice().list.single()
        val text = ReportBuilder.buildDevice(entry, nowMs = atMs)
        assertTrue(text.contains("RID 合规测试报告"))
        assertTrue(text.contains("AA:BB:CC:DD:EE:01"))
        assertTrue(text.contains("判定: 正常"))
        assertTrue(text.contains("未发现问题"))
    }

    @Test
    fun deviceReportContainsClauseAndSparkline() {
        val reg = regWithOneDevice(version = 0x01) // 旧版本 → STRUCT_VER WARN
        val entry = reg.list.single()
        reg.sampleAll(atMs + 1000)
        reg.sampleAll(atMs + 2000)

        val text = ReportBuilder.buildDevice(entry, nowMs = atMs + 2000)
        assertTrue(text.contains("判定: 警告"))
        assertTrue(text.contains("STRUCT_VER"))
        assertTrue(text.contains("5.2.3 表3 数据格式"))
        assertTrue(text.contains("RSSI 采样曲线"))
        assertTrue(text.contains("▁") || text.contains("▂") || text.contains("█"))
    }

    @Test
    fun deviceReportFallsBackWhenNoSamples() {
        val entry = regWithOneDevice().list.single() // 未采样
        val text = ReportBuilder.buildDevice(entry, nowMs = atMs)
        assertTrue(text.contains("(无采样)"))
        assertTrue(text.contains("RSSI: -55 dBm"))
    }

    @Test
    fun csvHeaderAndRows() {
        val reg = regWithOneDevice()
        val entry = reg.list.single()
        reg.sampleAll(atMs + 1000)
        reg.sampleAll(atMs + 2000)

        val csv = ReportBuilder.buildCsv(entry)
        val lines = csv.trim().split("\n")
        assertEquals("time_utc,elapsed_s,rssi_dbm,rate_pkts,level", lines[0])
        assertEquals(entry.samples.size, lines.size - 1)
        val cols = lines[1].split(",")
        assertEquals("-55", cols[2])
        assertTrue(cols[4] in listOf("PASS", "WARN", "FAIL"))
    }
}
