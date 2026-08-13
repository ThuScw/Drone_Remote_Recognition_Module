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
import com.ridcheck.core.ConfigDevice

/**
 * 可 GATT 配置模块列表行 adapter。
 * 每行：● + 设备地址 / RSSI + 广播名 + 最后更新时间。
 * devices 用 lambda 惰性读取，避免持有过期引用。
 */
class ConfigDeviceAdapter(
    private val context: Context,
    private val devices: () -> List<ConfigDevice>
) : BaseAdapter() {

    override fun getCount(): Int = devices().size

    override fun getItem(position: Int): ConfigDevice = devices()[position]

    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val entry = getItem(position)
        val row = (convertView as? LinearLayout) ?: buildRow()
        row.removeAllViews()

        val top = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        top.addView(TextView(context).apply {
            text = "● "
            setTextColor(Theme.PRIMARY)
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

        val bottom = TextView(context).apply {
            val name = entry.name ?: "(无)"
            text = "${entry.rssi}dBm | $name | ${agoText(System.currentTimeMillis() - entry.lastSeenMs)}"
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
            setColor(Theme.PRIMARY_SOFT)
        }
        val lp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        )
        lp.bottomMargin = dp(6)
        layoutParams = lp
        setPadding(dp(14), dp(10), dp(14), dp(10))
    }

    private fun agoText(diffMs: Long): String = when {
        diffMs < 1000 -> "刚刚"
        diffMs < 60_000 -> "${diffMs / 1000}s前"
        else -> "${diffMs / 60_000}min前"
    }

    private fun dp(v: Int): Int = (v * context.resources.displayMetrics.density).toInt()
}
