package com.ridcheck

import com.ridcheck.core.Decoder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream

/** 解码器测试。移植自 app/tests/test_decoder.py。 */
class DecoderTest {

    private fun h(s: String): ByteArray =
        s.chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun fullPacketDecodes() {
        val raw = PacketBuilder.buildPacket()
        val pkt = Decoder.decodeGbPacket(raw, address = "AA:BB:CC:DD:EE:FF", rssi = -55)

        assertEquals("", pkt.structureError)
        assertEquals("AA:BB:CC:DD:EE:FF", pkt.address)
        assertEquals(-55, pkt.rssi)
        assertEquals(PacketBuilder.DATA_TYPE, pkt.dataType)
        assertEquals(PacketBuilder.VERSION, pkt.version)
        assertEquals(raw.size - 6, pkt.declaredLen)
        assertEquals(raw.size - 6, pkt.contentLen)

        assertEquals("CPNYMDL001234567890A", pkt.uasId)
        assertEquals("12345678", pkt.realname)
        assertEquals(1, pkt.opCategory)
        assertEquals(1, pkt.uaClass)
        assertEquals(0, pkt.opLocType)
        assertEquals(31.2304, pkt.opLat, 1e-7)
        assertEquals(121.4737, pkt.opLon, 1e-7)
        assertEquals(50.0, pkt.opAlt, 0.1)
        assertEquals(31.2305, pkt.uaLat, 1e-7)
        assertEquals(121.4738, pkt.uaLon, 1e-7)
        assertEquals(45.6, pkt.heading, 0.1)
        assertEquals(3.5, pkt.speed, 0.1)
        assertTrue(pkt.hasRelHeight)
        assertEquals(120.0, pkt.relHeight, 0.1)
        assertTrue(pkt.hasVspeed)
        assertEquals(-1.5, pkt.vspeed, 0.1)
        assertEquals(150.0, pkt.geoAlt, 0.1)
        assertTrue(pkt.hasBaroAlt)
        assertEquals(149.0, pkt.baroAlt, 0.1)
        assertEquals(2, pkt.opStatus)
        assertEquals(0, pkt.coordSys)
        assertEquals(10, pkt.horizAcc)
        assertEquals(5, pkt.vertAcc)
        assertEquals(3, pkt.speedAcc)
        assertEquals(1700000000000L, pkt.timestampMs)
        assertEquals(5, pkt.tsAcc)
    }

    /** 黄金向量：直接从 Python 解码器生成的完整包字节解码。 */
    @Test
    fun fullHexGoldenVector() {
        val raw = Decoder.parseHex(
            "FF2048FFFFFE43504E594D444C303031323334353637383930413132333435363738" +
                "01010080619D12686A6748340868659D12506E6748C8012300404783FC08FA0802000A" +
                "05030068E5CF8B0105"
        )
        val pkt = Decoder.decodeGbPacket(raw)
        assertEquals(72, pkt.declaredLen)
        assertEquals(72, pkt.contentLen)
        assertEquals("", pkt.structureError)
        assertEquals("CPNYMDL001234567890A", pkt.uasId)
        assertEquals("12345678", pkt.realname)
        assertEquals(31.2304, pkt.opLat, 1e-7)
        assertEquals(121.4737, pkt.opLon, 1e-7)
        assertEquals(31.2305, pkt.uaLat, 1e-7)
        assertEquals(121.4738, pkt.uaLon, 1e-7)
        assertEquals(45.6, pkt.heading, 0.1)
        assertEquals(3.5, pkt.speed, 0.1)
        assertEquals(120.0, pkt.relHeight, 0.1)
        assertEquals(-1.5, pkt.vspeed, 0.1)
        assertEquals(150.0, pkt.geoAlt, 0.1)
        assertEquals(149.0, pkt.baroAlt, 0.1)
        assertEquals(2, pkt.opStatus)
        assertEquals(10, pkt.horizAcc)
        assertEquals(1700000000000L, pkt.timestampMs)
    }

    @Test
    fun optionalFieldsAbsent() {
        val pkt = Decoder.decodeGbPacket(
            PacketBuilder.buildPacket(relHeight = null, vspeed = null, baroAlt = null)
        )
        assertEquals("", pkt.structureError)
        assertTrue(!pkt.hasRelHeight)
        assertTrue(!pkt.hasVspeed)
        assertTrue(!pkt.hasBaroAlt)
        assertEquals("未广播", pkt.fmt["相对高度_m"])
        // 内容解析不能错位：后续字段仍正确
        assertEquals(2, pkt.opStatus)
        assertEquals(150.0, pkt.geoAlt, 0.1)
        assertEquals(1700000000000L, pkt.timestampMs)
    }

    @Test
    fun versionWrongIsFlagged() {
        val pkt = Decoder.decodeGbPacket(PacketBuilder.buildPacket(version = 0x01))
        assertTrue(pkt.structureError.contains("版本错误"))
    }

    @Test
    fun truncatedPacketDoesNotCrash() {
        val raw = PacketBuilder.buildPacket()
        val pkt = Decoder.decodeGbPacket(raw.copyOf(20))
        assertTrue(pkt.uasId.isNotEmpty()) // 头部字节足够，部分解析不崩溃
        val pkt2 = Decoder.decodeGbPacket(raw.copyOf(3))
        assertTrue(pkt2.structureError.contains("过短"))
        assertArrayEquals(raw.copyOf(3), pkt2.raw)
    }

    @Test
    fun unknownSentinelsDecodedAsNaN() {
        val pkt = Decoder.decodeGbPacket(PacketBuilder.buildPacket(uaPosUnknown = true))
        assertTrue(pkt.uaLat.isNaN())
        assertTrue(pkt.uaLon.isNaN())
        assertEquals("未知, 未知", pkt.fmt["无人机位置"])
    }

    @Test
    fun parseHexAcceptsCommonFormats() {
        assertArrayEquals(h("FF2040"), Decoder.parseHex("FF 20 40"))
        assertArrayEquals(h("FF2040"), Decoder.parseHex("ff 20,40"))
        assertArrayEquals(h("FF20"), Decoder.parseHex("0xFF 0x20"))
        assertArrayEquals(h(""), Decoder.parseHex(""))
        // nRF Connect 复制的连续无分隔串
        assertArrayEquals(h("FF2040"), Decoder.parseHex("FF2040"))
        assertArrayEquals(h("FF2040"), Decoder.parseHex("ff2040"))
        assertArrayEquals(h("0201060C09"), Decoder.parseHex("0x0201060C09"))
        // 混合分隔 + 连续 token
        assertArrayEquals(h("FF20461B"), Decoder.parseHex("FF20 46,0x1B"))
    }

    @Test
    fun parseHexRejectsBadInput() {
        try {
            Decoder.parseHex("FF2")
            fail("奇长连续串应报错")
        } catch (e: IllegalArgumentException) {
            // expected
        }
        try {
            Decoder.parseHex("FF G0")
            fail("非法字符应报错")
        } catch (e: IllegalArgumentException) {
            // expected
        }
    }

    /** 从完整 BLE 广播帧中提取 GB 包（nRF Connect 粘贴场景）。 */
    @Test
    fun extractGbFromRawFrame() {
        val gb = PacketBuilder.buildPacket()
        // 伪广播帧：AD Flags + Service Data (0x16, UUID 0x0D50 LE + 载荷)
        val ad = ByteArrayOutputStream()
        ad.write(byteArrayOf(0x02, 0x01, 0x06))
        val payload = byteArrayOf(0x50, 0x0D) + gb
        ad.write(byteArrayOf((payload.size + 1).toByte(), 0x16))
        ad.write(payload)
        val frame = ad.toByteArray()

        val extracted = Decoder.extractGbFromAdv(frame)
        assertArrayEquals(gb, extracted)

        // 裸包原样返回
        assertArrayEquals(gb, Decoder.extractGbFromAdv(gb))
    }
}
