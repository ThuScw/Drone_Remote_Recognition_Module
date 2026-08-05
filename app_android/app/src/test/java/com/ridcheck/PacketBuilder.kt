package com.ridcheck

import java.io.ByteArrayOutputStream

/**
 * 合成 GB 46750-2025 数据包构建器。逐行移植自 app/tests/packet_builder.py，
 * 生成与固件编码一致的字节，用于测试 Kotlin 解码/判定移植的正确性。
 * 坐标、高度等一律用 Double，以匹配 Python 的 64 位浮点运算。
 */
object PacketBuilder {
    const val DATA_TYPE = 0xFF
    const val VERSION = 0x20

    // dataId 位掩码
    const val DID_REL_HEIGHT = 0x10
    const val DID_VERT_SPEED = 0x08
    const val DID_BARO_ALT = 0x02

    private fun leInt32(v: Int): ByteArray = byteArrayOf(
        (v and 0xFF).toByte(),
        ((v shr 8) and 0xFF).toByte(),
        ((v shr 16) and 0xFF).toByte(),
        ((v shr 24) and 0xFF).toByte()
    )

    private fun leUint16(v: Int): ByteArray = byteArrayOf(
        (v and 0xFF).toByte(),
        ((v shr 8) and 0xFF).toByte()
    )

    private fun encodeAlt1000(alt: Double): ByteArray =
        if (alt.isNaN()) leUint16(0) else leUint16(Math.round((alt + 1000.0) * 2.0).toInt())

    private fun encodeRelHeight(h: Double): ByteArray =
        leUint16(Math.round((h + 9000.0) * 2.0).toInt())

    private fun encodeHeading(deg: Double): ByteArray =
        leUint16(Math.round(deg * 10.0).toInt())

    private fun encodeSpeed(mps: Double): ByteArray =
        leUint16(Math.round(mps * 10.0).toInt())

    private fun encodeVspeed(mps: Double): ByteArray {
        val dirBit = if (mps < 0) 0x80 else 0
        return byteArrayOf((dirBit or Math.round(Math.abs(mps) * 2.0).toInt()).toByte())
    }

    private fun encodePos(lat: Double, lon: Double): ByteArray =
        leInt32((lat * 1e7).toInt()) + leInt32((lon * 1e7).toInt())

    private fun encodeTs(ms: Long): ByteArray {
        val out = ByteArray(6)
        for (i in 0 until 6) {
            out[i] = ((ms shr (8 * i)) and 0xFF).toByte()
        }
        return out
    }

    /** ASCII 编码并截断/补 0 到 n 字节（对应 Python s.encode()[:n].ljust(n, b"\x00")）。 */
    private fun padAscii(s: String, n: Int): ByteArray {
        val bytes = s.toByteArray(Charsets.US_ASCII)
        return ByteArray(n) { if (it < bytes.size) bytes[it] else 0 }
    }

    fun buildPacket(
        uasId: String = "CPNYMDL001234567890A",
        realname: String = "12345678",
        opCategory: Int = 1,
        uaClass: Int = 1,
        opLocType: Int = 0,
        opLat: Double = 31.2304,
        opLon: Double = 121.4737,
        opAlt: Double = 50.0,
        uaLat: Double = 31.2305,
        uaLon: Double = 121.4738,
        heading: Double = 45.6,
        speed: Double = 3.5,
        relHeight: Double? = 120.0,
        vspeed: Double? = -1.5,
        geoAlt: Double = 150.0,
        baroAlt: Double? = 149.0,
        opStatus: Int = 2,
        coordSys: Int = 0,
        horizAcc: Int = 10,
        vertAcc: Int = 5,
        speedAcc: Int = 3,
        timestampMs: Long = 1700000000000L,
        tsAcc: Int = 5,
        version: Int = VERSION,
        dataType: Int = DATA_TYPE,
        uaPosUnknown: Boolean = false
    ): ByteArray {
        val c = ByteArrayOutputStream()
        c.write(padAscii(uasId, 20))
        c.write(padAscii(realname, 8))
        c.write(byteArrayOf(opCategory.toByte(), uaClass.toByte(), opLocType.toByte()))
        c.write(encodePos(opLat, opLon))
        c.write(encodeAlt1000(opAlt))
        if (uaPosUnknown) {
            c.write(leInt32(-1))
            c.write(leInt32(-1))
        } else {
            c.write(encodePos(uaLat, uaLon))
        }
        c.write(encodeHeading(heading))
        c.write(encodeSpeed(speed))
        if (relHeight != null) c.write(encodeRelHeight(relHeight))
        if (vspeed != null) c.write(encodeVspeed(vspeed))
        c.write(encodeAlt1000(geoAlt))
        if (baroAlt != null) c.write(encodeAlt1000(baroAlt))
        c.write(byteArrayOf(
            opStatus.toByte(), coordSys.toByte(),
            horizAcc.toByte(), vertAcc.toByte(), speedAcc.toByte()
        ))
        c.write(encodeTs(timestampMs))
        c.write(byteArrayOf(tsAcc.toByte()))

        var dataId1 = 0xE5 // M 位（位置/航迹/地速/大地高度/扩展）
        if (relHeight != null) dataId1 = dataId1 or DID_REL_HEIGHT
        if (vspeed != null) dataId1 = dataId1 or DID_VERT_SPEED
        if (baroAlt != null) dataId1 = dataId1 or DID_BARO_ALT

        val body = ByteArrayOutputStream()
        body.write(byteArrayOf(dataType.toByte(), version.toByte(), c.size().toByte()))
        body.write(byteArrayOf(0xFF.toByte(), dataId1.toByte(), 0xFE.toByte()))
        c.writeTo(body)
        return body.toByteArray()
    }
}
