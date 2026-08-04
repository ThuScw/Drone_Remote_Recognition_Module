package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/** 单设备合规报告与历史数据 CSV 生成（纯文本，可分享/留存）。nowMs 可注入便于单元测试。 */
object ReportBuilder {

    private val TS = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
        .withZone(ZoneOffset.UTC)

    private val TS_COMPACT = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
        .withZone(ZoneOffset.UTC)

    /** Word 报告里判定等级的着色（与 APP 紫色主题一致的十六进制色值）。 */
    private fun verdictColor(level: HealthLevel): String = when (level) {
        HealthLevel.PASS -> "5E35B1" // 紫
        HealthLevel.WARN -> "B26A00" // 琥珀
        HealthLevel.FAIL -> "B71C1C" // 红
    }

    /** 单台设备的文本报告：判定 + 运行状态 + 时长 + RSSI 统计 + 问题清单 + RSSI 采样曲线。 */
    fun buildDevice(entry: DeviceEntry, nowMs: Long = System.currentTimeMillis()): String {
        val rep = entry.assessor.report()
        val sb = StringBuilder()
        sb.append("RID 合规测试报告\n")
        sb.append("判定基准: GB 46750-2025\n")
        sb.append("生成时间: ").append(TS.format(Instant.ofEpochMilli(nowMs))).append(" UTC\n")
        sb.append('\n')
        sb.append("设备: ").append(entry.address).append('\n')
        sb.append("判定: ").append(rep.level.verdictLabel())
        if (rep.packetsSeen > 0) {
            sb.append(" (").append(rep.packetsSeen).append(" 包, ")
                .append(String.format(Locale.US, "%.1f 包/s", rep.avgRateHz)).append(')')
        }
        sb.append("  ").append(rep.note).append('\n')
        val op = entry.lastPkt?.opStatus
        sb.append("运行状态: ")
            .append(if (op != null) Decoder.OP_STATUS[op] ?: "无效($op)" else "-").append('\n')
        sb.append("持续时长: ").append(durationText(entry, nowMs)).append('\n')
        sb.append("RSSI: ").append(rssiStats(entry)).append('\n')
        if (rep.issues.isEmpty()) {
            sb.append("问题清单: 未发现问题\n")
        } else {
            sb.append("问题清单:\n")
            for (it in rep.issues) {
                sb.append("  - [").append(it.level.label()).append("] ")
                    .append(it.code).append(clauseSuffix(it))
                    .append(": ").append(it.message).append('\n')
            }
        }
        return sb.toString()
    }

    /**
     * 历史数据 CSV：逐帧广播数据（每帧原始字节 HEX + 表3 21 项解码值，按需重解码），含中文，UTF-8。
     */
    fun buildCsv(entry: DeviceEntry): String {
        val sb = StringBuilder()

        sb.append("GB 46750-2025 逐帧广播数据（共 ${entry.frames.size} 帧）\n")
        sb.append("帧序号,时间(UTC),RSSI(dBm),原始字节(HEX)")
        for (f in GB_FIELD_TABLE) sb.append(',').append(csvCell(f.num + "-" + f.name))
        sb.append('\n')
        if (entry.frames.isEmpty()) {
            sb.append("(无逐帧记录)\n")
        } else {
            entry.frames.forEachIndexed { i, rec ->
                val pkt = Decoder.decodeGbPacket(rec.raw, entry.address, rec.rssi, rec.timeMs)
                sb.append(i + 1).append(',')
                    .append(TS.format(Instant.ofEpochMilli(rec.timeMs))).append(',')
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
     * Word (.docx) 合规检测报告：检测信息 / 被测设备 / 判定结果+问题清单 /
     * 表3 会话累计字段对照表 / 信号质量与采样曲线（内嵌 PNG 图）/ 结论 / 免责声明。
     * chartPng 为采样曲线位图（ChartPng.render 产物），null 时曲线区块降级为文字占位。
     */
    fun buildDeviceDocx(entry: DeviceEntry, nowMs: Long, chartPng: ByteArray? = null): ByteArray {
        val rep = entry.assessor.report()
        val pkt = entry.lastPkt
        val id = entry.address

        return DocxBuilder.build { doc ->
            doc.title("民用无人驾驶航空器系统运行识别（RID）检测报告")
            doc.subtitle("依据 GB 46750-2025《民用无人驾驶航空器系统运行识别规范》")
            doc.spacer()

            doc.heading("一、检测信息")
            doc.keyValue(listOf(
                "报告编号" to reportNo(id, nowMs),
                "检测依据" to "GB 46750-2025（5.1.2 连续广播 / 5.1.3 广播间隔 / 表3 数据项）",
                "检测工具" to "RID 检测 APP v1.4（Android 蓝牙接收）",
                "检测日期" to (TS.format(Instant.ofEpochMilli(nowMs)) + " UTC"),
                "检测方式" to "现场接收广播式远程识别（BLE 5.0 扩展广播）"
            ))

            doc.heading("二、被测设备信息")
            doc.keyValue(listOf(
                "设备地址 (MAC)" to id,
                "唯一产品识别码" to (pkt?.uasId?.ifEmpty { "(空)" } ?: "-"),
                "实名登记号" to (pkt?.realname?.ifEmpty { "(空)" } ?: "-"),
                "运行状态" to (pkt?.opStatus?.let { Decoder.OP_STATUS[it] ?: "无效($it)" } ?: "-"),
                "广播速率" to String.format(Locale.US, "%.1f 包/s（累计 %d 包）", rep.avgRateHz, rep.packetsSeen),
                "持续时长" to durationText(entry, nowMs),
                "RSSI" to rssiStats(entry)
            ))

            doc.heading("三、判定结果")
            doc.verdict("总体判定：${rep.level.verdictLabel()}", verdictColor(rep.level))
            doc.body(rep.note)
            if (rep.issues.isEmpty()) {
                doc.body("问题清单：未发现问题")
            } else {
                doc.table(
                    header = listOf("级别", "条款", "问题"),
                    rows = rep.issues.map { listOf(it.level.label(), it.clause, it.message) },
                    widths = listOf(0.12, 0.22, 0.66)
                )
            }

            doc.heading("四、广播字段对照表（表3，21 项，会话累计）")
            if (pkt == null) {
                doc.body("（无数据包）")
            } else {
                doc.table(
                    header = listOf("序号", "数据项", "携带帧数", "最新解析值", "必选"),
                    rows = GB_FIELD_TABLE.map { f ->
                        val seen = entry.fieldSeen[f.num.toIntOrNull() ?: 0]
                        listOf(
                            f.num,
                            f.name,
                            if (seen > 0) "$seen/${entry.packetCount}" else "-",
                            pkt.fmt[f.fmtKey]?.ifEmpty { "(空)" } ?: "-",
                            if (f.optional) "O" else "M"
                        )
                    },
                    widths = listOf(0.07, 0.16, 0.13, 0.58, 0.06)
                )
            }

            doc.heading("五、信号质量与历史采样")
            if (chartPng != null && entry.samples.isNotEmpty()) {
                doc.image(chartPng, 16.0, 6.2)
                doc.body("统计：${rssiStats(entry)}；采样 ${entry.samples.size} 点 @1Hz（保留最近 10 分钟）")
            } else {
                doc.body("（无采样数据）")
            }

            doc.heading("六、结论")
            doc.body(conclusion(id, rep))
            doc.spacer()
            doc.small(
                "本报告由 RID 检测 APP 自动生成，仅供合规自查参考，不构成官方检测结论；" +
                    "具体合规判定以 GB 46750-2025 标准原文及具备资质的检测机构为准。\n" +
                    "逐帧原始字节与对应字段解码请见同一会话导出的 CSV（导出数据）。"
            )
        }
    }

    private fun reportNo(address: String, nowMs: Long): String =
        "RID-" + TS_COMPACT.format(Instant.ofEpochMilli(nowMs)) + "-" + address.replace(":", "-")

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

    private fun rssiStats(entry: DeviceEntry): String {
        val s = entry.samples
        if (s.isEmpty()) return "${entry.rssi} dBm（无采样，仅最新值）"
        val min = s.minOf { it.rssi }
        val max = s.maxOf { it.rssi }
        val avg = s.map { it.rssi }.average()
        return "${entry.rssi} dBm（最小 $min, 最大 $max, 平均 " +
            String.format(Locale.US, "%.0f", avg) + "）"
    }

    private fun rawHex(raw: ByteArray): String =
        raw.joinToString(" ") { String.format("%02X", it.toInt() and 0xFF) }

    private fun clauseSuffix(issue: HealthIssue): String =
        if (issue.clause.isEmpty()) "" else " (${issue.clause})"
}
