package com.ridcheck

import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceRegistry
import com.ridcheck.core.ReportBuilder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.util.zip.ZipInputStream

/** 单设备合规报告（文本 + Word）与历史 CSV 生成测试。 */
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
    fun deviceReportContainsClauseAndStats() {
        val reg = regWithOneDevice(version = 0x01) // 旧版本 → STRUCT_VER WARN
        val entry = reg.list.single()
        reg.sampleAll(atMs + 1000)
        reg.sampleAll(atMs + 2000)

        val text = ReportBuilder.buildDevice(entry, nowMs = atMs + 2000)
        assertTrue(text.contains("判定: 警告"))
        assertTrue(text.contains("STRUCT_VER"))
        assertTrue(text.contains("5.2.3 表3 数据格式"))
        assertTrue(text.contains("RSSI:"))
        assertFalse("制表符曲线已移除", text.contains("▁"))
    }

    @Test
    fun deviceReportFallsBackWhenNoSamples() {
        val entry = regWithOneDevice().list.single() // 未采样
        val text = ReportBuilder.buildDevice(entry, nowMs = atMs)
        assertTrue(text.contains("无采样"))
        assertTrue(text.contains("RSSI: -55 dBm"))
    }

    @Test
    fun csvContainsPerFrameData() {
        val reg = regWithOneDevice()
        val entry = reg.list.single()

        val csv = ReportBuilder.buildCsv(entry)

        // 逐帧广播数据块：表头 + 每帧原始字节 + 解码值
        assertTrue(csv.contains("GB 46750-2025 逐帧广播数据（共 1 帧）"))
        assertTrue(csv.contains("帧序号,时间(UTC),RSSI(dBm),原始字节(HEX)"))
        assertTrue(csv.contains("001-唯一产品识别码"))
        assertTrue(csv.contains("CPNYMDL001234567890A")) // UAS_ID 解码值
        val lines = csv.trim().split("\n")
        val dataRow = lines[2] // 0=块标题, 1=列头, 2=首帧数据行
        assertTrue("首行为逐帧数据行: $dataRow", dataRow.startsWith("1,"))
        assertTrue(dataRow.contains("-55"))       // RSSI
        assertTrue(dataRow.contains("FF"))        // 原始字节含 dataType 0xFF
        assertFalse("历史采样块已移除", csv.contains("time_utc,elapsed_s"))
    }

    @Test
    fun deviceReportDocxIsValidZipWithContent() {
        val entry = regWithOneDevice().list.single()
        val bytes = ReportBuilder.buildDeviceDocx(entry, nowMs = atMs)

        // ZIP 魔数 PK
        assertTrue(bytes.size > 4)
        assertEquals('P'.code.toByte(), bytes[0])
        assertEquals('K'.code.toByte(), bytes[1])

        // 含 word/document.xml 且含关键内容
        val zip = ZipInputStream(ByteArrayInputStream(bytes))
        var docXml = ""
        var e = zip.nextEntry
        while (e != null) {
            if (e.name == "word/document.xml") {
                docXml = zip.readBytes().toString(Charsets.UTF_8)
            }
            e = zip.nextEntry
        }
        assertTrue(docXml.contains("RID）检测报告") || docXml.contains("检测报告"))
        assertTrue(docXml.contains("AA:BB:CC:DD:EE:01"))
        assertTrue(docXml.contains("唯一产品识别码"))
        assertTrue(docXml.contains("携带帧数")) // 会话累计字段表
        assertTrue(docXml.contains("总体判定"))
        assertTrue(docXml.contains("CPNYMDL001234567890A"))
    }

    /** XML 1.0 非法控制字符（设备脏字节）必须被剔除，否则 Word/WPS 会拒绝打开 docx。 */
    @Test
    fun deviceReportDocxStripsControlChars() {
        val reg = DeviceRegistry()
        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(realname = "${1.toChar()}${2.toChar()}bad${3.toChar()}"),
                address = "AA:BB:CC:DD:EE:03",
                rssi = -50,
                receivedAtMs = atMs
            ),
            nowMs = atMs
        )
        val bytes = ReportBuilder.buildDeviceDocx(reg.list.single(), nowMs = atMs)

        val zip = ZipInputStream(ByteArrayInputStream(bytes))
        var docXml = ""
        var e = zip.nextEntry
        while (e != null) {
            if (e.name == "word/document.xml") {
                docXml = zip.readBytes().toString(Charsets.UTF_8)
            }
            e = zip.nextEntry
        }
        assertTrue(docXml.contains("bad")) // 正文保留
        assertFalse(docXml.contains(1.toChar()))
        assertFalse(docXml.contains(3.toChar()))
    }

    /** 传入采样曲线位图时，docx 应包含媒体部件 + 关系文件 + r:embed 引用。 */
    @Test
    fun deviceReportDocxEmbedsChartPng() {
        val reg = regWithOneDevice()
        val entry = reg.list.single()
        reg.sampleAll(atMs + 1000)
        val fakePng = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)

        val bytes = ReportBuilder.buildDeviceDocx(entry, nowMs = atMs + 1000, chartPng = fakePng)

        val zip = ZipInputStream(ByteArrayInputStream(bytes))
        val names = mutableListOf<String>()
        var docXml = ""
        var e = zip.nextEntry
        while (e != null) {
            names.add(e.name)
            if (e.name == "word/document.xml") docXml = zip.readBytes().toString(Charsets.UTF_8)
            e = zip.nextEntry
        }
        assertTrue("应含媒体部件", "word/media/image1.png" in names)
        assertTrue("应含文档关系", "word/_rels/document.xml.rels" in names)
        assertTrue(docXml.contains("rIdImg1"))
        assertTrue(docXml.contains("r:embed"))
        assertFalse(docXml.contains("（无采样数据）"))
    }

    /** CSV 公式注入防护：以 = + - @ 开头的非数值解析值须加前导单引号强制按文本读。 */
    @Test
    fun csvGuardsFormulaInjection() {
        val reg = DeviceRegistry()
        reg.onPacket(
            Decoder.decodeGbPacket(
                PacketBuilder.buildPacket(uasId = "=SUM(A1:A2)000000000"),
                address = "AA:BB:CC:DD:EE:02",
                rssi = -50,
                receivedAtMs = atMs
            ),
            nowMs = atMs
        )
        val csv = ReportBuilder.buildCsv(reg.list.single())
        // 逐帧数据行中 UAS_ID 解码值以 = 开头，必须被前导单引号防护强制按文本读
        assertTrue("公式注入应被前导单引号防护", csv.contains("'=SUM(A1:A2)"))
    }
}
