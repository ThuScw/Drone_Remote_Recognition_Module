package com.ridcheck.ui

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import com.ridcheck.core.DeviceEntry
import com.ridcheck.core.RidFileProvider
import com.ridcheck.core.ReportBuilder
import java.io.File
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** 文本 / CSV / Word 报告分享（零 AndroidX：文件走自建 RidFileProvider 的 content URI）。 */
object ShareUtil {

    private val STAMP = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
        .withZone(ZoneId.systemDefault())

    fun shareText(context: Context, title: String, text: String) {
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, title)
            putExtra(Intent.EXTRA_TEXT, text)
        }
        launch(context, send, title)
    }

    /** 历史数据 CSV（逐帧原始字节+解码 + 时间序列），文件名为 设备名_时间戳.csv。 */
    fun shareCsv(context: Context, entry: DeviceEntry) {
        val csv = ReportBuilder.buildCsv(entry)
        val name = "rid_${safeAddr(entry.address)}_${stamp()}.csv"
        shareFile(context, "导出数据", name, csv.toByteArray(Charsets.UTF_8))
    }

    /** Word 合规检测报告（.docx），内嵌 RSSI/速率采样曲线图 + 相对轨迹图。 */
    fun shareReport(context: Context, entry: DeviceEntry) {
        val nowMs = System.currentTimeMillis()
        val chart = if (entry.samples.isEmpty()) null else ChartPng.render(entry.samples)
        val track = if (entry.track.isEmpty()) null else ChartPng.renderTrack(entry.track.toList())
        val bytes = ReportBuilder.buildDeviceDocx(entry, nowMs, chart, track)
        val name = "RID检测报告_${safeAddr(entry.address)}_${stamp()}.docx"
        shareFile(context, "分享报告", name, bytes)
    }

    private fun shareFile(context: Context, title: String, name: String, data: ByteArray) {
        val dir = File(context.filesDir, "share").apply { mkdirs() }
        pruneStale(dir) // 只保留 7 天内的导出文件，避免无限累积
        File(dir, name).writeBytes(data)

        val uri = Uri.parse("content://com.ridcheck.files/" + Uri.encode(name))
        val send = Intent(Intent.ACTION_SEND).apply {
            type = RidFileProvider.mimeFor(name)
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            clipData = ClipData.newRawUri("rid", uri)
        }
        launch(context, send, title)
    }

    private fun safeAddr(address: String): String =
        address.replace(':', '-').replace(' ', '_').replace('/', '_').replace('\\', '_')

    private fun pruneStale(dir: File) {
        val cutoff = System.currentTimeMillis() - 7L * 24 * 60 * 60 * 1000
        dir.listFiles()?.forEach { f ->
            if (f.isFile && f.lastModified() < cutoff) f.delete()
        }
    }

    private fun stamp(): String = STAMP.format(java.time.Instant.now())

    private fun launch(context: Context, send: Intent, title: String) {
        try {
            context.startActivity(Intent.createChooser(send, title))
        } catch (e: Exception) {
            Toast.makeText(context, "无可用分享目标", Toast.LENGTH_SHORT).show()
        }
    }
}
