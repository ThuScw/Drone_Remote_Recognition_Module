package com.ridcheck.ui

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.LinearLayout
import android.widget.TextView
import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceEntry
import com.ridcheck.core.HealthLevel

/**
 * 设备列表行 adapter。
 * 每行：健康级别● + 设备地址 / RSSI + 运行状态 + 最后更新时间。
 * devices 用 lambda 惰性读取，避免持有过期引用。
 */
class DeviceListAdapter(
    private val context: Context,
    private val devices: () -> List<DeviceEntry>
) : BaseAdapter() {

    override fun getCount(): Int = devices().size

    override fun getItem(position: Int): DeviceEntry = devices()[position]

    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val entry = getItem(position)
        val row = (convertView as? LinearLayout) ?: buildRow()
        row.removeAllViews()

        val now = System.currentTimeMillis()
        val level = entry.assessor.report().level

        val top = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        top.addView(TextView(context).apply {
            text = "● " + level.label()
            setTextColor(colorOf(level))
            setTypeface(null, Typeface.BOLD)
            textSize = 14f
        })
        top.addView(TextView(context).apply {
            text = entry.address
            setTextColor(Color.rgb(30, 30, 30))
            setTypeface(null, Typeface.BOLD)
            textSize = 14f
            setPadding(dp(8), 0, 0, 0)
        })

        val opStatus = entry.lastPkt?.opStatus
        val opText = if (opStatus != null) Decoder.OP_STATUS[opStatus] ?: "无效($opStatus)" else "-"
        val bottom = TextView(context).apply {
            text = "${entry.rssi}dBm | $opText | ${agoText(now - entry.lastSeenMs)}"
            setTextColor(Color.rgb(102, 102, 102))
            textSize = 12f
        }

        row.addView(top)
        row.addView(bottom)
        return row
    }

    private fun buildRow(): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        background = GradientDrawable().apply {
            cornerRadius = dp(12).toFloat()
            setColor(Color.rgb(245, 246, 248))
        }
        val lp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        )
        lp.bottomMargin = dp(6)
        layoutParams = lp
        setPadding(dp(14), dp(10), dp(14), dp(10))
    }

    private fun colorOf(level: HealthLevel): Int = when (level) {
        HealthLevel.PASS -> Color.rgb(27, 94, 32)
        HealthLevel.WARN -> Color.rgb(178, 106, 0)
        HealthLevel.FAIL -> Color.rgb(183, 28, 28)
    }

    private fun agoText(diffMs: Long): String = when {
        diffMs < 1000 -> "刚刚"
        diffMs < 60_000 -> "${diffMs / 1000}s前"
        else -> "${diffMs / 60_000}min前"
    }

    private fun dp(v: Int): Int = (v * context.resources.displayMetrics.density).toInt()
}
