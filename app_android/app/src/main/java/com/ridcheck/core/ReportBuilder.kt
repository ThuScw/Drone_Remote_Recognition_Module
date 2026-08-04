package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/** 单设备合规报告与历史数据 CSV 生成（纯文本，可分享/留存）。nowMs 可注入便于单元测试。 */
object ReportBuilder {

    private val TS = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
        .withZone(ZoneOffset.UTC)

    private val SPARK = "▁▂▃▄▅▆▇█" // ▁▂▃▄▅▆▇█

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
        sb.append("RSSI 采样曲线（最近 ").append(entry.samples.size).append(" 点）:\n")
        sb.append(sparkline(entry.samples.map { it.rssi })).append('\n')
        return sb.toString()
    }

    /** 历史采样 CSV 文本：表头 + 每样本一行（UTC 时间、相对首见秒数、RSSI、速率、判定）。 */
    fun buildCsv(entry: DeviceEntry): String {
        val sb = StringBuilder()
        sb.append("time_utc,elapsed_s,rssi_dbm,rate_pkts,level\n")
        if (entry.samples.isEmpty()) return sb.toString()
        val base = if (entry.firstSeenMs > 0) entry.firstSeenMs else entry.samples.first().timeMs
        for (s in entry.samples) {
            sb.append(TS.format(Instant.ofEpochMilli(s.timeMs)))
                .append(',')
                .append(String.format(Locale.US, "%.3f", (s.timeMs - base) / 1000.0))
                .append(',').append(s.rssi)
                .append(',').append(String.format(Locale.US, "%.3f", s.rateHz))
                .append(',').append(s.level.name)
                .append('\n')
        }
        return sb.toString()
    }

    private fun durationText(entry: DeviceEntry, nowMs: Long): String =
        if (entry.firstSeenMs <= 0) "-"
        else "${(nowMs - entry.firstSeenMs) / 1000} 秒"

    private fun rssiStats(entry: DeviceEntry): String {
        val s = entry.samples
        if (s.isEmpty()) return "${entry.rssi} dBm（无采样，仅最新值）"
        val min = s.minOf { it.rssi }
        val max = s.maxOf { it.rssi }
        val avg = s.map { it.rssi }.average()
        return "${entry.rssi} dBm（最小 $min, 最大 $max, 平均 " +
            String.format(Locale.US, "%.0f", avg) + "）"
    }

    /** 600 点降采样到 ≤60 个块字符，超宽时按步长抽点。 */
    private fun sparkline(values: List<Int>): String {
        if (values.isEmpty()) return "(无采样)"
        val stride = maxOf(1, values.size / 60)
        val sub = values.filterIndexed { i, _ -> i % stride == 0 }
        val lo = sub.min().toDouble()
        val hi = sub.max().toDouble()
        if (hi == lo) return SPARK.last().toString().repeat(sub.size)
        return sub.joinToString("") {
            SPARK[(((it - lo) / (hi - lo)) * (SPARK.length - 1)).toInt()].toString()
        }
    }

    private fun clauseSuffix(issue: HealthIssue): String =
        if (issue.clause.isEmpty()) "" else " (${issue.clause})"
}
