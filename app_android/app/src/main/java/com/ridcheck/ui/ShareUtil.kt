package com.ridcheck.ui

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import com.ridcheck.core.DeviceEntry
import com.ridcheck.core.ReportBuilder
import java.io.File

/** 文本与 CSV 文件分享（零 AndroidX：CSV 走自建 RidFileProvider 的 content URI）。 */
object ShareUtil {

    fun shareText(context: Context, title: String, text: String) {
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, title)
            putExtra(Intent.EXTRA_TEXT, text)
        }
        launch(context, send, title)
    }

    /** 把历史采样写成本地 CSV 并通过系统分享面板导出（可存到 WPS/Excel/文件管理）。 */
    fun shareCsv(context: Context, entry: DeviceEntry) {
        val csv = ReportBuilder.buildCsv(entry)
        val name = "rid_${entry.address.replace(':', '-').replace(' ', '_')}.csv"
        val dir = File(context.filesDir, "csv").apply { mkdirs() }
        File(dir, name).writeText(csv)

        val uri = Uri.parse("content://com.ridcheck.files/$name")
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/csv"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            clipData = ClipData.newRawUri("csv", uri)
        }
        launch(context, send, "导出数据")
    }

    private fun launch(context: Context, send: Intent, title: String) {
        try {
            context.startActivity(Intent.createChooser(send, title))
        } catch (e: Exception) {
            Toast.makeText(context, "无可用分享目标", Toast.LENGTH_SHORT).show()
        }
    }
}
