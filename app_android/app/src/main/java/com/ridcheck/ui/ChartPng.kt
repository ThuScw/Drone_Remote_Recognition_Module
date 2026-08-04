package com.ridcheck.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import com.ridcheck.core.SamplePoint
import java.io.ByteArrayOutputStream
import java.util.Locale

/**
 * 报告内嵌 RSSI/速率双轴曲线图（PNG）。
 * 与 RidChartView 同款配色与时间轴逻辑（Long 差值避免 epoch 毫秒转 Float 丢精度），
 * 但固定 1240×480 像素，供 docx 以 16cm×6.2cm（≈2.58 宽高比）内嵌不失真。
 * 依赖 android.*，不可 JVM 单测，真机/构建验证。
 */
object ChartPng {

    const val WIDTH = 1240
    const val HEIGHT = 480

    fun render(samples: List<SamplePoint>, width: Int = WIDTH, height: Int = HEIGHT): ByteArray {
        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        try {
            val canvas = Canvas(bmp)
            draw(canvas, samples, width, height)
            val out = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
            return out.toByteArray()
        } finally {
            bmp.recycle()
        }
    }

    private fun draw(canvas: Canvas, samples: List<SamplePoint>, width: Int, height: Int) {
        val padL = 90f
        val padR = 90f
        val padT = 36f
        val padB = 70f
        val plotW = width - padL - padR
        val plotH = height - padT - padB
        val plotBottom = padT + plotH

        val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(224, 224, 224)
            style = Paint.Style.STROKE
            strokeWidth = 2f
        }
        val rssiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(94, 53, 177)
            style = Paint.Style.STROKE
            strokeWidth = 5f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        val ratePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(230, 126, 34)
            style = Paint.Style.STROKE
            strokeWidth = 5f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 26f
            color = Color.rgb(90, 90, 90)
        }
        val legendPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 26f
            color = Color.rgb(60, 60, 60)
        }

        if (samples.isEmpty()) {
            labelPaint.textAlign = Paint.Align.CENTER
            val y = (height - labelPaint.ascent() - labelPaint.descent()) / 2
            canvas.drawText("（无采样数据）", width / 2f, y, labelPaint)
            return
        }

        var rssiMin = Int.MAX_VALUE
        var rssiMax = Int.MIN_VALUE
        var rateMax = 0.0
        for (s in samples) {
            rssiMin = minOf(rssiMin, s.rssi)
            rssiMax = maxOf(rssiMax, s.rssi)
            rateMax = maxOf(rateMax, s.rateHz)
        }
        val rssiLo = (rssiMin - 5).toFloat()
        val rssiHi = (rssiMax + 5).toFloat()
        val rateHi = maxOf((rateMax * 1.2).toFloat(), 1f)
        val t0Ms = samples.first().timeMs
        val t1Ms = samples.last().timeMs
        val spanMs = t1Ms - t0Ms
        val sameMs = spanMs <= 0

        fun xOf(idx: Int, tMs: Long): Float = when {
            samples.size == 1 -> padL + plotW / 2f
            sameMs -> padL + idx.toFloat() / (samples.size - 1) * plotW
            else -> padL + (tMs - t0Ms).toFloat() / spanMs.toFloat() * plotW
        }
        fun rssiY(v: Float): Float = padT + (rssiHi - v) / (rssiHi - rssiLo) * plotH
        fun rateY(v: Float): Float = padT + (rateHi - v) / rateHi * plotH

        // ---- 网格 + 双轴刻度（左右各 4 档，位置对齐） ----
        labelPaint.textAlign = Paint.Align.RIGHT
        for (i in 0..3) {
            val v = rssiLo + (rssiHi - rssiLo) * i / 3
            val y = rssiY(v)
            canvas.drawLine(padL, y, padL + plotW, y, gridPaint)
            canvas.drawText(
                String.format(Locale.US, "%.0f", v),
                padL - 14f,
                y - (labelPaint.ascent() + labelPaint.descent()) / 2,
                labelPaint
            )
        }
        labelPaint.textAlign = Paint.Align.LEFT
        for (i in 0..3) {
            val v = rateHi * i / 3
            val y = rateY(v)
            canvas.drawText(
                String.format(Locale.US, "%.1f", v),
                padL + plotW + 14f,
                y - (labelPaint.ascent() + labelPaint.descent()) / 2,
                labelPaint
            )
        }

        // ---- 曲线（1240px 足够密，直接画折线；长轴走 Long 差值，无精度冻结） ----
        val rssiPath = Path()
        val ratePath = Path()
        samples.forEachIndexed { i, s ->
            val x = xOf(i, s.timeMs)
            if (i == 0) {
                rssiPath.moveTo(x, rssiY(s.rssi.toFloat()))
                ratePath.moveTo(x, rateY(s.rateHz.toFloat()))
            } else {
                rssiPath.lineTo(x, rssiY(s.rssi.toFloat()))
                ratePath.lineTo(x, rateY(s.rateHz.toFloat()))
            }
        }
        canvas.drawPath(rssiPath, rssiPaint)
        canvas.drawPath(ratePath, ratePaint)

        // ---- 图例（顶部，颜色对应） ----
        val legendY = padT - 12f
        val sw = 46f
        canvas.drawLine(padL, legendY, padL + sw, legendY, rssiPaint)
        legendPaint.textAlign = Paint.Align.LEFT
        val l1 = padL + sw + 14f
        canvas.drawText(
            "RSSI (dBm)", l1,
            legendY - (legendPaint.ascent() + legendPaint.descent()) / 2, legendPaint
        )
        val l2 = l1 + legendPaint.measureText("RSSI (dBm)") + 46f
        canvas.drawLine(l2, legendY, l2 + sw, legendY, ratePaint)
        canvas.drawText(
            "速率 (包/s)", l2 + sw + 14f,
            legendY - (legendPaint.ascent() + legendPaint.descent()) / 2, legendPaint
        )

        // ---- 时间轴：距首采 mm:ss（3 个标签）+ 轴说明 ----
        labelPaint.textAlign = Paint.Align.CENTER
        val yTicks = plotBottom + 36f
        val midMs = t0Ms + spanMs / 2
        canvas.drawText(mmss(0), padL, yTicks, labelPaint)
        canvas.drawText(mmss((midMs - t0Ms) / 1000), padL + plotW / 2f, yTicks, labelPaint)
        canvas.drawText(mmss((t1Ms - t0Ms) / 1000), padL + plotW, yTicks, labelPaint)
        canvas.drawText(
            "时间（距首采 mm:ss）", padL + plotW / 2f, plotBottom + 64f,
            labelPaint
        )
    }

    private fun mmss(sec: Long): String =
        String.format(Locale.US, "%d:%02d", sec / 60, sec % 60)
}
