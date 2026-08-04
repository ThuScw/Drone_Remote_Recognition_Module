package com.ridcheck

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ScrollView
import android.widget.TextView
import com.ridcheck.ble.BleScanner
import com.ridcheck.core.DecodedPacket
import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceRegistry
import com.ridcheck.core.Health
import com.ridcheck.core.HealthLevel
import com.ridcheck.core.HealthReport
import com.ridcheck.ui.DeviceListAdapter
import java.util.Locale

/**
 * 安卓版 RID 检测工具：设备列表 + 详情页（自用，界面从简）。
 * 主界面列出所有广播 UUID 0x0D50 的信号源，点进某台设备实时查看判定/字段/原始数据。
 */
class MainActivity : Activity() {

    companion object {
        private const val REQ_PERMISSIONS = 1001
        private const val REQ_BT_ENABLE = 1002
        private const val MAX_LOG_LINES = 200
        private const val MANUAL_ADDRESS = "手动"

        private val C_VERDICT_BG = mapOf(
            HealthLevel.PASS to Color.rgb(232, 245, 233),
            HealthLevel.WARN to Color.rgb(255, 248, 225),
            HealthLevel.FAIL to Color.rgb(255, 235, 238)
        )
        private val C_VERDICT_FG = mapOf(
            HealthLevel.PASS to Color.rgb(27, 94, 32),
            HealthLevel.WARN to Color.rgb(178, 106, 0),
            HealthLevel.FAIL to Color.rgb(183, 28, 28)
        )
        private val C_ISSUE = mapOf(
            HealthLevel.PASS to Color.rgb(27, 94, 32),
            HealthLevel.WARN to Color.rgb(178, 106, 0),
            HealthLevel.FAIL to Color.rgb(183, 28, 28)
        )
    }

    private val registry = DeviceRegistry()
    private var scanner: BleScanner? = null
    private var pendingStart = false

    /** 当前详情页关联的设备；null = 停留在列表。手动粘贴时为 MANUAL_ADDRESS。 */
    private var currentDetailAddress: String? = null
    private var currentDetailPkt: DecodedPacket? = null

    // UI
    private lateinit var listRoot: LinearLayout
    private lateinit var detailRoot: ScrollView
    private lateinit var adapter: DeviceListAdapter
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var txtScanState: TextView
    private lateinit var txtCount: TextView
    private lateinit var txtLog: TextView
    private lateinit var txtDetailTitle: TextView
    private lateinit var txtVerdict: TextView
    private lateinit var txtIssues: TextView
    private lateinit var txtFields: TextView
    private lateinit var txtRaw: TextView

    /** 每秒刷新列表行与详情（更新时间、STALE 判定随时间变化）。 */
    private val ticker = Handler(Looper.getMainLooper())
    private val tickRunnable = object : Runnable {
        override fun run() {
            adapter.notifyDataSetChanged()
            if (currentDetailAddress != null) renderDetail()
            ticker.postDelayed(this, 1000)
        }
    }

    private val bleListener = object : BleScanner.Listener {
        override fun onDistinctPacket(address: String, rssi: Int, raw: ByteArray) {
            val pkt = Decoder.decodeGbPacket(
                raw,
                address = address,
                rssi = rssi,
                receivedAtMs = System.nanoTime() / 1_000_000,
                source = "ble"
            )
            onPacket(pkt)
        }

        override fun onScanState(scanning: Boolean) {
            btnStart.isEnabled = !scanning
            btnStop.isEnabled = scanning
            txtScanState.text = if (scanning) "● 扫描中" else "● 已停止"
            txtScanState.setTextColor(
                if (scanning) Color.rgb(27, 94, 32) else Color.rgb(102, 102, 102)
            )
        }

        override fun onLog(text: String) {
            log(text)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        log("就绪。点击“开始扫描”收集所有 RID 模块，点设备进详情；或粘贴 nRF Connect 抓到的 HEX。")
    }

    override fun onResume() {
        super.onResume()
        ticker.post(tickRunnable)
    }

    override fun onPause() {
        super.onPause()
        ticker.removeCallbacks(tickRunnable)
        scanner?.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        scanner?.stop()
    }

    override fun onBackPressed() {
        if (detailRoot.visibility == View.VISIBLE) {
            showList()
        } else {
            super.onBackPressed()
        }
    }

    // ------------------------------------------------------------------ UI
    private fun buildUi() {
        val root = FrameLayout(this)

        listRoot = buildListPage()
        detailRoot = buildDetailPage()
        root.addView(listRoot, lpFill())
        root.addView(detailRoot, lpFill())
        detailRoot.visibility = View.GONE

        setContentView(root)
    }

    private fun buildListPage(): LinearLayout {
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        col.setPadding(dp(12), dp(8), dp(12), dp(8))

        val title = TextView(this)
        title.text = "RID 检测（GB 46750-2025）"
        title.textSize = 18f
        title.setTypeface(null, Typeface.BOLD)
        title.setTextColor(Color.rgb(27, 94, 32))
        col.addView(title)

        val btnRow = LinearLayout(this)
        btnRow.orientation = LinearLayout.HORIZONTAL
        btnRow.setPadding(0, dp(8), 0, dp(8))
        col.addView(btnRow)

        btnStart = button("开始扫描")
        btnStart.setOnClickListener { ensurePermissionsThenScan() }
        btnRow.addView(btnStart, lpWeight(1f))

        btnStop = button("停止扫描")
        btnStop.isEnabled = false
        btnStop.setOnClickListener { scanner?.stop() }
        btnRow.addView(btnStop, lpWeight(1f))

        val btnPaste = button("粘贴HEX")
        btnPaste.setOnClickListener { showPasteDialog() }
        btnRow.addView(btnPaste, lpWeight(1f))

        val statusRow = LinearLayout(this)
        statusRow.orientation = LinearLayout.HORIZONTAL
        statusRow.setPadding(0, dp(4), 0, dp(4))
        col.addView(statusRow)

        txtScanState = TextView(this)
        txtScanState.text = "● 已停止"
        txtScanState.setTextColor(Color.rgb(102, 102, 102))
        txtScanState.setTypeface(null, Typeface.BOLD)
        statusRow.addView(txtScanState, lpWeight(1f))

        txtCount = TextView(this)
        txtCount.text = "已发现 0 台设备"
        txtCount.gravity = Gravity.END
        statusRow.addView(txtCount)

        col.addView(sectionLabel("信号源（点击查看详情）"))

        adapter = DeviceListAdapter(this) { registry.list }
        val listView = ListView(this)
        listView.adapter = adapter
        listView.setOnItemClickListener { _, _, position, _ ->
            showDetail(adapter.getItem(position).address)
        }
        // 垂直 LinearLayout 中 weight 只作用于高度；宽度必须 MATCH_PARENT，
        // 否则列表宽 0px 完全不可见（lpWeight 的 width=0 只适合水平行）
        col.addView(listView, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        col.addView(sectionLabel("日志"))
        val logScroll = ScrollView(this)
        txtLog = TextView(this)
        txtLog.textSize = 12f
        txtLog.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
        txtLog.setTextColor(Color.rgb(80, 80, 80))
        logScroll.addView(txtLog, lpFill())
        col.addView(logScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(150)))

        return col
    }

    private fun buildDetailPage(): ScrollView {
        val scroll = ScrollView(this)
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        col.setPadding(dp(12), dp(8), dp(12), dp(24))
        scroll.addView(col, lpFill())

        val topRow = LinearLayout(this)
        topRow.orientation = LinearLayout.HORIZONTAL
        col.addView(topRow)

        val btnBack = button("← 返回列表")
        btnBack.setOnClickListener { showList() }
        topRow.addView(btnBack)

        txtDetailTitle = TextView(this)
        txtDetailTitle.textSize = 16f
        txtDetailTitle.setTypeface(null, Typeface.BOLD)
        txtDetailTitle.setTextColor(Color.rgb(27, 94, 32))
        txtDetailTitle.setPadding(dp(10), 0, 0, 0)
        txtDetailTitle.gravity = Gravity.CENTER_VERTICAL
        topRow.addView(txtDetailTitle, lpWeight(1f))

        txtVerdict = TextView(this)
        txtVerdict.setTextColor(Color.BLACK)
        txtVerdict.setPadding(dp(10), dp(10), dp(10), dp(10))
        txtVerdict.textSize = 15f
        col.addView(txtVerdict, lpMatch())

        col.addView(sectionLabel("问题清单"))
        txtIssues = TextView(this)
        txtIssues.textSize = 14f
        txtIssues.setTextColor(Color.DKGRAY)
        col.addView(txtIssues, lpMatch())

        col.addView(sectionLabel("最新数据包"))
        txtFields = TextView(this)
        txtFields.textSize = 13f
        txtFields.setTextColor(Color.DKGRAY)
        col.addView(txtFields, lpMatch())

        col.addView(sectionLabel("原始数据"))
        txtRaw = TextView(this)
        txtRaw.textSize = 12f
        txtRaw.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
        txtRaw.setTextColor(Color.BLACK)
        col.addView(txtRaw, lpMatch())

        return scroll
    }

    private fun button(text: String): Button {
        val b = Button(this)
        b.text = text
        b.isAllCaps = false
        return b
    }

    private fun sectionLabel(text: String): TextView {
        val tv = TextView(this)
        tv.text = text
        tv.textSize = 14f
        tv.setTypeface(null, Typeface.BOLD)
        tv.setTextColor(Color.rgb(27, 94, 32))
        tv.setPadding(0, dp(10), 0, dp(2))
        return tv
    }

    private fun lpWeight(w: Float): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w)

    private fun lpMatch(): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

    private fun lpFill(): FrameLayout.LayoutParams =
        FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    // ------------------------------------------------------------ view switch
    private fun showList() {
        currentDetailAddress = null
        currentDetailPkt = null
        detailRoot.visibility = View.GONE
        listRoot.visibility = View.VISIBLE
        adapter.notifyDataSetChanged()
    }

    private fun showDetail(address: String) {
        currentDetailAddress = address
        currentDetailPkt = registry.list.firstOrNull { it.address == address }?.lastPkt
        listRoot.visibility = View.GONE
        detailRoot.visibility = View.VISIBLE
        renderDetail()
    }

    private fun renderDetail() {
        val pkt = currentDetailPkt ?: return
        val manual = currentDetailAddress == MANUAL_ADDRESS
        val entry = if (manual) null else registry.list.firstOrNull { it.address == currentDetailAddress }

        txtDetailTitle.text = buildString {
            append(if (manual) "手动粘贴" else currentDetailAddress ?: "")
            val rssi = entry?.rssi ?: pkt.rssi
            if (rssi != 0) append("  ($rssi dBm)")
        }

        txtRaw.text = "${pkt.address}\n" + pkt.raw.joinToString(" ") {
            String.format("%02X", it.toInt() and 0xFF)
        }

        val sb = StringBuilder()
        if (pkt.structureError.isNotEmpty()) {
            sb.append("结构错误: ").append(pkt.structureError).append('\n')
        }
        for ((k, v) in pkt.fmt) {
            sb.append(k).append(": ").append(v).append('\n')
        }
        txtFields.text = sb.toString()

        val report = if (entry != null) entry.assessor.report() else manualReport(pkt)
        val extra = if (!manual && report.packetsSeen > 0) {
            String.format(Locale.US, "   速率 %.1f 包/s", report.avgRateHz)
        } else {
            ""
        }
        txtVerdict.text = "${report.level.verdictLabel()}  ${report.note}$extra"
        txtVerdict.setBackgroundColor(C_VERDICT_BG[report.level] ?: Color.WHITE)
        txtVerdict.setTextColor(C_VERDICT_FG[report.level] ?: Color.BLACK)

        txtIssues.text = if (report.issues.isEmpty()) {
            "未发现问题"
        } else {
            report.issues.joinToString("\n") { i ->
                "[${i.level.label()}] ${i.code}: ${i.message}"
            }
        }
        txtIssues.setTextColor(
            if (report.issues.isEmpty()) Color.rgb(27, 94, 32)
            else C_ISSUE[report.issues.maxOfOrNull { it.level }] ?: Color.DKGRAY
        )
    }

    /** 手动粘贴包没有流式统计，退化为单包判定。 */
    private fun manualReport(pkt: DecodedPacket): HealthReport {
        val rep = HealthReport()
        val issues = Health.assessPacket(pkt)
        rep.issues.addAll(issues)
        rep.packetsOk = if (issues.isEmpty()) 1 else 0
        var worst = HealthLevel.PASS
        for (i in rep.issues) if (i.level.ordinal > worst.ordinal) worst = i.level
        rep.level = worst
        rep.note = when (rep.level) {
            HealthLevel.PASS -> "模块工作正常，广播符合 GB 46750-2025 要求"
            HealthLevel.WARN -> "存在可改善项，不影响基本广播"
            else -> "存在故障，请根据下方问题清单排查"
        }
        return rep
    }

    // ------------------------------------------------------------ permissions
    private fun ensurePermissionsThenScan() {
        pendingStart = true
        if (!hasPermissions()) {
            requestPermissions(neededPermissions(), REQ_PERMISSIONS)
        } else {
            startScanFlow()
        }
    }

    private fun neededPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun hasPermissions(): Boolean =
        neededPermissions().all {
            checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED
        }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERMISSIONS) return
        if (hasPermissions()) {
            startScanFlow()
        } else {
            pendingStart = false
            log("缺少蓝牙/定位权限，无法扫描")
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_BT_ENABLE) return
        if (resultCode == Activity.RESULT_OK) {
            startScanFlow()
        } else {
            pendingStart = false
            log("未开启蓝牙，无法扫描")
        }
    }

    private fun startScanFlow() {
        if (!pendingStart) return
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter
        if (adapter == null) {
            log("本机无蓝牙")
            return
        }
        if (!adapter.isEnabled) {
            log("请求开启蓝牙...")
            val enableIntent = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)
            startActivityForResult(enableIntent, REQ_BT_ENABLE)
            return
        }
        if (Build.VERSION.SDK_INT in 23..30) {
            log("提示：Android 12 以下需要打开系统定位开关才能扫描")
        }
        pendingStart = false
        val sc = BleScanner(bleListener)
        if (sc.start(this)) {
            scanner = sc
        }
    }

    // ------------------------------------------------------------------ data
    private fun onPacket(pkt: DecodedPacket) {
        registry.onPacket(pkt, System.currentTimeMillis())
        adapter.notifyDataSetChanged()
        txtCount.text = "已发现 ${registry.size} 台设备"
        if (pkt.address == currentDetailAddress) {
            currentDetailPkt = pkt
            renderDetail()
        }
    }

    private fun showPasteDialog() {
        val edit = EditText(this)
        edit.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        edit.gravity = Gravity.START or Gravity.TOP
        edit.hint = "粘贴完整 Raw 广播帧，或从 FF 开始的 GB 数据包\n例如: FF20 48 FF FE 43 50 4E..."
        edit.minLines = 5
        val pad = dp(12)
        edit.setPadding(pad, pad, pad, pad)

        AlertDialog.Builder(this)
            .setTitle("粘贴 HEX")
            .setView(edit)
            .setPositiveButton("解码") { _, _ ->
                val text = edit.text.toString().trim()
                if (text.isNotEmpty()) pasteHex(text)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun pasteHex(text: String) {
        val raw: ByteArray = try {
            Decoder.parseHex(text)
        } catch (e: IllegalArgumentException) {
            log("HEX 解析失败: ${e.message}")
            return
        }
        val pktRaw = Decoder.extractGbFromAdv(raw)
        if (pktRaw.size < 6) {
            log("HEX 过短，无法解析")
            return
        }
        if (!pktRaw.contentEquals(raw)) {
            log("从广播帧中提取到 GB 包（${pktRaw.size} 字节）")
        }
        val pkt = Decoder.decodeGbPacket(
            pktRaw,
            address = MANUAL_ADDRESS,
            rssi = 0,
            receivedAtMs = System.nanoTime() / 1_000_000,
            source = "manual"
        )
        onManualPacket(pkt)
        log("已手动解码 ${pktRaw.size} 字节: " +
            pktRaw.joinToString("") { String.format("%02X", it.toInt() and 0xFF) })
    }

    private fun onManualPacket(pkt: DecodedPacket) {
        currentDetailAddress = MANUAL_ADDRESS
        currentDetailPkt = pkt
        listRoot.visibility = View.GONE
        detailRoot.visibility = View.VISIBLE
        renderDetail()
    }

    // ------------------------------------------------------------------ log
    private fun log(msg: String) {
        if (!::txtLog.isInitialized) return
        val base = if (txtLog.text.isEmpty()) "" else txtLog.text.toString() + "\n"
        txtLog.text = base + msg
        val lines = txtLog.text.split("\n")
        if (lines.size > MAX_LOG_LINES) {
            txtLog.text = lines.takeLast(MAX_LOG_LINES).joinToString("\n")
        }
    }
}
