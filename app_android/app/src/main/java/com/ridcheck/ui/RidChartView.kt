package com.ridcheck.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.View
import com.ridcheck.core.SamplePoint
import java.util.Locale

/** 双轴曲线图：RSSI(dBm, 左轴) + 广播速率(包/s, 右轴) 随时间变化。0/1/2+ 样本均稳健。 */
class RidChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val samples = ArrayList<SamplePoint>()

    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFE0E0E0.toInt()
        style = Paint.Style.STROKE
        strokeWidth = dp(1).toFloat()
    }
    private val rssiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(27, 94, 32)
        style = Paint.Style.STROKE
        strokeWidth = dp(2).toFloat()
        strokeCap = Paint.Cap.ROUND
    }
    private val ratePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(230, 126, 34)
        style = Paint.Style.STROKE
        strokeWidth = dp(2).toFloat()
        strokeCap = Paint.Cap.ROUND
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = dp(10).toFloat()
        color = Color.rgb(90, 90, 90)
    }
    private val legendPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = dp(11).toFloat()
        color = Color.rgb(60, 60, 60)
    }

    fun setData(data: List<SamplePoint>) {
        samples.clear()
        samples.addAll(data)
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), dp(180))
    }

    override fun onDraw(canvas: Canvas) {
        if (samples.isEmpty()) {
            labelPaint.textAlign = Paint.Align.CENTER
            val y = (height - labelPaint.ascent() - labelPaint.descent()) / 2
            canvas.drawText("暂无采样数据（开始扫描后每秒采集）", width / 2f, y, labelPaint)
            return
        }

        val padL = dp(40).toFloat()
        val padR = dp(40).toFloat()
        val padT = dp(12).toFloat()
        val padB = dp(24).toFloat()
        val plotW = width - padL - padR
        val plotH = height - padT - padB

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
        val rateHi = maxOf((rateMax * 1.2).toFloat(), 1.0f)
        val t0 = samples.first().timeMs.toFloat()
        val t1 = samples.last().timeMs.toFloat()
        val tSpan = maxOf(t1 - t0, 1f)

        fun xOf(t: Float): Float = padL + (t - t0) / tSpan * plotW
        fun rssiY(v: Float): Float = padT + (rssiHi - v) / (rssiHi - rssiLo) * plotH
        fun rateY(v: Float): Float = padT + (rateHi - v) / rateHi * plotH

        // 网格 + RSSI 左轴刻度
        labelPaint.textAlign = Paint.Align.RIGHT
        for (i in 0..3) {
            val v = rssiLo + (rssiHi - rssiLo) * i / 3
            val y = rssiY(v)
            canvas.drawLine(padL, y, width - padR, y, gridPaint)
            canvas.drawText(
                String.format(Locale.US, "%.0f", v),
                padL - dp(4),
                y - (labelPaint.ascent() + labelPaint.descent()) / 2,
                labelPaint
            )
        }
        // 速率右轴刻度
        labelPaint.textAlign = Paint.Align.LEFT
        for (i in 0..3) {
            val v = rateHi * i / 3
            val y = rateY(v)
            canvas.drawText(
                String.format(Locale.US, "%.1f", v),
                width - padR + dp(4),
                y - (labelPaint.ascent() + labelPaint.descent()) / 2,
                labelPaint
            )
        }

        // 曲线
        if (samples.size == 1) {
            val s = samples.first()
            val x = xOf(s.timeMs.toFloat())
            canvas.drawCircle(x, rssiY(s.rssi.toFloat()), dp(3).toFloat(), rssiPaint)
            canvas.drawCircle(x, rateY(s.rateHz.toFloat()), dp(3).toFloat(), ratePaint)
        } else {
            val rssiPath = Path()
            val ratePath = Path()
            samples.forEachIndexed { i, s ->
                val x = xOf(s.timeMs.toFloat())
                val ry = rssiY(s.rssi.toFloat())
                val ty = rateY(s.rateHz.toFloat())
                if (i == 0) {
                    rssiPath.moveTo(x, ry)
                    ratePath.moveTo(x, ty)
                } else {
                    rssiPath.lineTo(x, ry)
                    ratePath.lineTo(x, ty)
                }
            }
            canvas.drawPath(rssiPath, rssiPaint)
            canvas.drawPath(ratePath, ratePaint)
        }

        // 图例
        val legendY = padT - dp(2)
        canvas.drawLine(padL, legendY, padL + dp(14), legendY, rssiPaint)
        legendPaint.textAlign = Paint.Align.LEFT
        val l1 = padL + dp(18)
        canvas.drawText(
            "RSSI (dBm)", l1,
            legendY - (legendPaint.ascent() + legendPaint.descent()) / 2, legendPaint
        )
        val l2 = l1 + legendPaint.measureText("RSSI (dBm)") + dp(14)
        canvas.drawLine(l2, legendY, l2 + dp(14), legendY, ratePaint)
        canvas.drawText(
            "速率 (包/s)", l2 + dp(18),
            legendY - (legendPaint.ascent() + legendPaint.descent()) / 2, legendPaint
        )

        // 时间轴
        labelPaint.textAlign = Paint.Align.CENTER
        val sec0 = t0 / 1000f
        val sec1 = t1 / 1000f
        val y = padT + plotH + dp(14)
        canvas.drawText(mmss(sec0), padL, y, labelPaint)
        canvas.drawText(mmss((sec0 + sec1) / 2), padL + plotW / 2f, y, labelPaint)
        canvas.drawText(mmss(sec1), width - padR, y, labelPaint)
    }

    private fun mmss(sec: Float): String {
        val s = sec.toInt()
        return String.format(Locale.US, "%d:%02d", s / 60, s % 60)
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
