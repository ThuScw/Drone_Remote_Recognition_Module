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

        // --- 结构（5.2.3 表3 数据格式） ---
        if (pkt.dataType != Decoder.GB46750_DATA_TYPE) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STRUCT_TYPE",
                String.format("dataType=0x%02X，应为 0xFF", pkt.dataType),
                clause = "5.2.3 表3 数据格式"))
        }
        if (pkt.version != Decoder.GB46750_VERSION) {
            issues.add(HealthIssue(HealthLevel.WARN, "STRUCT_VER",
                String.format("版本=0x%02X，标准为 0x20 (V1.0)；旧版/非标设备降级为警告", pkt.version),
                clause = "5.2.3 表3 数据格式"))
        }
        if (pkt.structureError.isNotEmpty()) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STRUCT_LEN", pkt.structureError,
                clause = "5.2.3 表3 数据格式"))
        }

        // --- UAS_ID (001, M) ---
        val uas = pkt.uasId
        when {
            uas.isEmpty() ->
                issues.add(HealthIssue(HealthLevel.FAIL, "UAS_EMPTY", "唯一产品识别码为空",
                    clause = "表3-001"))
            uas.length != 20 ->
                issues.add(HealthIssue(HealthLevel.WARN, "UAS_LEN",
                    "唯一产品识别码长度 ${uas.length}，应为 20",
                    clause = "表3-001"))
            else -> {
                if (!UAS_ID_CHARSET.matches(uas)) {
                    issues.add(HealthIssue(HealthLevel.WARN, "UAS_CHARSET",
                        "唯一产品识别码含非 0-9/A-Z 字符",
                        clause = "表3-001"))
                }
                if (uas.any { it in UAS_ID_FORBIDDEN }) {
                    issues.add(HealthIssue(HealthLevel.WARN, "UAS_OI",
                        "唯一产品识别码含禁用字符 O/I",
                        clause = "表3-001"))
                }
            }
        }

        // --- 实名登记 (002, M) ---
        if (pkt.realname == "00000000" || pkt.realname.isEmpty()) {
            issues.add(HealthIssue(HealthLevel.WARN, "REALNAME_EMPTY",
                "实名登记号为默认值/空，未完成 UOM 实名登记",
                clause = "表3-002"))
        } else if (pkt.realname.length != 8) {
            issues.add(HealthIssue(HealthLevel.WARN, "REALNAME_LEN",
                "实名登记号长度 ${pkt.realname.length}，应为 8",
                clause = "表3-002"))
        }

        // --- 运行类别/无人机分类 (M) ---
        if (pkt.opCategory !in setOf(1, 2, 3)) {
            issues.add(HealthIssue(HealthLevel.WARN, "OP_CATEGORY",
                "运行类别 ${pkt.opCategory} 无效（应为 1/2/3）",
                clause = "表3-003"))
        }
        if (pkt.uaClass !in setOf(0, 1, 2, 3, 4)) {
            issues.add(HealthIssue(HealthLevel.WARN, "UA_CLASS",
                "无人机分类 ${pkt.uaClass} 无效（应为 0-4）",
                clause = "表3-004"))
        }

        // --- 位置 (008, M) ---
        if (pkt.uaLat.isNaN() || pkt.uaLon.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "POS_UNKNOWN",
                "无人机位置未知（0xFFFFFFFF）— 未收到 GPS/飞控数据",
                clause = "表3-008"))
        } else {
            if (!(-90.0 <= pkt.uaLat && pkt.uaLat <= 90.0)) {
                issues.add(HealthIssue(HealthLevel.FAIL, "LAT_RANGE",
                    String.format(Locale.US, "纬度 %.7f 超出 [-90,90]", pkt.uaLat),
                    clause = "表3-008"))
            }
            if (!(-180.0 <= pkt.uaLon && pkt.uaLon <= 180.0)) {
                issues.add(HealthIssue(HealthLevel.FAIL, "LON_RANGE",
                    String.format(Locale.US, "经度 %.7f 超出 [-180,180]", pkt.uaLon),
                    clause = "表3-008"))
            }
        }

        // --- 高度 (013, M) ---
        if (pkt.geoAlt.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "GEOALT_UNKNOWN",
                "大地高度未知（0）— 未收到高度数据",
                clause = "表3-013"))
        } else if (!(-1000.0 <= pkt.geoAlt && pkt.geoAlt <= 10000.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "GEOALT_RANGE",
                String.format(Locale.US, "大地高度 %.1fm 超出 [-1000,10000]", pkt.geoAlt),
                clause = "表3-013"))
        }

        // --- 航迹/地速 (009/010, M) ---
        if (pkt.heading.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "HEADING_UNKNOWN", "航迹角未知（0xFFFF）",
                clause = "表3-009"))
        } else if (!(0.0 <= pkt.heading && pkt.heading < 360.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "HEADING_RANGE",
                String.format(Locale.US, "航迹角 %.1f° 超出 [0,360)", pkt.heading),
                clause = "表3-009"))
        }
        if (pkt.speed.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "SPEED_UNKNOWN", "地速未知（0xFFFF）",
                clause = "表3-010"))
        } else if (!(0.0 <= pkt.speed && pkt.speed <= 6553.5)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "SPEED_RANGE",
                String.format(Locale.US, "地速 %.1fm/s 超出范围", pkt.speed),
                clause = "表3-010"))
        }

        // --- 运行状态 (015, M) ---
        if (pkt.opStatus !in Decoder.OP_STATUS.keys) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STATUS_INVALID",
                "运行状态 ${pkt.opStatus} 无效（应为 0-5）",
                clause = "表3-015"))
        } else if (pkt.opStatus == 4 || pkt.opStatus == 5) {
            issues.add(HealthIssue(HealthLevel.FAIL, "STATUS_FAIL",
                "设备上报故障状态：${Decoder.OP_STATUS[pkt.opStatus]}",
                clause = "表3-015"))
        }

        // --- 遥控站位置 (006, M) ---
        if (pkt.opLat.isNaN() || pkt.opLon.isNaN()) {
            issues.add(HealthIssue(HealthLevel.WARN, "OPPOS_UNKNOWN",
                "遥控站位置未知 — 未配置起飞点/Home",
                clause = "表3-006"))
        } else if (!(-90.0 <= pkt.opLat && pkt.opLat <= 90.0 && -180.0 <= pkt.opLon && pkt.opLon <= 180.0)) {
            issues.add(HealthIssue(HealthLevel.FAIL, "OPPOS_RANGE", "遥控站位置超出合理范围",
                clause = "表3-006"))
        }

        // --- 时间戳 (020, M) ---
        if (pkt.timestampMs == 0L) {
            issues.add(HealthIssue(HealthLevel.WARN, "TS_UNSYNCED", "时间戳为 0 — 设备尚未 GPS 授时",
                clause = "表3-020"))
        } else {
            val now = System.currentTimeMillis()
            val drift = Math.abs(now - pkt.timestampMs)
            if (drift > 5 * 60 * 1000) {
                issues.add(HealthIssue(HealthLevel.WARN, "TS_DRIFT",
                    String.format(Locale.US, "时间戳与手机时钟偏差 %.0fs，GPS 授时可能异常", drift / 1000.0),
                    clause = "表3-020"))
            }
        }

        return issues
    }

    /** 问题 code → 一句整改建议（报告「建议」列 / 文本报告使用）。未知 code 返回空串。 */
    fun advice(code: String): String = when (code) {
        "STRUCT_TYPE" -> "数据包头部 dataType 非 0xFF，非标准运行识别包；核对广播载荷编码。"
        "STRUCT_VER" -> "数据包版本非 0x20（V1.0），可能为旧版/非标固件；建议升级固件。"
        "STRUCT_LEN" -> "数据包长度与头部声明不一致，疑粘包/截断；抓包复测并核对编码长度。"
        "UAS_EMPTY" -> "唯一产品识别码为空；在固件 config 填入 UOM 注册的 20 位 ID。"
        "UAS_LEN" -> "唯一产品识别码长度不符，应为 20 位；核对固件配置。"
        "UAS_CHARSET" -> "识别码含非 0-9/A-Z 字符；仅允许大写字母与数字。"
        "UAS_OI" -> "识别码含禁用字符 O/I（易与 0/1 混淆）；更换为合法字符。"
        "REALNAME_EMPTY" -> "实名登记号为默认值，未完成 UOM 实名登记；登记后填入 8 位登记号。"
        "REALNAME_LEN" -> "实名登记号长度不符，UOM 登记号固定 8 位。"
        "OP_CATEGORY" -> "运行类别无效；应填 1(开放类)/2(特定类)/3(审定类)。"
        "UA_CLASS" -> "无人机分类无效；应填 0(微型)/1(轻型)/2(小型)/3(中型)/4(大型)。"
        "POS_UNKNOWN" -> "无人机位置未知，未收到 GPS/飞控位置；检查 GPS 定位与飞控数据链路。"
        "LAT_RANGE" -> "纬度超出 [-90,90]，数据异常；核对飞控经纬度换算。"
        "LON_RANGE" -> "经度超出 [-180,180]，数据异常；核对飞控经纬度换算。"
        "GEOALT_UNKNOWN" -> "大地高度未上报；检查 GPS 定位或高度数据链路。"
        "GEOALT_RANGE" -> "大地高度超出合理范围；核对高度数据源。"
        "HEADING_UNKNOWN" -> "航迹角未上报；检查飞控航向输出。"
        "HEADING_RANGE" -> "航迹角超出 [0,360)，数据异常；核对航向编码。"
        "SPEED_UNKNOWN" -> "地速未上报；检查飞控速度输出。"
        "SPEED_RANGE" -> "地速超出合理范围；核对速度编码。"
        "STATUS_INVALID" -> "运行状态值无效；核对 GB 状态字节 0-5。"
        "STATUS_FAIL" -> "设备上报识别发送功能失效；按故障排查（天线/功率/模块自检），必要时重启模块。"
        "OPPOS_UNKNOWN" -> "遥控站位置未上报；确认已设置起飞点/Home 或遥控站 GPS 正常。"
        "OPPOS_RANGE" -> "遥控站位置超出合理范围；核对操作员位置数据。"
        "TS_UNSYNCED" -> "时间戳为 0，设备未 GPS 授时；确保模块在有卫星信号处上电。"
        "TS_DRIFT" -> "时间戳与手机时钟偏差大，GPS 授时异常；重启并等待授时收敛。"
        "RATE_ZERO" -> "近 10s 无数据包，广播中断；检查模块电源/天线/飞控数据源。"
        "RATE_LOW" -> "广播速率低于 1 包/s 标准；检查广播间隔配置与射频拥塞。"
        "RATE_SLOW" -> "广播速率接近下限；适当缩短广播间隔或减小干扰。"
        "STALE" -> "数据长时间未更新，链路停滞；检查飞控连接与模块运行状态。"
        "FROZEN" -> "广播内容长时间不变，数据源可能冻结；检查飞控数据输出与模块缓存。"
        else -> ""
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
                    "近 ${windowS.toInt()}s 内未收到数据包",
                    clause = "5.1.3 广播间隔≤1s"))
            } else if (rep.avgRateHz < Health.GB_MIN_RATE_HZ * 0.5) {
                rep.issues.add(HealthIssue(HealthLevel.WARN, "RATE_LOW",
                    String.format(Locale.US, "广播速率仅 %.1f 包/s，低于 1 包/s 标准", rep.avgRateHz),
                    clause = "5.1.3 广播间隔≤1s"))
            } else if (rep.avgRateHz < Health.GB_MIN_RATE_HZ) {
                rep.issues.add(HealthIssue(HealthLevel.WARN, "RATE_SLOW",
                    String.format(Locale.US, "广播速率 %.1f 包/s，略低于 1 包/s", rep.avgRateHz),
                    clause = "5.1.3 广播间隔≤1s"))
            }
        }

        // --- 停滞 (5.1.2 全程连续广播) ---
        if (rep.staleSeconds > 5.0) {
            rep.issues.add(HealthIssue(HealthLevel.FAIL, "STALE",
                String.format(Locale.US, "已 %.0fs 无新数据", rep.staleSeconds),
                clause = "5.1.2 全程连续广播"))
        } else if (rep.staleSeconds > Health.FRESH_THRESHOLD_S) {
            rep.issues.add(HealthIssue(HealthLevel.WARN, "STALE",
                String.format(Locale.US, "数据已 %.0fs 未更新", rep.staleSeconds),
                clause = "5.1.2 全程连续广播"))
        }

        // --- 内容冻结（数据源停滞，而非射频静默；5.1.2 全程连续广播） ---
        if (identSeq >= 5 && last.timestampMs != 0L) {
            rep.issues.add(HealthIssue(HealthLevel.WARN, "FROZEN",
                "广播内容长时间未变化 — 数据源可能停滞",
                clause = "5.1.2 全程连续广播"))
        }

        // --- 判定 = 最严重问题 ---
        var worst = HealthLevel.PASS
        for (i in rep.issues) {
            if (i.level.ordinal > worst.ordinal) worst = i.level
        }
        rep.level = worst

        rep.note = when (rep.level) {
            HealthLevel.PASS -> "设备工作正常，广播符合 GB 46750-2025 要求"
            HealthLevel.WARN -> "存在可改善项，不影响基本广播"
            else -> "存在故障，请根据下方问题清单排查"
        }

        return rep
    }
}
