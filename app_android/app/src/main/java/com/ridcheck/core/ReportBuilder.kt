package com.ridcheck.core

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * 单设备合规报告（文本 + Word）与历史数据 CSV 生成。nowMs 可注入便于单元测试。
 * 时间均采用北京时间（Asia/Shanghai）；字段 020 设备时间戳为设备侧 UTC，字段表内已标注。
 */
object ReportBuilder {

    private val TS_BJ = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
        .withZone(ZoneId.of("Asia/Shanghai"))

    private val TS_COMPACT_BJ = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
        .withZone(ZoneId.of("Asia/Shanghai"))

    private val HMS_BJ = DateTimeFormatter.ofPattern("HH:mm:ss")
        .withZone(ZoneId.of("Asia/Shanghai"))

    /** Word 报告里判定等级的着色（与 APP 紫色主题一致的十六进制色值）。 */
    private fun verdictColor(level: HealthLevel): String = when (level) {
        HealthLevel.PASS -> "5E35B1" // 紫
        HealthLevel.WARN -> "B26A00" // 琥珀
        HealthLevel.FAIL -> "B71C1C" // 红
    }

    /** 单台设备的文本报告：判定 + 状态时间线 + 会话统计 + 异常时段 + 问题清单（含建议）。 */
    fun buildDevice(entry: DeviceEntry, nowMs: Long = System.currentTimeMillis()): String {
        val rep = entry.assessor.report()
        val statusLine = statusTimeline(entry, nowMs)
        val sb = StringBuilder()
        sb.append("RID 合规测试报告\n")
        sb.append("判定基准: GB 46750-2025\n")
        sb.append("报告编号: ").append(reportNo(entry.address, nowMs)).append('\n')
        sb.append("生成时间: ").append(TS_BJ.format(Instant.ofEpochMilli(nowMs))).append("（北京时间）\n")
        sb.append('\n')
        sb.append("设备: ").append(entry.address).append('\n')
        sb.append("判定: ").append(rep.level.verdictLabel())
        if (rep.packetsSeen > 0) {
            sb.append(" (").append(rep.packetsSeen).append(" 包, ")
                .append(String.format(Locale.US, "%.1f 包/s", rep.avgRateHz)).append(')')
        }
        sb.append("  ").append(rep.note).append('\n')
        sb.append("运行状态: ").append(statusLine).append('\n')
        sb.append("持续时长: ").append(durationText(entry, nowMs)).append('\n')
        sb.append("RSSI: ").append(rssiStats(entry)).append('\n')
        sb.append('\n')

        sb.append("会话统计（表3 21 项）:\n")
        for (f in GB_FIELD_TABLE) {
            val num = f.num.toIntOrNull() ?: 0
            sb.append("  ").append(f.num).append(' ').append(f.name)
                .append(": ").append(entry.sessionStats.fieldSummary(num, statusLine)).append('\n')
        }
        sb.append('\n')

        val episodes = entry.issueTimeline.snapshot()
        if (episodes.isEmpty()) {
            sb.append("异常时段: 未记录到异常\n")
        } else {
            sb.append("异常时段:\n")
            for (ep in episodes) {
                sb.append("  - [").append(ep.level.label()).append("] ").append(ep.code)
                    .append(' ').append(hmsBj(ep.startMs)).append('~')
                    .append(if (ep.isOpen) "进行中" else hmsBj(ep.endMs))
                    .append(" (").append(formatDuration(ep.durationMs(nowMs))).append("): ")
                    .append(ep.message).append('\n')
                val adv = Health.advice(ep.code)
                if (adv.isNotEmpty()) sb.append("      建议: ").append(adv).append('\n')
            }
        }
        sb.append('\n')

        if (rep.issues.isEmpty()) {
            sb.append("问题清单: 未发现问题\n")
        } else {
            sb.append("问题清单:\n")
            for (it in rep.issues) {
                sb.append("  - [").append(it.level.label()).append("] ")
                    .append(it.code).append(clauseSuffix(it))
                    .append(": ").append(it.message).append('\n')
                val adv = Health.advice(it.code)
                if (adv.isNotEmpty()) sb.append("      建议: ").append(adv).append('\n')
            }
        }
        return sb.toString()
    }

    /**
     * 历史数据 CSV：逐帧广播数据（每帧原始字节 HEX + 表3 21 项解码值，按需重解码），含中文，UTF-8。
     * 时间列为接收时刻（北京时间）。
     */
    fun buildCsv(entry: DeviceEntry): String {
        val sb = StringBuilder()

        sb.append("GB 46750-2025 逐帧广播数据（共 ${entry.frames.size} 帧）\n")
        sb.append("帧序号,时间(北京时间),RSSI(dBm),原始字节(HEX)")
        for (f in GB_FIELD_TABLE) sb.append(',').append(csvCell(f.num + "-" + f.name))
        sb.append('\n')
        if (entry.frames.isEmpty()) {
            sb.append("(无逐帧记录)\n")
        } else {
            entry.frames.forEachIndexed { i, rec ->
                val pkt = Decoder.decodeGbPacket(rec.raw, entry.address, rec.rssi, rec.timeMs)
                sb.append(i + 1).append(',')
                    .append(TS_BJ.format(Instant.ofEpochMilli(rec.timeMs))).append(',')
                    .append(rec.rssi).append(',')
                    .append(csvCell(rawHex(rec.raw)))
                for (f in GB_FIELD_TABLE) {
                    sb.append(',').append(csvCell(pkt.fmt[f.fmtKey] ?: "-"))
                }
                sb.append('\n')
            }
        }
        return sb.toString()
    }

    private fun csvCell(v: String): String {
        val needsQuote = v.any { it == ',' || it == '"' || it == '\n' || it == '\r' || it == '\t' }
        val isNumeric = v.toDoubleOrNull() != null
        // 防公式注入：非数值且以 = + - @（或制表/回车）开头时加前导单引号强制按文本读
        val guarded = !isNumeric && v.isNotEmpty() && v[0] in "=+-@\t\r"
        val safe = if (guarded) "'" + v else v
        return if (needsQuote) "\"" + safe.replace("\"", "\"\"") + "\"" else safe
    }

    /**
     * Word (.docx) 合规检测报告，共 8 章：
     * 一、检测信息；二、被测设备信息；三、判定结果（问题清单含建议）；四、异常事件记录（问题时间轴 + 状态时间线）；
     * 五、广播字段对照表（表3 21 项，会话统计，无最新值列）；六、信号质量与历史采样（RSSI/速率曲线 + 相对轨迹图）；
     * 七、结论；八、声明。
     * chartPng 为 RSSI/速率曲线位图（ChartPng.render），trackPng 为相对轨迹图（ChartPng.renderTrack），null 时相应区块降级为文字。
     */
    fun buildDeviceDocx(
        entry: DeviceEntry,
        nowMs: Long,
        chartPng: ByteArray? = null,
        trackPng: ByteArray? = null
    ): ByteArray {
        val rep = entry.assessor.report()
        val pkt = entry.lastPkt
        val id = entry.address
        val statusLine = statusTimeline(entry, nowMs)

        return DocxBuilder.build { doc ->
            doc.title("民用无人驾驶航空器系统运行识别（RID）检测报告")
            doc.subtitle("依据 GB 46750-2025《民用无人驾驶航空器系统运行识别规范》")
            doc.spacer()

            // 一、检测信息
            doc.heading("一、检测信息")
            doc.keyValue(listOf(
                "报告编号" to reportNo(id, nowMs),
                "检测依据" to "GB 46750-2025（5.1.2 连续广播 / 5.1.3 广播间隔 / 表3 数据项）",
                "检测工具" to "RID 检测 APP v1.7（Android 蓝牙接收）",
                "检测日期" to (TS_BJ.format(Instant.ofEpochMilli(nowMs)) + "（北京时间）"),
                "检测方式" to "现场接收广播式远程识别（BLE 5.0 扩展广播）",
                "检测时长" to durationText(entry, nowMs)
            ))

            // 二、被测设备信息
            doc.heading("二、被测设备信息")
            doc.keyValue(listOf(
                "设备地址 (MAC)" to id,
                "唯一产品识别码" to (pkt?.uasId?.ifEmpty { "(空)" } ?: "-"),
                "实名登记号" to (pkt?.realname?.ifEmpty { "(空)" } ?: "-"),
                "运行类别" to (pkt?.opCategory?.let { OP_CATEGORY_LABELS[it] ?: "无效($it)" } ?: "-"),
                "无人机分类" to (pkt?.uaClass?.let { UA_CLASS_LABELS[it] ?: "无效($it)" } ?: "-"),
                "最新运行状态" to (pkt?.opStatus?.let { Decoder.OP_STATUS[it] ?: "无效($it)" } ?: "-"),
                "广播速率（近 10s）" to String.format(Locale.US, "%.1f 包/s", rep.avgRateHz),
                "累计包数" to "${entry.packetCount} 包（有效 ${entry.packetCount - entry.structErrCount}，结构错误 ${entry.structErrCount}）",
                "RSSI" to rssiStats(entry),
                "持续时长" to durationText(entry, nowMs)
            ))

            // 三、判定结果
            doc.heading("三、判定结果")
            doc.verdict("总体判定：${rep.level.verdictLabel()}", verdictColor(rep.level))
            doc.body(rep.note)
            if (rep.issues.isEmpty()) {
                doc.body("问题清单：未发现问题")
            } else {
                doc.table(
                    header = listOf("级别", "条款", "问题", "建议"),
                    rows = rep.issues.map { listOf(it.level.label(), it.clause, it.message, Health.advice(it.code)) },
                    widths = listOf(0.09, 0.15, 0.40, 0.36)
                )
            }

            // 四、异常事件记录（问题时间轴）
            doc.heading("四、异常事件记录（问题时间轴）")
            val episodes = entry.issueTimeline.snapshot()
            if (episodes.isEmpty()) {
                doc.body("会话期间未记录到异常时段。")
            } else {
                doc.table(
                    header = listOf("问题", "级别", "起始", "结束", "持续", "建议"),
                    rows = episodes.map { ep ->
                        listOf(
                            ep.message,
                            ep.level.label(),
                            hmsBj(ep.startMs),
                            if (ep.isOpen) "进行中" else hmsBj(ep.endMs),
                            formatDuration(ep.durationMs(nowMs)),
                            Health.advice(ep.code)
                        )
                    },
                    widths = listOf(0.28, 0.08, 0.11, 0.11, 0.09, 0.33)
                )
            }
            doc.body("运行状态时间线：$statusLine")

            // 五、广播字段对照表（表3，21 项）
            doc.heading("五、广播字段对照表（表3，21 项，会话统计）")
            if (pkt == null) {
                doc.body("（无数据包）")
            } else {
                doc.table(
                    header = listOf("序号", "数据项", "必选", "携带帧数", "会话统计"),
                    rows = GB_FIELD_TABLE.map { f ->
                        val num = f.num.toIntOrNull() ?: 0
                        val seen = entry.fieldSeen[num]
                        listOf(
                            f.num,
                            f.name,
                            if (f.optional) "O" else "M",
                            if (seen > 0) "$seen/${entry.packetCount}" else "-",
                            entry.sessionStats.fieldSummary(num, statusLine)
                        )
                    },
                    widths = listOf(0.06, 0.13, 0.06, 0.11, 0.64)
                )
            }

            // 六、信号质量与历史采样
            doc.heading("六、信号质量与历史采样")
            if (chartPng != null && entry.samples.isNotEmpty()) {
                doc.image(chartPng, 16.0, 6.2)
                doc.body("信号统计：${rssiStats(entry)}；采样 ${entry.samples.size} 点 @1Hz（保留最近 10 分钟）")
            } else {
                doc.body("（无采样数据）")
            }
            if (trackPng != null && entry.track.isNotEmpty()) {
                doc.image(trackPng, 9.0, 9.0)
                doc.body("相对轨迹图：相对首个有效位置（起飞点）的北/东位移（米），共 ${entry.track.size} 个采样点。")
            }

            // 七、结论
            doc.heading("七、结论")
            doc.body(conclusion(id, rep))
            doc.spacer()

            // 八、声明
            doc.heading("八、声明")
            doc.small("1. 本报告由 RID 检测 APP（v1.7）自动生成，检测数据为现场实时接收的广播式运行识别信息，仅供合规自查参考，不构成官方检测结论。")
            doc.small("2. 检测结果仅对本次检测会话及受检设备负责，具体合规判定以 GB 46750-2025 标准原文及具备资质的检测机构为准。")
            doc.small("3. 逐帧原始字节与对应字段解码请见同一会话导出的 CSV 数据。")
        }
    }

    private fun reportNo(address: String, nowMs: Long): String =
        "RID-" + TS_COMPACT_BJ.format(Instant.ofEpochMilli(nowMs)) + "-" + address.replace(":", "-")

    private fun conclusion(address: String, rep: HealthReport): String = when (rep.level) {
        HealthLevel.PASS ->
            "被测设备 $address 全程持续广播运行识别信息，广播间隔符合 5.1.3 要求，表3 关键字段完整，判定为正常。"
        HealthLevel.WARN ->
            "被测设备 $address 可正常广播运行识别信息，但存在可改善项，判定为警告，建议按问题清单整改后复测。"
        else ->
            "被测设备 $address 广播运行识别信息存在影响合规或安全的问题，判定为故障，建议停机排查后复测。"
    }

    private fun durationText(entry: DeviceEntry, nowMs: Long): String =
        if (entry.firstSeenMs <= 0) "-"
        else "${maxOf(0, (nowMs - entry.firstSeenMs) / 1000)} 秒"

    /** RSSI 统计：最新 / 最小 / 最大 / 平均 / 中位数。 */
    private fun rssiStats(entry: DeviceEntry): String {
        val s = entry.samples
        if (s.isEmpty()) return "${entry.rssi} dBm（无采样，仅最新值）"
        val vals = s.map { it.rssi }.sorted()
        val min = vals.first()
        val max = vals.last()
        val avg = vals.average()
        val med = if (vals.size % 2 == 1) vals[vals.size / 2].toDouble()
        else (vals[vals.size / 2 - 1] + vals[vals.size / 2]) / 2.0
        return "最新 ${entry.rssi} dBm；最小 $min，最大 $max，平均 " +
            String.format(Locale.US, "%.0f", avg) + "，中位数 " + String.format(Locale.US, "%.0f", med)
    }

    /** 运行状态时间线：地面(00:32)→空中(03:11)→紧急(00:05)（由 statusLog 变化记录推算各状态时长）。 */
    private fun statusTimeline(entry: DeviceEntry, nowMs: Long): String {
        val log = entry.statusLog
        if (log.isEmpty()) {
            val op = entry.lastPkt?.opStatus
            return if (op != null) (Decoder.OP_STATUS[op] ?: "无效($op)") + "（全程）" else "（未知）"
        }
        val segs = ArrayList<Pair<Int, Long>>()
        for (i in log.indices) {
            val start = log[i].timeMs
            val end = if (i + 1 < log.size) log[i + 1].timeMs else nowMs
            val dur = maxOf(0L, end - start)
            if (segs.isNotEmpty() && segs.last().first == log[i].opStatus) {
                segs[segs.size - 1] = segs.last().first to (segs.last().second + dur)
            } else {
                segs.add(log[i].opStatus to dur)
            }
        }
        val parts = segs.filter { it.second > 0 }.joinToString("→") { (st, dur) ->
            "${Decoder.OP_STATUS[st] ?: "无效($st)"}(${formatDuration(dur)})"
        }
        if (parts.isNotEmpty()) return parts
        return (Decoder.OP_STATUS[log.last().opStatus] ?: "无效") + "（全程）"
    }

    private fun hmsBj(ms: Long): String = try {
        HMS_BJ.format(Instant.ofEpochMilli(ms))
    } catch (e: Exception) {
        "-"
    }

    private fun rawHex(raw: ByteArray): String =
        raw.joinToString(" ") { String.format("%02X", it.toInt() and 0xFF) }

    private fun clauseSuffix(issue: HealthIssue): String =
        if (issue.clause.isEmpty()) "" else " (${issue.clause})"
}
