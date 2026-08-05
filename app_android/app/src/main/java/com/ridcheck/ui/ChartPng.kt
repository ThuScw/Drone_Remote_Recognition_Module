package com.ridcheck.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import com.ridcheck.core.SamplePoint
import com.ridcheck.core.TrackPoint
import java.io.ByteArrayOutputStream
import java.util.Locale
import kotlin.math.sqrt

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

    // ---- 相对轨迹图 ----

    const val TRACK_WIDTH = 1000
    const val TRACK_HEIGHT = 1000

    /** 相对轨迹图（PNG）：x/y 为相对首点（起飞点）的 E/N 米（等距圆柱投影，见 core/Geo）。 */
    fun renderTrack(points: List<TrackPoint>, width: Int = TRACK_WIDTH, height: Int = TRACK_HEIGHT): ByteArray {
        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        try {
            val canvas = Canvas(bmp)
            drawTrack(canvas, points, width, height)
            val out = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
            return out.toByteArray()
        } finally {
            bmp.recycle()
        }
    }

    private fun drawTrack(canvas: Canvas, points: List<TrackPoint>, width: Int, height: Int) {
        val padL = 110f
        val padR = 80f
        val padT = 90f
        val padB = 90f
        val plotW = width - padL - padR
        val plotH = height - padT - padB

        val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(224, 224, 224)
            style = Paint.Style.STROKE
            strokeWidth = 2f
        }
        val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(94, 53, 177)
            style = Paint.Style.STROKE
            strokeWidth = 6f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 28f
            color = Color.rgb(90, 90, 90)
        }
        val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 34f
            color = Color.rgb(60, 60, 60)
            textAlign = Paint.Align.CENTER
        }
        val markerFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
        val markerStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 4f
            color = Color.WHITE
        }

        if (points.isEmpty()) {
            titlePaint.textSize = 34f
            titlePaint.textAlign = Paint.Align.CENTER
            canvas.drawText("（无轨迹数据）", width / 2f, height / 2f, titlePaint)
            return
        }

        var nMin = Float.MAX_VALUE
        var nMax = Float.MIN_VALUE
        var eMin = Float.MAX_VALUE
        var eMax = Float.MIN_VALUE
        for (p in points) {
            nMin = minOf(nMin, p.relN.toFloat())
            nMax = maxOf(nMax, p.relN.toFloat())
            eMin = minOf(eMin, p.relE.toFloat())
            eMax = maxOf(eMax, p.relE.toFloat())
        }
        // 单点/零跨度时扩张 ±1m，避免除零
        if (nMax - nMin < 1f) { nMin -= 1f; nMax += 1f }
        if (eMax - eMin < 1f) { eMin -= 1f; eMax += 1f }
        // 留 8% 边距
        val nPad = (nMax - nMin) * 0.08f
        val ePad = (eMax - eMin) * 0.08f
        nMin -= nPad; nMax += nPad
        eMin -= ePad; eMax += ePad

        fun xOf(e: Double): Float = padL + ((e - eMin) / (eMax - eMin) * plotW).toFloat()
        fun yOf(n: Double): Float = padT + ((nMax - n) / (nMax - nMin) * plotH).toFloat()

        // ---- 网格 + N/E 轴刻度 ----
        labelPaint.textAlign = Paint.Align.RIGHT
        for (i in 0..3) {
            val v = nMin + (nMax - nMin) * i / 3
            val y = yOf(v.toDouble())
            canvas.drawLine(padL, y, padL + plotW, y, gridPaint)
            canvas.drawText(String.format(Locale.US, "%.0f", v), padL - 14f,
                y - (labelPaint.ascent() + labelPaint.descent()) / 2, labelPaint)
        }
        labelPaint.textAlign = Paint.Align.CENTER
        val yAxis = padT + plotH + 40f
        for (i in 0..3) {
            val v = eMin + (eMax - eMin) * i / 3
            val x = xOf(v.toDouble())
            canvas.drawLine(x, padT, x, padT + plotH, gridPaint)
            canvas.drawText(String.format(Locale.US, "%.0f", v), x, yAxis, labelPaint)
        }
        // 轴标题
        titlePaint.textSize = 30f
        titlePaint.textAlign = Paint.Align.CENTER
        canvas.drawText("东向 E（米）", padL + plotW / 2f, padT + plotH + 72f, titlePaint)
        canvas.save()
        canvas.rotate(-90f, 40f, padT + plotH / 2f)
        canvas.drawText("北向 N（米）", 40f, padT + plotH / 2f, titlePaint)
        canvas.restore()
        titlePaint.textSize = 34f
        canvas.drawText("相对轨迹（相对首点，米）", padL + plotW / 2f, padT - 30f, titlePaint)

        // ---- 轨迹折线 ----
        val path = Path()
        points.forEachIndexed { i, p ->
            val x = xOf(p.relE)
            val y = yOf(p.relN)
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        canvas.drawPath(path, trackPaint)

        // ---- 起点/终点标记 + 最远距离标注 ----
        val start = points.first()
        val end = points.last()
        val r = 14f
        markerFill.color = Color.rgb(46, 125, 50) // 起点绿
        canvas.drawCircle(xOf(start.relE), yOf(start.relN), r, markerFill)
        canvas.drawCircle(xOf(start.relE), yOf(start.relN), r, markerStroke)
        markerFill.color = Color.rgb(198, 40, 40) // 终点红
        canvas.drawCircle(xOf(end.relE), yOf(end.relN), r, markerFill)
        canvas.drawCircle(xOf(end.relE), yOf(end.relN), r, markerStroke)

        var maxDist = 0f
        for (p in points) maxDist = maxOf(maxDist, sqrt(p.relN * p.relN + p.relE * p.relE).toFloat())
        labelPaint.textAlign = Paint.Align.LEFT
        canvas.drawText(String.format(Locale.US, "最远距离 %.1f m", maxDist), padL, padT - 58f, labelPaint)

        // ---- 图例 ----
        val ly = padT + plotH + 8f
        markerFill.color = Color.rgb(46, 125, 50)
        canvas.drawCircle(padL + 14f, ly, 8f, markerFill)
        canvas.drawCircle(padL + 14f, ly, 8f, markerStroke)
        canvas.drawText("起点", padL + 32f, ly - (labelPaint.ascent() + labelPaint.descent()) / 2, labelPaint)
        markerFill.color = Color.rgb(198, 40, 40)
        val lx = padL + 32f + labelPaint.measureText("起点") + 40f
        canvas.drawCircle(lx + 14f, ly, 8f, markerFill)
        canvas.drawCircle(lx + 14f, ly, 8f, markerStroke)
        canvas.drawText("终点", lx + 32f, ly - (labelPaint.ascent() + labelPaint.descent()) / 2, labelPaint)
    }
}
