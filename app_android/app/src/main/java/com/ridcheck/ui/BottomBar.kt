package com.ridcheck.ui

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Process
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import com.ridcheck.ble.RidScanService
import com.ridcheck.core.AppState

/** 底部导航：主界面 / 说明 / 退出（红色加粗，点击确认后彻底关闭程序）。 */
class BottomBar(
    private val activity: Activity,
    private val onMain: () -> Unit,
    private val onExplain: () -> Unit
) {
    enum class Tab { MAIN, EXPLAIN }

    val root: LinearLayout

    private lateinit var tabMain: LinearLayout
    private lateinit var tabExplain: LinearLayout
    private lateinit var tabMainStripe: View
    private lateinit var tabExplainStripe: View
    private lateinit var tabMainText: TextView
    private lateinit var tabExplainText: TextView

    init {
        root = build()
    }

    fun setActive(active: Tab) {
        setTabState(tabMainStripe, tabMainText, active == Tab.MAIN)
        setTabState(tabExplainStripe, tabExplainText, active == Tab.EXPLAIN)
    }

    private fun build(): LinearLayout {
        val bar = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.rgb(250, 250, 250))
            setElevation(dp(8).toFloat())
        }

        tabMain = buildTab(Tab.MAIN, "主界面", onMain)
        tabExplain = buildTab(Tab.EXPLAIN, "说明", onExplain)
        val btnQuit = buildQuitButton()
        bar.addView(tabMain, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        bar.addView(tabExplain, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        bar.addView(btnQuit, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        setActive(Tab.MAIN)
        return bar
    }

    /** 每个 tab = 顶部 3dp 色条 + 居中文字，点击切换。 */
    private fun buildTab(tab: Tab, label: String, onClick: () -> Unit): LinearLayout {
        val container = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val stripe = View(activity).apply { setBackgroundColor(Theme.PRIMARY) }
        container.addView(stripe, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(3)
        ))
        val text = TextView(activity).apply {
            this.text = label
            textSize = 13f
            gravity = Gravity.CENTER
        }
        container.addView(text, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
        ))
        container.setOnClickListener { onClick() }
        when (tab) {
            Tab.MAIN -> {
                tabMain = container; tabMainStripe = stripe; tabMainText = text
            }
            Tab.EXPLAIN -> {
                tabExplain = container; tabExplainStripe = stripe; tabExplainText = text
            }
        }
        return container
    }

    private fun setTabState(stripe: View, text: TextView, active: Boolean) {
        stripe.visibility = if (active) View.VISIBLE else View.GONE
        text.setTextColor(if (active) Theme.PRIMARY else Theme.TEXT_INACTIVE)
        text.setTypeface(null, if (active) Typeface.BOLD else Typeface.NORMAL)
    }

    /** 底部栏「退出」键：红色加粗，与导航 tab 区分；点击弹出确认后彻底关闭程序。 */
    private fun buildQuitButton(): TextView = TextView(activity).apply {
        text = "退出"
        textSize = 13f
        setTextColor(Theme.FAIL)
        setTypeface(null, Typeface.BOLD)
        gravity = Gravity.CENTER
        setOnClickListener { confirmQuit() }
    }

    private fun confirmQuit() {
        AlertDialog.Builder(activity)
            .setTitle("退出程序")
            .setMessage("确定要完全关闭吗？将停止后台扫描、结束本次记录并退出程序。")
            .setPositiveButton("退出") { _, _ -> quitApp() }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 先同步停掉前台服务（onDestroy 内会停扫描并关闭问题时段），再关掉全部界面并结束进程，
     *  保证彻底退出，START_STICKY 服务不会复活。 */
    private fun quitApp() {
        activity.stopService(Intent(activity, RidScanService::class.java))
        AppState.scanning = false
        activity.finishAffinity()
        Process.killProcess(Process.myPid())
    }

    private fun dp(v: Int): Int = activity.dp(v)
}
