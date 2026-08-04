package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/** 表3 21 项数据的字段语义分类（决定「会话统计」列的格式化方式）。 */
enum class FieldKind { IDENTITY, ENUM, NUMERIC, ANGLE, COORD, TS }

/** 运行类别 / 无人机分类 / 遥控站位置类型 / 坐标系 的标签（与固件 config.h 注释一致）。 */
val OP_CATEGORY_LABELS = mapOf(0 to "未定义", 1 to "开放类", 2 to "特定类", 3 to "审定类")
val UA_CLASS_LABELS = mapOf(0 to "微型", 1 to "轻型", 2 to "小型", 3 to "中型", 4 to "大型")
val OP_LOC_TYPE_LABELS = mapOf(0 to "起飞点", 1 to "遥控站实时位置")
val COORD_SYS_LABELS = mapOf(0 to "WGS-84", 1 to "CGCS2000")

/**
 * 单个字段的会话累计器。按字段语义填充相应槽位：
 * 身份(001/002) / 枚举(003-005,015-019,021) / 数值(007,010-014) / 角度(009) / 坐标(006,008) / 时间戳(020)。
 */
class FieldAccum(val kind: FieldKind) {
    var seen: Int = 0          // 携带该字段的帧数
    var unknown: Int = 0       // 携带但值为「未知/哨兵/无效」的帧数
    var firstMs: Long = 0      // 首帧接收时间
    var lastMs: Long = 0

    // 数值槽（NUMERIC / ANGLE）
    var hasVal = false
    var minV = Double.NaN
    var maxV = Double.NaN
    var sumV = 0.0
    var nV = 0

    // 枚举计数（ENUM）：值 → 次数
    val enumCounts = LinkedHashMap<Int, Int>()

    // 身份槽（IDENTITY）：不同取值集合
    val distinct = LinkedHashSet<String>()
    var firstStr: String? = null

    // 坐标槽（COORD）：lat/lon 范围 + 相对首点的最远距离（米）
    var latMin = Double.NaN
    var latMax = Double.NaN
    var lonMin = Double.NaN
    var lonMax = Double.NaN
    var refLat = Double.NaN
    var refLon = Double.NaN
    var maxDistM = 0.0

    // 时间戳槽（TS）：设备时间戳范围 + 未授时计数
    var tsFirst: Long = 0
    var tsLast: Long = 0
    var tsZero = 0

    fun countEnum(v: Int) {
        enumCounts[v] = (enumCounts[v] ?: 0) + 1
    }

    fun countValue(v: Double) {
        if (hasVal) {
            minV = minOf(minV, v)
            maxV = maxOf(maxV, v)
        } else {
            minV = v
            maxV = v
            hasVal = true
        }
        sumV += v
        nV++
    }

    fun countIdentity(v: String) {
        if (firstStr == null) firstStr = v
        distinct.add(v)
    }
}

/**
 * 表3 全部 21 项数据的会话累计统计（身份恒定/枚举占比/数值范围/未知占比/坐标跨度/状态时长/授时范围）。
 * 由 DeviceEntry.onPacket 每次喂入解码包；「会话统计」列文本见 [fieldSummary]。
 */
class SessionStats {
    val accums = Array(22) { FieldAccum(fieldKindFor(it)) }

    /** 收录一个解码包的全部 21 项字段。 */
    fun record(pkt: DecodedPacket) {
        val now = pkt.receivedAtMs

        fun touch(a: FieldAccum) {
            if (a.seen == 0) a.firstMs = now
            a.lastMs = now
        }

        // 001 唯一产品识别码 (M)
        accums[1].let { a ->
            touch(a)
            a.seen++
            if (pkt.uasId.isEmpty()) a.unknown++ else a.countIdentity(pkt.uasId)
        }
        // 002 实名登记号 (M)
        accums[2].let { a ->
            touch(a)
            a.seen++
            if (pkt.realname.isEmpty() || pkt.realname == "00000000") {
                a.unknown++
                a.countIdentity(pkt.realname.ifEmpty { "(空)" })
            } else {
                a.countIdentity(pkt.realname)
            }
        }
        // 003 运行类别 / 004 无人机分类 / 005 遥控站位置类型 (M 单字节枚举)
        recordEnum(3, pkt.opCategory)
        recordEnum(4, pkt.uaClass)
        recordEnum(5, pkt.opLocType)
        // 006 遥控站位置 / 007 遥控站高度
        recordCoord(6, pkt.opLat, pkt.opLon)
        recordValue(7, pkt.opAlt)
        // 008 无人机位置
        recordCoord(8, pkt.uaLat, pkt.uaLon)
        // 009 航迹角 / 010 地速
        recordValue(9, pkt.heading)
        recordValue(10, pkt.speed)
        // 011 相对高度 (O) / 012 垂直速度 (O) / 014 气压高度 (O)：未携带则不计数
        if (pkt.hasRelHeight) recordValue(11, pkt.relHeight)
        if (pkt.hasVspeed) recordValue(12, pkt.vspeed)
        recordValue(13, pkt.geoAlt)
        if (pkt.hasBaroAlt) recordValue(14, pkt.baroAlt)
        // 015 运行状态 / 016 坐标系
        recordEnum(15, pkt.opStatus)
        recordEnum(16, pkt.coordSys)
        // 017-019 精度 / 021 时间戳精度：值 0 =「未知/超最大档」
        recordAccuracy(17, pkt.horizAcc)
        recordAccuracy(18, pkt.vertAcc)
        recordAccuracy(19, pkt.speedAcc)
        // 020 时间戳
        accums[20].let { a ->
            touch(a)
            a.seen++
            if (pkt.timestampMs <= 0) {
                a.tsZero++
                a.unknown++
            } else {
                if (a.tsFirst == 0L) a.tsFirst = pkt.timestampMs
                a.tsLast = pkt.timestampMs
            }
        }
        recordAccuracy(21, pkt.tsAcc)
    }

    /**
     * 第 num 项字段的「会话统计」文本。statusTimeline 为 015 专用的状态时长串
     * （由 ReportBuilder 依据 DeviceEntry.statusLog 计算），其余字段自足。
     */
    fun fieldSummary(num: Int, statusTimeline: String? = null): String {
        val a = accums[num]
        if (a.seen == 0) return if (num == 11 || num == 12 || num == 14) "未上报" else "-"
        return when (num) {
            1 -> identitySummary(a)
            2 -> realnameSummary(a)
            3 -> enumPercent(a, OP_CATEGORY_LABELS)
            4 -> enumPercent(a, UA_CLASS_LABELS)
            5 -> opLocTypeSummary(a)
            6 -> opPosSummary(a)
            7 -> numericRange(a, "m")
            8 -> uaPosSummary(a)
            9 -> headingSummary(a)
            10 -> numericRangeMean(a, "m/s")
            11 -> optionalRange(a, "m")
            12 -> optionalRange(a, "m/s")
            13 -> numericRangeMean(a, "m")
            14 -> optionalRange(a, "m")
            15 -> statusSummary(a, statusTimeline)
            16 -> enumPercent(a, COORD_SYS_LABELS)
            17 -> accuracySummary(a, Decoder.HORIZ_ACC)
            18 -> accuracySummary(a, Decoder.VERT_ACC)
            19 -> accuracySummary(a, Decoder.SPEED_ACC)
            20 -> tsSummary(a)
            21 -> accuracySummary(a, Decoder.TS_ACC)
            else -> "-"
        }
    }

    // ---------- 记录 ----------

    private fun recordEnum(num: Int, v: Int) {
        val a = accums[num]
        a.seen++
        if (v < 0) a.unknown++ else a.countEnum(v)
    }

    private fun recordValue(num: Int, v: Double) {
        val a = accums[num]
        a.seen++
        if (v.isNaN()) a.unknown++ else a.countValue(v)
    }

    /** 精度字段：值 0 表示「未知 / 超最大档」，计入 unknown 而非有效分布。 */
    private fun recordAccuracy(num: Int, v: Int) {
        val a = accums[num]
        a.seen++
        if (v <= 0) a.unknown++ else a.countEnum(v)
    }

    private fun recordCoord(num: Int, lat: Double, lon: Double) {
        val a = accums[num]
        a.seen++
        if (lat.isNaN() || lon.isNaN()) {
            a.unknown++
            return
        }
        if (a.hasVal) {
            a.latMin = minOf(a.latMin, lat)
            a.latMax = maxOf(a.latMax, lat)
            a.lonMin = minOf(a.lonMin, lon)
            a.lonMax = maxOf(a.lonMax, lon)
        } else {
            a.latMin = lat; a.latMax = lat
            a.lonMin = lon; a.lonMax = lon
            a.refLat = lat; a.refLon = lon
            a.hasVal = true
        }
        a.maxDistM = maxOf(a.maxDistM, Geo.distMeters(a.refLat, a.refLon, lat, lon))
    }

    // ---------- 格式化 ----------

    private fun identitySummary(a: FieldAccum): String {
        val first = a.firstStr
            ?: return "无有效值" + if (a.unknown > 0) "（${a.unknown} 次）" else ""
        return if (a.distinct.size == 1) "恒定：$first"
        else "变化！${a.distinct.size} 个不同值：${a.distinct.joinToString("/")}"
    }

    private fun realnameSummary(a: FieldAccum): String {
        val first = a.firstStr
        if (first == null || first == "00000000" || first == "(空)") {
            return "默认值/未实名" + if (a.unknown > 0) "（${a.unknown} 次）" else ""
        }
        return if (a.distinct.size == 1) "恒定：$first"
        else "变化！${a.distinct.size} 个不同值：${a.distinct.joinToString("/")}"
    }

    private fun enumPercent(a: FieldAccum, labels: Map<Int, String>): String {
        val total = a.enumCounts.values.sum()
        if (total == 0) return "无有效值"
        val parts = ArrayList<String>()
        for ((v, c) in a.enumCounts) {
            val pct = Math.round(c * 100.0 / total)
            parts.add("${labels[v] ?: "未定义($v)"} $pct%")
        }
        if (a.unknown > 0) parts.add("未知 ${a.unknown} 次")
        return parts.joinToString("；")
    }

    private fun opLocTypeSummary(a: FieldAccum): String {
        val total = a.enumCounts.values.sum()
        if (total == 0) return "无有效值"
        val parts = ArrayList<String>()
        for ((v, c) in a.enumCounts) {
            val pct = Math.round(c * 100.0 / total)
            parts.add("${OP_LOC_TYPE_LABELS[v] ?: "未定义($v)"} $pct%")
        }
        if (a.unknown > 0) parts.add("未知 ${a.unknown} 次")
        return parts.joinToString("；")
    }

    private fun opPosSummary(a: FieldAccum): String {
        if (!a.hasVal) return "未知" + if (a.unknown > 0) "（${a.unknown} 次）" else ""
        val lat = fmtRange(a.latMin, a.latMax, 7)
        val lon = fmtRange(a.lonMin, a.lonMax, 7)
        val alwaysTakeoff = accums[5].enumCounts.size == 1 && accums[5].enumCounts.containsKey(0)
        return "lat $lat / lon $lon" + if (alwaysTakeoff) "（起飞点，恒定）" else ""
    }

    private fun uaPosSummary(a: FieldAccum): String {
        if (!a.hasVal) return "未知" + if (a.unknown > 0) "（${a.unknown} 次）" else ""
        val lat = fmtRange(a.latMin, a.latMax, 7)
        val lon = fmtRange(a.lonMin, a.lonMax, 7)
        return String.format(Locale.US, "lat %s / lon %s，跨度 %.1f m", lat, lon, a.maxDistM)
    }

    private fun numericRange(a: FieldAccum, unit: String): String {
        if (!a.hasVal) return "无有效值"
        return if (a.minV == a.maxV) String.format(Locale.US, "%.1f %s（恒定）", a.minV, unit)
        else String.format(Locale.US, "%.1f~%.1f %s", a.minV, a.maxV, unit)
    }

    private fun numericRangeMean(a: FieldAccum, unit: String): String {
        if (!a.hasVal) return "无有效值"
        val mean = a.sumV / a.nV
        return if (a.minV == a.maxV) String.format(Locale.US, "%.1f %s，均值 %.1f", a.minV, unit, mean)
        else String.format(Locale.US, "%.1f~%.1f %s，均值 %.1f", a.minV, a.maxV, unit, mean)
    }

    private fun headingSummary(a: FieldAccum): String {
        if (!a.hasVal) return "无有效值"
        val body = if (a.minV == a.maxV) String.format(Locale.US, "%.1f°", a.minV)
        else String.format(Locale.US, "%.1f°~%.1f°", a.minV, a.maxV)
        val still = a.maxV < 0.5 // 全程近似静止
        return body + if (still) "（静止）" else ""
    }

    private fun optionalRange(a: FieldAccum, unit: String): String {
        if (a.seen == 0) return "未上报"
        if (!a.hasVal) return "未知" + if (a.unknown > 0) "（${a.unknown} 次）" else ""
        return numericRange(a, unit)
    }

    private fun statusSummary(a: FieldAccum, statusTimeline: String?): String {
        if (statusTimeline != null && statusTimeline.isNotEmpty()) return statusTimeline
        return enumPercent(a, Decoder.OP_STATUS)
    }

    private fun accuracySummary(a: FieldAccum, labels: Map<Int, String>): String {
        if (a.seen == 0) return "-"
        val unknownPct = Math.round(a.unknown * 100.0 / a.seen)
        val sb = StringBuilder()
        if (a.unknown > 0) sb.append("未知 ${a.unknown}/${a.seen} 次($unknownPct%)")
        if (a.enumCounts.isNotEmpty()) {
            val valid = a.enumCounts.entries.joinToString("；") { (v, c) ->
                "${labels[v] ?: "无效($v)"} $c 次"
            }
            if (a.unknown > 0) sb.append("；")
            sb.append("有效时 $valid")
        }
        return if (sb.isEmpty()) "-" else sb.toString()
    }

    private fun tsSummary(a: FieldAccum): String {
        if (a.seen == 0) return "-"
        if (a.tsZero >= a.seen) return "未授时（${a.tsZero} 次）"
        val f1 = timeHms(a.tsFirst)
        val f2 = timeHms(a.tsLast)
        val span = formatDuration(a.tsLast - a.tsFirst)
        val base = "$f1~$f2（跨度 $span，已授时，设备 UTC）"
        return if (a.tsZero > 0) "$base；${a.tsZero} 次未授时" else base
    }

    private fun fmtRange(min: Double, max: Double, dec: Int): String {
        val f = String.format(Locale.US, "%%.%df", dec)
        return if (min == max) String.format(Locale.US, f, min)
        else String.format(Locale.US, f, min) + "~" + String.format(Locale.US, f, max)
    }

    private val tsHms = DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneOffset.UTC)

    private fun timeHms(ms: Long): String = try {
        tsHms.format(Instant.ofEpochMilli(ms))
    } catch (e: Exception) {
        "无效"
    }

    private companion object {
        fun fieldKindFor(num: Int): FieldKind = when (num) {
            1, 2 -> FieldKind.IDENTITY
            3, 4, 5, 15, 16, 17, 18, 19, 21 -> FieldKind.ENUM
            6, 8 -> FieldKind.COORD
            9 -> FieldKind.ANGLE
            20 -> FieldKind.TS
            else -> FieldKind.NUMERIC
        }
    }
}

/** 毫秒时长 → mm:ss（≥1 小时时 h:mm:ss）。报告的状态时长与问题持续共用。 */
fun formatDuration(ms: Long): String {
    val sec = maxOf(0L, ms / 1000)
    val h = sec / 3600
    val m = (sec % 3600) / 60
    val s = sec % 60
    return if (h > 0) String.format(Locale.US, "%d:%02d:%02d", h, m, s)
    else String.format(Locale.US, "%d:%02d", m, s)
}
