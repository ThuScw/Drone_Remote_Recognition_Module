package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/** 合规测试报告生成（纯文本，可分享/留存）。nowMs 可注入便于单元测试。 */
object ReportBuilder {

    private val TS = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
        .withZone(ZoneOffset.UTC)

    fun build(devices: List<DeviceEntry>, nowMs: Long = System.currentTimeMillis()): String {
        val sb = StringBuilder()
        sb.append("RID 合规测试报告\n")
        sb.append("判定基准: GB 46750-2025\n")
        sb.append("生成时间: ").append(TS.format(Instant.ofEpochMilli(nowMs))).append(" UTC\n")

        if (devices.isEmpty()) {
            sb.append("\n未发现任何设备。")
            return sb.toString()
        }

        devices.forEachIndexed { idx, e ->
            sb.append('\n').append('[').append(idx + 1).append("] 设备 ").append(e.address).append('\n')
            val rep = e.assessor.report()
            sb.append("    判定: ").append(rep.level.verdictLabel())
            if (rep.packetsSeen > 0) {
                sb.append(" (").append(rep.packetsSeen).append(" 包, ")
                    .append(String.format(Locale.US, "%.1f 包/s", rep.avgRateHz)).append(')')
            }
            sb.append('\n')
            val op = e.lastPkt?.opStatus
            sb.append("    运行状态: ").append(if (op != null) Decoder.OP_STATUS[op] ?: "无效($op)" else "-").append('\n')
            sb.append("    RSSI: ").append(e.rssi).append(" dBm\n")
            sb.append("    首次发现: ").append(timeText(e.firstSeenMs)).append('\n')
            sb.append("    最后更新: ").append(timeText(e.lastSeenMs)).append('\n')
            if (rep.issues.isEmpty()) {
                sb.append("    问题: 未发现问题\n")
            } else {
                sb.append("    问题:\n")
                for (it in rep.issues) {
                    sb.append("      - [").append(it.level.label()).append("] ")
                        .append(it.code).append(clauseSuffix(it))
                        .append(": ").append(it.message).append('\n')
                }
            }
        }
        return sb.toString()
    }

    private fun timeText(ms: Long): String =
        if (ms <= 0) "-" else TS.format(Instant.ofEpochMilli(ms))

    private fun clauseSuffix(issue: HealthIssue): String =
        if (issue.clause.isEmpty()) "" else " (${issue.clause})"
}
