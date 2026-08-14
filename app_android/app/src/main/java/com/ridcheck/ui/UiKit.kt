package com.ridcheck.ui

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/** 通用 UI 助手：按钮/小节标题/布局参数/dp/圆角背景（各页共享，避免重复定义）。 */

fun Context.button(text: String): Button = Button(this).apply {
    this.text = text
    isAllCaps = false
}

fun Context.sectionLabel(text: String): TextView = TextView(this).apply {
    this.text = text
    textSize = 14f
    setTypeface(null, Typeface.BOLD)
    setTextColor(Theme.PRIMARY)
    setPadding(0, dp(10), 0, dp(2))
}

fun Context.lpWeight(w: Float): LinearLayout.LayoutParams =
    LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w)

fun Context.lpMatch(): LinearLayout.LayoutParams =
    LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

fun Context.lpFill(): FrameLayout.LayoutParams =
    FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)

fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

fun Context.roundedRect(color: Int): GradientDrawable =
    GradientDrawable().apply {
        cornerRadius = dp(12).toFloat()
        setColor(color)
    }
