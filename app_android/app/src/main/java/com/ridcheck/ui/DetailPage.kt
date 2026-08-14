package com.ridcheck.ui

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.ridcheck.core.AppState
import com.ridcheck.core.DecodedPacket
import com.ridcheck.core.DeviceEntry
import com.ridcheck.core.Health
import com.ridcheck.core.HealthLevel
import com.ridcheck.core.toHexSpaced
import java.util.Locale

/** 设备详情页：字段 / 问题清单 / 判定 / 记录与曲线 / 原始数据，附复制/分享/导出/报告。 */
class DetailPage(
    private val activity: Activity,
    private val onBack: () -> Unit
) {

    companion object {
        const val MANUAL_ADDRESS = "手动"

        private val C_VERDICT_BG = mapOf(
            HealthLevel.PASS to Theme.PASS_BG,
            HealthLevel.WARN to Theme.WARN_BG,
            HealthLevel.FAIL to Theme.FAIL_BG
        )
        private val C_VERDICT_FG = mapOf(
            HealthLevel.PASS to Theme.PASS,
            HealthLevel.WARN to Theme.WARN,
            HealthLevel.FAIL to Theme.FAIL
        )
        private val C_ISSUE = mapOf(
            HealthLevel.PASS to Theme.PASS,
            HealthLevel.WARN to Theme.WARN,
            HealthLevel.FAIL to Theme.FAIL
        )
    }

    val root: ScrollView

    private lateinit var txtDetailTitle: TextView
    private lateinit var txtManualBanner: TextView
    private lateinit var txtVerdict: TextView
    private lateinit var txtIssues: TextView
    private lateinit var txtFields: TextView
    private lateinit var txtRaw: TextView
    private lateinit var recordSection: LinearLayout
    private lateinit var chartView: RidChartView

    // 当前展示对象：address（BLE 实时）或 MANUAL_ADDRESS + manualPkt（粘贴单包）
    private var currentAddress: String? = null
    private var manualPkt: DecodedPacket? = null

    val isShowing: Boolean get() = root.visibility == View.VISIBLE
    private val isManual: Boolean get() = currentAddress == MANUAL_ADDRESS

    init {
        root = build()
    }

    fun showDevice(address: String) {
        currentAddress = address
        manualPkt = null
        render()
    }

    fun showManual(pkt: DecodedPacket) {
        currentAddress = MANUAL_ADDRESS
        manualPkt = pkt
        render()
    }

    fun clear() {
        currentAddress = null
        manualPkt = null
    }

    // --- UiKit 委托（本类非 Context 子类，经 activity 调用） ---
    private fun button(text: String): Button = activity.button(text)

    private fun sectionLabel(text: String): TextView = activity.sectionLabel(text)

    private fun lpWeight(w: Float): LinearLayout.LayoutParams = activity.lpWeight(w)

    private fun lpMatch(): LinearLayout.LayoutParams = activity.lpMatch()

    private fun lpFill(): FrameLayout.LayoutParams = activity.lpFill()

    private fun dp(v: Int): Int = activity.dp(v)

    private fun roundedRect(color: Int): GradientDrawable = activity.roundedRect(color)

    /** 每秒由主界面 ticker 调用，从注册表实时重绘当前设备。 */
    fun render() {
        val entry = if (isManual) null
            else AppState.registry.list.firstOrNull { it.address == currentAddress }
        val pkt = if (isManual) manualPkt else entry?.lastPkt
        if (pkt == null) return

        txtDetailTitle.text = buildString {
            append(if (isManual) "粘贴解码" else currentAddress ?: "")
            val rssi = entry?.rssi ?: pkt.rssi
            if (rssi != 0) append("  ($rssi dBm)")
        }
        txtManualBanner.visibility = if (isManual) View.VISIBLE else View.GONE

        txtRaw.text = buildRawText(pkt)

        val sb = StringBuilder()
        if (pkt.structureError.isNotEmpty()) {
            sb.append("结构错误: ").append(pkt.structureError).append('\n')
        }
        for ((k, v) in pkt.fmt) {
            sb.append(k).append(": ").append(v).append('\n')
        }
        txtFields.text = sb.toString()

        val report = if (entry != null) entry.assessor.report() else Health.manualReport(pkt)
        val extra = if (!isManual && report.packetsSeen > 0) {
            String.format(Locale.US, "   速率 %.1f 包/s", report.avgRateHz)
        } else {
            ""
        }
        txtVerdict.text = "${report.level.verdictLabel()}  ${report.note}$extra"
        txtVerdict.background = roundedRect(C_VERDICT_BG[report.level] ?: Color.WHITE)
        txtVerdict.setTextColor(C_VERDICT_FG[report.level] ?: Color.BLACK)

        txtIssues.text = if (report.issues.isEmpty()) {
            "未发现问题"
        } else {
            report.issues.joinToString("\n") { i ->
                val clause = if (i.clause.isEmpty()) "" else " (${i.clause})"
                "[${i.level.label()}] ${i.code}$clause: ${i.message}"
            }
        }
        txtIssues.setTextColor(
            if (report.issues.isEmpty()) Theme.PASS
            else C_ISSUE[report.issues.maxOfOrNull { it.level }] ?: Color.DKGRAY
        )

        if (isManual) {
            recordSection.visibility = View.GONE
        } else {
            recordSection.visibility = View.VISIBLE
            chartView.setData(entry?.samples ?: emptyList())
        }
    }

    // ------------------------------------------------------------------ UI
    private fun build(): ScrollView {
        val scroll = ScrollView(activity)
        val col = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(8), dp(12), dp(24))
        }
        scroll.addView(col, lpFill())

        val topRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        col.addView(topRow)

        val btnBack = button("← 返回列表")
        btnBack.setOnClickListener { onBack() }
        topRow.addView(btnBack)

        txtDetailTitle = TextView(activity).apply {
            textSize = 16f
            setTypeface(null, Typeface.BOLD)
            setTextColor(Theme.PRIMARY)
            setPadding(dp(10), 0, 0, 0)
            gravity = Gravity.CENTER_VERTICAL
        }
        topRow.addView(txtDetailTitle, lpWeight(1f))

        txtManualBanner = TextView(activity).apply {
            text = "单包静态判定（不含流式统计）"
            textSize = 12f
            setTextColor(Theme.WARN)
            setPadding(dp(10), dp(6), dp(10), dp(6))
            visibility = View.GONE
        }
        col.addView(txtManualBanner, lpMatch())

        txtVerdict = TextView(activity).apply {
            setTextColor(Color.BLACK)
            setPadding(dp(10), dp(10), dp(10), dp(10))
            textSize = 15f
        }
        col.addView(txtVerdict, lpMatch())

        col.addView(sectionLabel("问题清单"))
        txtIssues = TextView(activity).apply {
            textSize = 14f
            setTextColor(Color.DKGRAY)
        }
        col.addView(txtIssues, lpMatch())

        col.addView(sectionLabel("最新数据包"))
        txtFields = TextView(activity).apply {
            textSize = 13f
            setTextColor(Color.DKGRAY)
        }
        col.addView(txtFields, lpMatch())

        recordSection = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        recordSection.addView(sectionLabel("记录与分析"))
        chartView = RidChartView(activity)
        recordSection.addView(chartView, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(180)
        ))
        val actRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(8), 0, 0)
        }
        val btnReport = button("生成报告")
        btnReport.setOnClickListener { shareDeviceReport() }
        actRow.addView(btnReport, lpWeight(1f))
        val btnExport = button("导出数据")
        btnExport.setOnClickListener { exportCsv() }
        actRow.addView(btnExport, lpWeight(1f))
        recordSection.addView(actRow)
        recordSection.visibility = View.GONE
        col.addView(recordSection, lpMatch())

        col.addView(sectionLabel("原始数据"))
        val rawRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        col.addView(rawRow)
        val btnCopy = button("复制")
        btnCopy.setOnClickListener { copyCurrentRaw() }
        rawRow.addView(btnCopy)
        val btnShare = button("分享")
        btnShare.setOnClickListener { shareCurrentRaw() }
        rawRow.addView(btnShare)
        txtRaw = TextView(activity).apply {
            textSize = 12f
            setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
            setTextColor(Color.BLACK)
            setOnLongClickListener {
                copyCurrentRaw()
                true
            }
        }
        col.addView(txtRaw, lpMatch())

        return scroll
    }

    // ------------------------------------------------------- copy / share
    private fun buildRawText(pkt: DecodedPacket): String =
        "${pkt.address}\n" + pkt.raw.toHexSpaced()

    /** 复制/分享仅对手动粘贴包可用（BLE 实时包在 txtRaw 已可见，长按复制同源）。 */
    private fun copyCurrentRaw() {
        val pkt = manualPkt ?: return
        val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("RID 原始数据", buildRawText(pkt)))
        Toast.makeText(activity, "已复制", Toast.LENGTH_SHORT).show()
        AppState.addLog("已复制原始数据")
    }

    private fun shareCurrentRaw() {
        val pkt = manualPkt ?: return
        ShareUtil.shareText(activity, "RID 原始数据", buildRawText(pkt))
    }

    private fun shareDeviceReport() {
        currentEntry()?.let { ShareUtil.shareReport(activity, it) }
    }

    private fun exportCsv() {
        currentEntry()?.let { ShareUtil.shareCsv(activity, it) }
    }

    private fun currentEntry(): DeviceEntry? {
        val addr = currentAddress ?: return null
        if (addr == MANUAL_ADDRESS) return null
        return AppState.registry.list.firstOrNull { it.address == addr }
    }
}
