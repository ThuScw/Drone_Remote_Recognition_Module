package com.ridcheck.core

import java.util.Locale

/**
 * 健康判定。逐行移植自 app/rid/health.py：
 * - assessPacket：单包判定
 * - StreamAssessor：10s 窗口的速率/停滞/冻结判定
 */
object Health {
    val UAS_ID_CHARSET = Regex("^[0-9A-Z]+$")
    val UAS_ID_FORBIDDEN = setOf('O', 'I')

    const val GB_MIN_RATE_HZ = 1.0 // GB 46750-2025 5.1.3: 广播间隔 ≤ 1s
    const val FRESH_THRESHOLD_S = 2.0 // DATA_FRESH_THRESHOLD_MS = 2000

    /** 判定单个解码包，返回问题列表（空 = 健康）。 */
    fun assessPacket(pkt: DecodedPacket): List<HealthIssue> {
        val issues = ArrayList<HealthIssue>()

        // --- 结构 ---
        if (pkt.dataType != Decoder.GB46750_DATA_TYPE) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STRUCT_TYPE",
                String.format("dataType=0x%02X，应为 0xFF", pkt.dataType)))
        }
        if (pkt.version != Decoder.GB46750_VERSION) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STRUCT_VER",
                String.format("版本=0x%02X，应为 0x20 (V1.0)", pkt.version)))
        }
        if (pkt.structureError.isNotEmpty()) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STRUCT_LEN", pkt.structureError))
        }

        // --- UAS_ID (001, M) ---
        val uas = pkt.uasId
        when {
            uas.isEmpty() ->
                issues.add(HealthIssue(HealthLevel.FAIL, "UAS_EMPTY", "唯一产品识别码为空"))
            uas.length != 20 ->
                issues.add(HealthIssue(HealthLevel.WARN, "UAS_LEN",
                    "唯一产品识别码长度 ${uas.length}，应为 20"))
            else -> {
                if (!UAS_ID_CHARSET.matches(uas)) {
                    issues.add(HealthIssue(HealthLevel.WARN, "UAS_CHARSET",
                        "唯一产品识别码含非 0-9/A-Z 字符"))
                }
                if (uas.any { it in UAS_ID_FORBIDDEN }) {
                    issues.add(HealthIssue(HealthLevel.WARN, "UAS_OI",
                        "唯一产品识别码含禁用字符 O/I"))
                }
            }
        }

        // --- 实名登记 (002, M) ---
        if (pkt.realname == "00000000" || pkt.realname.isEmpty()) {
            issues.add(HealthIssue(HealthLevel.WARN, "REALNAME_EMPTY",
                "实名登记号为默认值/空，未完成 UOM 实名登记"))
        } else if (pkt.realname.length != 8) {
            issues.add(HealthIssue(HealthLevel.WARN, "REALNAME_LEN",
                "实名登记号长度 ${pkt.realname.length}，应为 8"))
        }

        // --- 运行类别/无人机分类 (M) ---
        if (pkt.opCategory !in setOf(1, 2, 3)) {
            issues.add(HealthIssue(HealthLevel.WARN, "OP_CATEGORY",
                "运行类别 ${pkt.opCategory} 无效（应为 1/2/3）"))
        }
        if (pkt.uaClass !in setOf(0, 1, 2, 3, 4)) {
            issues.add(HealthIssue(HealthLevel.WARN, "UA_CLASS",
                "无人机分类 ${pkt.uaClass} 无效（应为 0-4）"))
        }

        // --- 位置 (008, M) ---
        if (pkt.uaLat.isNaN() || pkt.uaLon.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "POS_UNKNOWN",
                "无人机位置未知（0xFFFFFFFF）— 未收到 GPS/飞控数据"))
        } else {
            if (!(-90.0 <= pkt.uaLat && pkt.uaLat <= 90.0)) {
                issues.add(HealthIssue(HealthLevel.FAIL, "LAT_RANGE",
                    String.format(Locale.US, "纬度 %.7f 超出 [-90,90]", pkt.uaLat)))
            }
            if (!(-180.0 <= pkt.uaLon && pkt.uaLon <= 180.0)) {
                issues.add(HealthIssue(HealthLevel.FAIL, "LON_RANGE",
                    String.format(Locale.US, "经度 %.7f 超出 [-180,180]", pkt.uaLon)))
            }
        }

        // --- 高度 (013, M) ---
        if (pkt.geoAlt.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "GEOALT_UNKNOWN",
                "大地高度未知（0）— 未收到高度数据"))
        } else if (!(-1000.0 <= pkt.geoAlt && pkt.geoAlt <= 10000.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "GEOALT_RANGE",
                String.format(Locale.US, "大地高度 %.1fm 超出 [-1000,10000]", pkt.geoAlt)))
        }

        // --- 航迹/地速 (009/010, M) ---
        if (pkt.heading.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "HEADING_UNKNOWN", "航迹角未知（0xFFFF）"))
        } else if (!(0.0 <= pkt.heading && pkt.heading < 360.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "HEADING_RANGE",
                String.format(Locale.US, "航迹角 %.1f° 超出 [0,360)", pkt.heading)))
        }
        if (pkt.speed.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "SPEED_UNKNOWN", "地速未知（0xFFFF）"))
        } else if (!(0.0 <= pkt.speed && pkt.speed <= 6553.5)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "SPEED_RANGE",
                String.format(Locale.US, "地速 %.1fm/s 超出范围", pkt.speed)))
        }

        // --- 运行状态 (015, M) ---
        if (pkt.opStatus !in Decoder.OP_STATUS.keys) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STATUS_INVALID",
                "运行状态 ${pkt.opStatus} 无效（应为 0-5）"))
        } else if (pkt.opStatus == 4 || pkt.opStatus == 5) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STATUS_FAIL",
                "模块上报故障状态：${Decoder.OP_STATUS[pkt.opStatus]}"))
        }

        // --- 遥控站位置 (006, M) ---
        if (pkt.opLat.isNaN() || pkt.opLon.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "OPPOS_UNKNOWN",
                "遥控站位置未知 — 未配置起飞点/Home"))
        } else if (!(-90.0 <= pkt.opLat && pkt.opLat <= 90.0 && -180.0 <= pkt.opLon && pkt.opLon <= 180.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "OPPOS_RANGE", "遥控站位置超出合理范围"))
        }

        // --- 时间戳 (020, M) ---
        if (pkt.timestampMs == 0L) {
            issues.add(HealthIssue(HealthLevel.WARN, "TS_UNSYNCED", "时间戳为 0 — 模块尚未 GPS 授时"))
        } else {
            val now = System.currentTimeMillis()
            val drift = Math.abs(now - pkt.timestampMs)
            if (drift > 5 * 60 * 1000) {
                issues.add(HealthIssue(HealthLevel.WARN, "TS_DRIFT",
                    String.format(Locale.US, "时间戳与电脑时钟偏差 %.0fs，GPS 授时可能异常", drift / 1000.0)))
            }
        }

        return issues
    }
}

/**
 * 累积去重后的数据包并产出流式判定。
 * 时钟可注入（nowFunc），便于纯 JVM 单元测试。
 */
class StreamAssessor(
    private val windowS: Double = 10.0,
    private val nowFunc: () -> Long = { System.nanoTime() / 1_000_000 }
) {
    private val times = ArrayDeque<Long>()
    private var lastPkt: DecodedPacket? = null
    private var lastRaw: ByteArray = ByteArray(0)
    private var identSeq = 0
    private var startMs: Long? = null

    fun push(pkt: DecodedPacket) {
        val now = nowFunc()
        if (startMs == null) startMs = now
        val cutoff = now - (windowS * 1000).toLong()
        while (times.isNotEmpty() && times.first() < cutoff) {
            times.removeFirst()
        }
        times.addLast(now)

        if (pkt.raw.contentEquals(lastRaw)) {
            identSeq += 1
        } else {
            identSeq = 1
        }
        lastRaw = pkt.raw.copyOf()
        lastPkt = pkt
    }

    fun reset() {
        times.clear()
        lastPkt = null
        lastRaw = ByteArray(0)
        identSeq = 0
        startMs = null
    }

    fun report(): HealthReport {
        val now = nowFunc()
        val rep = HealthReport()
        val last = lastPkt ?: run {
            rep.level = HealthLevel.FAIL
            rep.note = "尚未收到任何数据包"
            return rep
        }

        val elapsedS = startMs?.let { (now - it) / 1000.0 } ?: 0.0
        if (times.isNotEmpty()) {
            rep.avgRateHz = times.size / Math.min(elapsedS, windowS)
        }
        rep.packetsSeen = times.size
        rep.staleSeconds = (now - times.last()) / 1000.0

        // --- 最新包的单包问题 ---
        val pktIssues = Health.assessPacket(last)
        rep.issues.addAll(pktIssues)
        rep.packetsOk = if (pktIssues.isEmpty()) 1 else 0

        // --- 速率 (GB 5.1.3: ≥ 1/s) ---
        if (elapsedS >= 3.0) {
            if (rep.packetsSeen == 0) {
                rep.issues.add(HealthIssue(HealthLevel.FAIL, "RATE_ZERO",
                    "近 ${windowS.toInt()}s 内未收到数据包"))
            } else if (rep.avgRateHz < Health.GB_MIN_RATE_HZ * 0.5) {
                rep.issues.add(HealthIssue(HealthLevel.WARN, "RATE_LOW",
                    String.format(Locale.US, "广播速率仅 %.1f 包/s，低于 1 包/s 标准", rep.avgRateHz)))
            } else if (rep.avgRateHz < Health.GB_MIN_RATE_HZ) {
                rep.issues.add(HealthIssue(HealthLevel.WARN, "RATE_SLOW",
                    String.format(Locale.US, "广播速率 %.1f 包/s，略低于 1 包/s", rep.avgRateHz)))
            }
        }

        // --- 停滞 ---
        if (rep.staleSeconds > 5.0) {
            rep.issues.add(HealthIssue(HealthLevel.FAIL, "STALE",
                String.format(Locale.US, "已 %.0fs 无新数据", rep.staleSeconds)))
        } else if (rep.staleSeconds > Health.FRESH_THRESHOLD_S) {
            rep.issues.add(HealthIssue(HealthLevel.WARN, "STALE",
                String.format(Locale.US, "数据已 %.0fs 未更新", rep.staleSeconds)))
        }

        // --- 内容冻结（数据源停滞，而非射频静默） ---
        if (identSeq >= 5 && last.timestampMs != 0L) {
            rep.issues.add(HealthIssue(HealthLevel.WARN, "FROZEN",
                "广播内容长时间未变化 — 数据源可能停滞"))
        }

        // --- 判定 = 最严重问题 ---
        var worst = HealthLevel.PASS
        for (i in rep.issues) {
            if (i.level.ordinal > worst.ordinal) worst = i.level
        }
        rep.level = worst

        rep.note = when (rep.level) {
            HealthLevel.PASS -> "模块工作正常，广播符合 GB 46750-2025 要求"
            HealthLevel.WARN -> "存在可改善项，不影响基本广播"
            else -> "存在故障，请根据下方问题清单排查"
        }

        return rep
    }
}
