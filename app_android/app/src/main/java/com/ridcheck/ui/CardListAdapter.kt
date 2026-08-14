package com.ridcheck.ui

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.LinearLayout
import android.widget.TextView
import com.ridcheck.core.HealthLevel

/**
 * 卡片式列表行公共基类：圆角卡片、顶部 ●级别+地址、底部副行 + 相对时间。
 * 设备列表与可配置模块列表复用骨架，子类只需实现 getView。
 */
abstract class CardListAdapter<T>(
    protected val context: Context,
    protected val items: () -> List<T>
) : BaseAdapter() {

    override fun getCount(): Int = items().size

    override fun getItem(position: Int): T = items()[position]

    override fun getItemId(position: Int): Long = position.toLong()

    protected fun buildRow(bg: Int = Theme.CARD_BG): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        background = GradientDrawable().apply {
            cornerRadius = dp(12).toFloat()
            setColor(bg)
        }
        val lp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        )
        lp.bottomMargin = dp(6)
        layoutParams = lp
        setPadding(dp(14), dp(10), dp(14), dp(10))
    }

    /** 顶部行：● 级别点（null 时用主色实心点）+ 地址。 */
    protected fun topLine(level: HealthLevel?, address: String): LinearLayout =
        LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }.apply {
            addView(TextView(context).apply {
                text = if (level != null) "● " + level.label() else "●"
                setTextColor(if (level != null) colorOf(level) else Theme.PRIMARY)
                setTypeface(null, Typeface.BOLD)
                textSize = 14f
            })
            addView(TextView(context).apply {
                text = address
                setTextColor(Theme.TEXT_DARK)
                setTypeface(null, Typeface.BOLD)
                textSize = 14f
                setPadding(dp(8), 0, 0, 0)
            })
        }

    protected fun bottomLine(text: String): TextView = TextView(context).apply {
        this.text = text
        setTextColor(Theme.TEXT_MUTED)
        textSize = 12f
    }

    protected fun agoText(diffMs: Long): String = when {
        diffMs < 1000 -> "刚刚"
        diffMs < 60_000 -> "${diffMs / 1000}s前"
        else -> "${diffMs / 60_000}min前"
    }

    protected fun colorOf(level: HealthLevel): Int = when (level) {
        HealthLevel.PASS -> Theme.PASS
        HealthLevel.WARN -> Theme.WARN
        HealthLevel.FAIL -> Theme.FAIL
    }

    protected fun dp(v: Int): Int = context.dp(v)
}
