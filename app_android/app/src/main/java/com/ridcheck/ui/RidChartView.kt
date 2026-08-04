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

/**
 * 双轴曲线图：RSSI(dBm, 左轴, 紫) + 广播速率(包/s, 右轴, 橙) 随时间变化。
 * - 密集样本按像素列聚合为包络(min/max)再连中点，避免相邻秒级采样在亚像素横距上拉出竖直长线；
 * - 横轴为"距首采 mm:ss"，左/右轴标题用对应系列色标注归属。
 */
class RidChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val samples = ArrayList<SamplePoint>()

    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(224, 224, 224)
        style = Paint.Style.STROKE
        strokeWidth = dp(1).toFloat()
    }
    private val rssiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.PRIMARY
        style = Paint.Style.STROKE
        strokeWidth = dp(2).toFloat()
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val ratePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.RATE
        style = Paint.Style.STROKE
        strokeWidth = dp(2).toFloat()
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val rssiRangePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(80, 94, 53, 177)
        style = Paint.Style.STROKE
        strokeWidth = dp(1).toFloat()
    }
    private val rateRangePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(80, 230, 126, 34)
        style = Paint.Style.STROKE
        strokeWidth = dp(1).toFloat()
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = dp(10).toFloat()
        color = Color.rgb(90, 90, 90)
    }
    private val legendPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = dp(11).toFloat()
        color = Color.rgb(60, 60, 60)
    }
    private val axisTitlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = dp(10).toFloat()
        color = Theme.PRIMARY
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

        val padL = dp(46).toFloat()
        val padR = dp(46).toFloat()
        val padT = dp(24).toFloat()
        val padB = dp(32).toFloat()
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
        // 时间横轴用 Long 差值算，避免 epoch 毫秒（~1.78e12）转 Float 丢精度：
        // 该量级下 Float 步长约 131s，窗口一超 2 分钟 t0/t1 就停在量化格上 → 折线冻结成直线
        val t0Ms = samples.first().timeMs
        val t1Ms = samples.last().timeMs
        val spanMs = t1Ms - t0Ms
        val sameMs = spanMs <= 0

        fun xOf(idx: Int, tMs: Long): Float =
            when {
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
            canvas.drawLine(padL, y, width - padR, y, gridPaint)
            canvas.drawText(
                String.format(Locale.US, "%.0f", v),
                padL - dp(5),
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
                width - padR + dp(5),
                y - (labelPaint.ascent() + labelPaint.descent()) / 2,
                labelPaint
            )
        }

        // ---- 曲线：按像素列聚合包络，画范围细线 + 中点连线（消除亚像素竖直长线） ----
        val cols = maxOf(2, plotW.toInt()) // ≥2，避免 (cols-1) 除零
        val rssiLoCol = FloatArray(cols) { Float.POSITIVE_INFINITY }
        val rssiHiCol = FloatArray(cols) { Float.NEGATIVE_INFINITY }
        val rateLoCol = FloatArray(cols) { Float.POSITIVE_INFINITY }
        val rateHiCol = FloatArray(cols) { Float.NEGATIVE_INFINITY }
        samples.forEachIndexed { i, s ->
            val x = xOf(i, s.timeMs)
            val ci = ((x - padL) / plotW * (cols - 1)).toInt().coerceIn(0, cols - 1)
            val ry = rssiY(s.rssi.toFloat())
            val ty = rateY(s.rateHz.toFloat())
            if (ry < rssiLoCol[ci]) rssiLoCol[ci] = ry
            if (ry > rssiHiCol[ci]) rssiHiCol[ci] = ry
            if (ty < rateLoCol[ci]) rateLoCol[ci] = ty
            if (ty > rateHiCol[ci]) rateHiCol[ci] = ty
        }
        val rssiPath = Path()
        val ratePath = Path()
        var rssiStarted = false
        var rateStarted = false
        for (ci in 0 until cols) {
            if (rssiLoCol[ci].isInfinite()) continue
            val x = padL + ci.toFloat() / (cols - 1) * plotW
            canvas.drawLine(x, rssiLoCol[ci], x, rssiHiCol[ci], rssiRangePaint)
            canvas.drawLine(x, rateLoCol[ci], x, rateHiCol[ci], rateRangePaint)
            val rMid = (rssiLoCol[ci] + rssiHiCol[ci]) / 2f
            val tMid = (rateLoCol[ci] + rateHiCol[ci]) / 2f
            if (!rssiStarted) {
                rssiPath.moveTo(x, rMid)
                rssiStarted = true
            } else {
                rssiPath.lineTo(x, rMid)
            }
            if (!rateStarted) {
                ratePath.moveTo(x, tMid)
                rateStarted = true
            } else {
                ratePath.lineTo(x, tMid)
            }
        }
        canvas.drawPath(rssiPath, rssiPaint)
        canvas.drawPath(ratePath, ratePaint)

        // ---- 图例（顶部，颜色对应） ----
        val legendY = padT - dp(4)
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

        // ---- 旋转轴标题（左 RSSI 紫 / 右速率橙，指明纵轴归属） ----
        axisTitlePaint.textSize = dp(10).toFloat()
        axisTitlePaint.color = Theme.PRIMARY
        canvas.save()
        canvas.translate(dp(15).toFloat(), padT + plotH / 2f)
        canvas.rotate(-90f)
        axisTitlePaint.textAlign = Paint.Align.CENTER
        canvas.drawText("RSSI (dBm)", 0f, -(axisTitlePaint.ascent() + axisTitlePaint.descent()) / 2, axisTitlePaint)
        canvas.restore()
        axisTitlePaint.color = Theme.RATE
        canvas.save()
        canvas.translate(width - dp(15).toFloat(), padT + plotH / 2f)
        canvas.rotate(90f)
        canvas.drawText("速率 (包/s)", 0f, -(axisTitlePaint.ascent() + axisTitlePaint.descent()) / 2, axisTitlePaint)
        canvas.restore()

        // ---- 时间轴：距首采 mm:ss（3 个标签）+ 轴说明 ----
        labelPaint.textAlign = Paint.Align.CENTER
        val yTicks = padT + plotH + dp(16)
        val midMs = t0Ms + spanMs / 2
        canvas.drawText(mmss(0f), padL, yTicks, labelPaint)
        canvas.drawText(mmss((midMs - t0Ms) / 1000f), padL + plotW / 2f, yTicks, labelPaint)
        canvas.drawText(mmss((t1Ms - t0Ms) / 1000f), width - padR, yTicks, labelPaint)
        canvas.drawText(
            "时间（距首采 mm:ss）", padL + plotW / 2f, padT + plotH + dp(30),
            labelPaint
        )
    }

    private fun mmss(sec: Float): String {
        val s = sec.toInt()
        return String.format(Locale.US, "%d:%02d", s / 60, s % 60)
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
