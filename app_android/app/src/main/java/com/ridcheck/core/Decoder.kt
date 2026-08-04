package com.ridcheck.core

import java.util.Locale

/**
 * GB 46750-2025 数据包解码器。逐行移植自 app/rid/decoder.py，
 * 行为与 PC 版完全一致：防御式解析，结构错误记录在 structureError 而不抛异常。
 */
object Decoder {
    const val GB46750_DATA_TYPE = 0xFF
    const val GB46750_VERSION = 0x20 // V1.0

    // dataId 位掩码（Byte 1）—— 可选字段存在标志
    const val DID_REL_HEIGHT = 0x10 // 011 相对高度
    const val DID_VERT_SPEED = 0x08 // 012 垂直速度
    const val DID_BARO_ALT = 0x02 // 014 气压高度

    // 编码器写入的哨兵值（表示"未知/不可用"）
    const val SENT_SPEED_HEADING = 0xFFFF // speed / heading
    const val SENT_VSPEED = 0xFF // vertical speed

    val OP_STATUS = mapOf(
        0 to "未报告",
        1 to "地面",
        2 to "空中",
        3 to "紧急状态",
        4 to "识别发送功能失效(非紧急)",
        5 to "识别发送功能失效(紧急)"
    )
    val HORIZ_ACC = mapOf(
        0 to ">=18.52km / 未知", 1 to "<18.52km", 2 to "<7.41km", 3 to "<3.70km",
        4 to "<1852m", 5 to "<926m", 6 to "<556m", 7 to "<185m", 8 to "<92.6m",
        9 to "<30m", 10 to "<10m", 11 to "<3m", 12 to "<1m"
    )
    val VERT_ACC = mapOf(
        0 to ">=150m / 未知", 1 to "<150m", 2 to "<45m", 3 to "<25m", 4 to "<10m", 5 to "<3m", 6 to "<1m"
    )
    val SPEED_ACC = mapOf(
        0 to ">=10m/s / 未知", 1 to "<10m/s", 2 to "<3m/s", 3 to "<1m/s", 4 to "<0.3m/s"
    )
    val TS_ACC = mapOf(
        0 to ">0.5s / 未知", 1 to "<=0.5s", 2 to "<=0.4s", 3 to "<=0.3s", 4 to "<=0.2s",
        5 to "<=0.1s", 6 to "<=50ms", 7 to "<=20ms", 8 to "<=10ms"
    )

    fun fmtCoord(v: Double): String =
        if (v.isNaN() || Math.abs(v) > 360) "未知" else String.format(Locale.US, "%.7f", v)

    fun fmtAlt(v: Double): String =
        if (v.isNaN()) "未知" else String.format(Locale.US, "%.1f", v)

    fun fmtSpeed(v: Double): String =
        if (v.isNaN()) "未知" else String.format(Locale.US, "%.1f", v)

    /** 去除尾部 NUL 字节后按 ASCII 解码（对应 Python rstrip(b"\x00").decode）。 */
    private fun asciiNoNull(bytes: ByteArray): String {
        var end = bytes.size
        while (end > 0 && (bytes[end - 1].toInt() and 0xFF) == 0) end--
        return String(bytes.copyOfRange(0, end), Charsets.US_ASCII)
    }

    /**
     * 把空格/逗号/冒号分隔的 hex 文本解析为字节。
     * 兼容 nRF Connect 复制的连续无分隔 hex 串。
     */
    fun parseHex(text: String): ByteArray {
        var cleaned = text.replace("0x", "").replace("0X", "")
        cleaned = cleaned.replace(",", " ").replace(":", " ")
        // 与 Python str.split() 一致：按任意连续空白切分（含制表符/换行）
        val parts = cleaned.split(Regex("\\s+")).filter { it.isNotEmpty() }
        if (parts.isEmpty()) return ByteArray(0)

        val out = ArrayList<Byte>(parts.size)
        for (p in parts) {
            if (p.length > 2 && p.length % 2 == 0) {
                // 连续 hex 串 —— 按 2 字符切分成字节
                if (!p.all { it in "0123456789abcdefABCDEF" }) {
                    throw IllegalArgumentException("invalid hex token: '$p'")
                }
                var i = 0
                while (i < p.length) {
                    out.add(p.substring(i, i + 2).toInt(16).toByte())
                    i += 2
                }
            } else {
                val v = p.toInt(16)
                if (v > 0xFF) throw IllegalArgumentException("byte out of range: '$p'")
                out.add(v.toByte())
            }
        }
        return out.toByteArray()
    }

    /**
     * 解析原始广播字节中的 AD type 0x16（16-bit UUID 的 Service Data）。
     */
    fun parseAdServiceData(raw: ByteArray): Map<Int, ByteArray> {
        val out = LinkedHashMap<Int, ByteArray>()
        var i = 0
        val n = raw.size
        while (i < n) {
            val length = raw[i].toInt() and 0xFF
            if (length == 0 || i + 1 + length > n) break
            val typ = raw[i + 1].toInt() and 0xFF
            val data = raw.copyOfRange(i + 2, i + 1 + length)
            if (typ == 0x16 && data.size >= 2) {
                val uuid16 = ((data[1].toInt() and 0xFF) shl 8) or (data[0].toInt() and 0xFF)
                val existing = out[uuid16] ?: ByteArray(0)
                out[uuid16] = existing + data.copyOfRange(2, data.size)
            }
            i += 1 + length
        }
        return out
    }

    /**
     * 把粘贴的字节规范化为原始 GB 数据包：
     * - 以 0xFF 开头的裸数据包直接返回
     * - 否则当作完整 BLE 广播帧，从 Service Data AD 中抽取
     */
    fun extractGbFromAdv(raw: ByteArray): ByteArray {
        if (raw.isEmpty() || (raw[0].toInt() and 0xFF) == 0xFF) return raw
        for (payload in parseAdServiceData(raw).values) {
            if (payload.isNotEmpty() && (payload[0].toInt() and 0xFF) == 0xFF) return payload
        }
        return raw
    }

    /** 解码一个 GB 46750 数据包。 */
    fun decodeGbPacket(
        data: ByteArray,
        address: String = "",
        rssi: Int = 0,
        receivedAtMs: Long = 0,
        source: String = "ble"
    ): DecodedPacket {
        val pkt = DecodedPacket()
        pkt.raw = data.copyOf()
        pkt.address = address
        pkt.rssi = rssi
        pkt.receivedAtMs = receivedAtMs
        pkt.source = source

        if (data.size < 6) {
            pkt.structureError = "数据包过短 (${data.size}B < 6B 头部)"
            return pkt
        }

        pkt.dataType = data[0].toInt() and 0xFF
        pkt.version = data[1].toInt() and 0xFF
        pkt.declaredLen = data[2].toInt() and 0xFF
        pkt.dataId = data.copyOfRange(3, 6)
        val content = data.copyOfRange(6, data.size)
        pkt.contentLen = content.size

        // 版本不匹配不再计入结构错误：非 0x20 按兼容警告处理（Health 判 WARN），
        // 否则 structureError 会触发 Health 的 STRUCT_LEN(FAIL)，版本降级就失效了。
        // 注意：PC 版 decoder.py 仍把版本写入 structure_error，这里是 Android 版的有意偏离。
        when {
            pkt.dataType != GB46750_DATA_TYPE ->
                pkt.structureError = String.format("dataType 错误: 0x%02X (应为 0xFF)", pkt.dataType)
            pkt.declaredLen != pkt.contentLen ->
                pkt.structureError =
                    "dataLength 不匹配: 声明 ${pkt.declaredLen}B, 实际 ${pkt.contentLen}B"
        }

        // 即便结构出错，也尽量解析内容以便展示。
        parseContent(pkt, content, if (pkt.dataId.size >= 2) (pkt.dataId[1].toInt() and 0xFF) else 0)
        return pkt
    }

    /** 按固件字段顺序解析内容字节（见 rid_messages.cpp）。 */
    fun parseContent(pkt: DecodedPacket, c: ByteArray, dataId1: Int) {
        var pos = 0
        val n = c.size

        fun take(size: Int): ByteArray {
            var end = pos + size
            if (end > n) end = n
            val chunk = c.copyOfRange(pos, end)
            pos = end
            return chunk
        }

        fun need(size: Int): Boolean = pos + size <= n

        /** 记录 [from, pos) 这段内容字节的 HEX 到 fieldHex[表3序号]。 */
        fun recordHex(num: String, from: Int) {
            if (from in 0..pos && from < pos) {
                pkt.fieldHex[num] = c.copyOfRange(from, pos).joinToString(" ") {
                    String.format("%02X", it.toInt() and 0xFF)
                }
            }
        }

        // 001 唯一产品识别码 (20 ASCII)
        var f0 = pos
        pkt.uasId = asciiNoNull(take(20))
        recordHex("001", f0)
        // 002 实名登记标志 (8 ASCII)
        f0 = pos
        pkt.realname = asciiNoNull(take(8))
        recordHex("002", f0)

        // 003-005 单字节枚举
        f0 = pos
        if (need(1)) { pkt.opCategory = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("003", f0)
        f0 = pos
        if (need(1)) { pkt.uaClass = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("004", f0)
        f0 = pos
        if (need(1)) { pkt.opLocType = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("005", f0)

        // 006 遥控站位置 (int32 LE x2, deg*1e7)
        f0 = pos
        if (need(8)) {
            val latI = leInt32(c, pos)
            val lonI = leInt32(c, pos + 4)
            pos += 8
            if (latI == -1 || lonI == -1) {
                pkt.opLat = Double.NaN
                pkt.opLon = Double.NaN
            } else {
                pkt.opLat = latI / 1e7
                pkt.opLon = lonI / 1e7
            }
        }
        recordHex("006", f0)

        // 007 遥控站高度 (uint16 LE, (val+1000)*2)
        f0 = pos
        if (need(2)) {
            val v = leUint16(c, pos)
            pos += 2
            pkt.opAlt = if (v != 0) v / 2.0 - 1000.0 else Double.NaN
        }
        recordHex("007", f0)

        // 008 无人机位置
        f0 = pos
        if (need(8)) {
            val latI = leInt32(c, pos)
            val lonI = leInt32(c, pos + 4)
            pos += 8
            if (latI == -1 || lonI == -1) {
                pkt.uaLat = Double.NaN
                pkt.uaLon = Double.NaN
            } else {
                pkt.uaLat = latI / 1e7
                pkt.uaLon = lonI / 1e7
            }
        }
        recordHex("008", f0)

        // 009 航迹角 (uint16 LE, *0.1 deg), 010 地速 (uint16 LE, *0.1 m/s)
        f0 = pos
        if (need(2)) {
            val v = leUint16(c, pos)
            pos += 2
            pkt.heading = if (v != SENT_SPEED_HEADING) v / 10.0 else Double.NaN
        }
        recordHex("009", f0)
        f0 = pos
        if (need(2)) {
            val v = leUint16(c, pos)
            pos += 2
            pkt.speed = if (v != SENT_SPEED_HEADING) v / 10.0 else Double.NaN
        }
        recordHex("010", f0)

        // 011 相对高度 (O, uint16 LE, (val+9000)*2)
        if (dataId1 and DID_REL_HEIGHT != 0) {
            f0 = pos
            if (need(2)) {
                val v = leUint16(c, pos)
                pos += 2
                pkt.relHeight = v / 2.0 - 9000.0
                pkt.hasRelHeight = true
            }
            recordHex("011", f0)
        }

        // 012 垂直速度 (O, 1 byte, bit7=dir, bits6-0=*0.5 m/s)
        if (dataId1 and DID_VERT_SPEED != 0) {
            f0 = pos
            if (need(1)) {
                val b = c[pos].toInt() and 0xFF
                pos += 1
                pkt.hasVspeed = true
                if (b == SENT_VSPEED) {
                    pkt.vspeed = Double.NaN
                } else {
                    val value = b and 0x7F
                    pkt.vspeed = if ((b and 0x80) != 0) -value / 2.0 else value / 2.0
                }
            }
            recordHex("012", f0)
        }

        // 013 大地高度 (uint16 LE, (val+1000)*2)
        f0 = pos
        if (need(2)) {
            val v = leUint16(c, pos)
            pos += 2
            pkt.geoAlt = if (v != 0) v / 2.0 - 1000.0 else Double.NaN
        }
        recordHex("013", f0)

        // 014 气压高度 (O, uint16 LE, (val+1000)*2)
        if (dataId1 and DID_BARO_ALT != 0) {
            f0 = pos
            if (need(2)) {
                val v = leUint16(c, pos)
                pos += 2
                pkt.baroAlt = if (v != 0) v / 2.0 - 1000.0 else Double.NaN
                pkt.hasBaroAlt = true
            }
            recordHex("014", f0)
        }

        // 015 运行状态, 016 坐标系
        f0 = pos
        if (need(1)) { pkt.opStatus = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("015", f0)
        f0 = pos
        if (need(1)) { pkt.coordSys = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("016", f0)

        // 017-019 精度
        f0 = pos
        if (need(1)) { pkt.horizAcc = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("017", f0)
        f0 = pos
        if (need(1)) { pkt.vertAcc = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("018", f0)
        f0 = pos
        if (need(1)) { pkt.speedAcc = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("019", f0)

        // 020 时间戳 (uint48 LE, ms)
        f0 = pos
        if (need(6)) {
            pkt.timestampMs = leUint48(c, pos)
            pos += 6
        }
        recordHex("020", f0)

        // 021 时间戳精度
        f0 = pos
        if (need(1)) { pkt.tsAcc = c[pos].toInt() and 0xFF; pos += 1 }
        recordHex("021", f0)

        pkt.contentLen = pos

        // --- 人类可读摘要 ---
        pkt.fmt["唯一产品识别码"] = pkt.uasId.ifEmpty { "(空)" }
        pkt.fmt["实名登记号"] = pkt.realname.ifEmpty { "(空)" }
        pkt.fmt["运行类别"] = if (pkt.opCategory >= 0) pkt.opCategory.toString() else "-"
        pkt.fmt["无人机分类"] = if (pkt.uaClass >= 0) pkt.uaClass.toString() else "-"
        pkt.fmt["遥控站位置类型"] = if (pkt.opLocType >= 0) pkt.opLocType.toString() else "-"
        pkt.fmt["遥控站位置"] = "${fmtCoord(pkt.opLat)}, ${fmtCoord(pkt.opLon)}"
        pkt.fmt["遥控站高度_m"] = fmtAlt(pkt.opAlt)
        pkt.fmt["无人机位置"] = "${fmtCoord(pkt.uaLat)}, ${fmtCoord(pkt.uaLon)}"
        pkt.fmt["航迹角_deg"] = fmtSpeed(pkt.heading)
        pkt.fmt["地速_mps"] = fmtSpeed(pkt.speed)
        pkt.fmt["相对高度_m"] = if (pkt.hasRelHeight) fmtAlt(pkt.relHeight) else "未广播"
        pkt.fmt["垂直速度_mps"] = if (pkt.hasVspeed) fmtSpeed(pkt.vspeed) else "未广播"
        pkt.fmt["大地高度_m"] = fmtAlt(pkt.geoAlt)
        pkt.fmt["气压高度_m"] = if (pkt.hasBaroAlt) fmtAlt(pkt.baroAlt) else "未广播"
        pkt.fmt["运行状态"] =
            if (pkt.opStatus >= 0) OP_STATUS[pkt.opStatus] ?: "无效(${pkt.opStatus})" else "-"
        pkt.fmt["坐标系"] =
            if (pkt.coordSys >= 0)
                when (pkt.coordSys) { 0 -> "WGS-84"; 1 -> "CGCS2000"; else -> "无效(${pkt.coordSys})" }
            else "-"
        pkt.fmt["水平精度"] =
            if (pkt.horizAcc >= 0) HORIZ_ACC[pkt.horizAcc] ?: "无效(${pkt.horizAcc})" else "-"
        pkt.fmt["垂直精度"] =
            if (pkt.vertAcc >= 0) VERT_ACC[pkt.vertAcc] ?: "无效(${pkt.vertAcc})" else "-"
        pkt.fmt["速度精度"] =
            if (pkt.speedAcc >= 0) SPEED_ACC[pkt.speedAcc] ?: "无效(${pkt.speedAcc})" else "-"
        pkt.fmt["时间戳"] = pkt.timestampUtc
        pkt.fmt["时间戳精度"] =
            if (pkt.tsAcc >= 0) TS_ACC[pkt.tsAcc] ?: "无效(${pkt.tsAcc})" else "-"
    }

    private fun leUint16(c: ByteArray, pos: Int): Int =
        (c[pos].toInt() and 0xFF) or ((c[pos + 1].toInt() and 0xFF) shl 8)

    private fun leInt32(c: ByteArray, pos: Int): Int =
        (c[pos].toInt() and 0xFF) or
            ((c[pos + 1].toInt() and 0xFF) shl 8) or
            ((c[pos + 2].toInt() and 0xFF) shl 16) or
            ((c[pos + 3].toInt() and 0xFF) shl 24)

    private fun leUint48(c: ByteArray, pos: Int): Long {
        var v = 0L
        for (i in 0 until 6) {
            v = v or (((c[pos + i].toLong()) and 0xFFL) shl (8 * i))
        }
        return v
    }
}
