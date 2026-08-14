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
import com.ridcheck.ble.RidScanService
import com.ridcheck.core.AppState
import com.ridcheck.core.DecodedPacket
import com.ridcheck.core.Decoder
import com.ridcheck.core.toHexJoined
import com.ridcheck.ui.BottomBar
import com.ridcheck.ui.ConfigDeviceAdapter
import com.ridcheck.ui.ConfigPage
import com.ridcheck.ui.DetailPage
import com.ridcheck.ui.DeviceListAdapter
import com.ridcheck.ui.ExplainPage
import com.ridcheck.ui.Theme
import com.ridcheck.ui.button
import com.ridcheck.ui.dp
import com.ridcheck.ui.lpFill
import com.ridcheck.ui.lpWeight
import com.ridcheck.ui.sectionLabel

/**
 * 安卓版 RID 检测工具：设备列表 + 详情页（自用，界面从简）。
 * 主界面列出所有广播 UUID 0xFFFF 的信号源，点进某台设备实时查看判定/字段/原始数据。
 * 详情页与底部导航分别由 DetailPage / BottomBar 承载，本类保留列表、扫描控制、
 * 权限流、粘贴入口、页面切换、日志与每秒刷新。
 */
class MainActivity : Activity() {

    companion object {
        private const val REQ_PERMISSIONS = 1001
        private const val REQ_BT_ENABLE = 1002
    }

    private var pendingStart = false

    // UI
    private lateinit var listRoot: LinearLayout
    private lateinit var explainRoot: ScrollView
    private lateinit var adapter: DeviceListAdapter
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var txtScanState: TextView
    private lateinit var txtCount: TextView
    private lateinit var txtLog: TextView
    private lateinit var detailPage: DetailPage
    private lateinit var bottomBar: BottomBar

    // GATT 双向配置（按选中的可配置模块地址动态构建）
    private var configPage: ConfigPage? = null
    private lateinit var configRoot: FrameLayout
    private lateinit var configAdapter: ConfigDeviceAdapter

    /**
     * 每秒刷新 UI（扫描状态、列表、日志、详情）。
     * 数据采样由 RidScanService 负责（1Hz），这里只读 AppState 展示，避免双份采样。
     */
    private val ticker = Handler(Looper.getMainLooper())
    private val tickRunnable = object : Runnable {
        override fun run() {
            renderScanState()
            adapter.notifyDataSetChanged()
            configAdapter.notifyDataSetChanged()
            txtCount.text = "广播 ${AppState.registry.size} · 可配置 ${AppState.configDeviceList.size}"
            renderLog()
            if (detailPage.isShowing) detailPage.render()
            ticker.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        log("就绪。点“开始扫描”收集广播（0xFFFF）与可配置模块（0xFFF0），点信号源进详情或配置；或粘贴 HEX。")
    }

    override fun onResume() {
        super.onResume()
        renderScanState()
        renderLog()
        ticker.post(tickRunnable)
    }

    override fun onPause() {
        super.onPause()
        // 只暂停 UI 刷新；扫描/记录由前台服务持有，退到后台（QGC 在前台）不中断
        ticker.removeCallbacks(tickRunnable)
    }

    override fun onDestroy() {
        super.onDestroy()
        configPage?.shutdown()
    }

    override fun onBackPressed() {
        when {
            detailPage.isShowing -> showList()
            explainRoot.visibility == View.VISIBLE -> showMainTab()
            configRoot.visibility == View.VISIBLE -> showList()
            else -> super.onBackPressed()
        }
    }

    // ------------------------------------------------------------------ UI
    private fun buildUi() {
        val root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL

        val content = FrameLayout(this)
        listRoot = buildListPage()
        detailPage = DetailPage(this) { showList() }
        configRoot = FrameLayout(this)
        explainRoot = ExplainPage.build(this)
        content.addView(listRoot, lpFill())
        content.addView(detailPage.root, lpFill())
        content.addView(configRoot, lpFill())
        content.addView(explainRoot, lpFill())
        detailPage.root.visibility = View.GONE
        configRoot.visibility = View.GONE
        explainRoot.visibility = View.GONE

        // 内容区占满底部导航以上空间；宽 MATCH_PARENT + 高 0 + weight 1
        root.addView(content, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
        ))
        bottomBar = BottomBar(this, { showMainTab() }, { showExplainTab() })
        root.addView(bottomBar.root, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(56)
        ))

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
        title.setTextColor(Theme.PRIMARY)
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
        btnStop.setOnClickListener {
            RidScanService.stop(this)
            AppState.scanning = false
            renderScanState()
        }
        btnRow.addView(btnStop, lpWeight(1f))

        val btnPaste = button("粘贴解码")
        btnPaste.setOnClickListener { showPasteDialog() }
        btnRow.addView(btnPaste, lpWeight(1f))

        val statusRow = LinearLayout(this)
        statusRow.orientation = LinearLayout.HORIZONTAL
        statusRow.setPadding(0, dp(4), 0, dp(4))
        col.addView(statusRow)

        txtScanState = TextView(this)
        txtScanState.text = "● 已停止"
        txtScanState.setTextColor(Theme.TEXT_MUTED)
        txtScanState.setTypeface(null, Typeface.BOLD)
        statusRow.addView(txtScanState, lpWeight(1f))

        txtCount = TextView(this)
        txtCount.text = "已发现 0 台设备"
        txtCount.gravity = Gravity.END
        statusRow.addView(txtCount)

        col.addView(sectionLabel("正在广播的信号源（点击查看详情）"))

        adapter = DeviceListAdapter(this) { AppState.registry.list }
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

        col.addView(sectionLabel("可 GATT 配置的信号源（点击进入配置）"))

        configAdapter = ConfigDeviceAdapter(this) { AppState.configDeviceList }
        val configList = ListView(this)
        configList.adapter = configAdapter
        configList.setOnItemClickListener { _, _, position, _ ->
            showConfig(configAdapter.getItem(position).address)
        }
        col.addView(configList, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(130)
        ))

        col.addView(sectionLabel("日志"))
        val logScroll = ScrollView(this)
        txtLog = TextView(this)
        txtLog.textSize = 12f
        txtLog.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
        txtLog.setTextColor(Theme.TEXT_SECONDARY)
        logScroll.addView(txtLog, lpFill())
        col.addView(logScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(130)))

        return col
    }

    // ------------------------------------------------------------ view switch
    private fun showList() {
        detailPage.clear()
        detailPage.root.visibility = View.GONE
        configRoot.visibility = View.GONE
        listRoot.visibility = View.VISIBLE
        adapter.notifyDataSetChanged()
    }

    private fun showDetail(address: String) {
        listRoot.visibility = View.GONE
        configRoot.visibility = View.GONE
        detailPage.root.visibility = View.VISIBLE
        explainRoot.visibility = View.GONE
        setActiveTab(Tab.MAIN)
        detailPage.showDevice(address)
    }

    private fun showConfig(address: String) {
        detailPage.clear()
        detailPage.root.visibility = View.GONE
        configPage?.shutdown()
        configPage = ConfigPage(this, address) { showList() }
        configRoot.removeAllViews()
        configRoot.addView(configPage!!.root, lpFill())
        listRoot.visibility = View.GONE
        explainRoot.visibility = View.GONE
        configRoot.visibility = View.VISIBLE
        setActiveTab(Tab.MAIN)
    }

    private fun showExplainTab() {
        detailPage.clear()
        detailPage.root.visibility = View.GONE
        listRoot.visibility = View.GONE
        configRoot.visibility = View.GONE
        explainRoot.visibility = View.VISIBLE
        setActiveTab(Tab.EXPLAIN)
        adapter.notifyDataSetChanged()
    }

    private fun showMainTab() {
        explainRoot.visibility = View.GONE
        configRoot.visibility = View.GONE
        showList()
        setActiveTab(Tab.MAIN)
    }

    private enum class Tab { MAIN, EXPLAIN }

    private fun setActiveTab(tab: Tab) =
        bottomBar.setActive(if (tab == Tab.MAIN) BottomBar.Tab.MAIN else BottomBar.Tab.EXPLAIN)

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
        if (Build.VERSION.SDK_INT >= 34) {
            // Android 14：connectedDevice 前台服务权限 + 通知权限均为运行时权限
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE,
                Manifest.permission.POST_NOTIFICATIONS
            )
        } else if (Build.VERSION.SDK_INT >= 33) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.POST_NOTIFICATIONS
            )
        } else if (Build.VERSION.SDK_INT >= 31) {
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
        log("启动后台扫描：前台服务常驻，切到 QGC 等前台应用也不中断记录")
        RidScanService.start(this)
        AppState.scanning = true
        renderScanState()
    }

    // ------------------------------------------------------------------ data
    private fun showPasteDialog() {
        val edit = EditText(this)
        edit.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        edit.gravity = Gravity.START or Gravity.TOP
        edit.hint = "粘贴完整 Raw 广播帧，或从 FF 开始的 GB 数据包（单包静态判定）\n例如: FF20 48 FF FE 43 50 4E..."
        edit.minLines = 5
        val pad = dp(12)
        edit.setPadding(pad, pad, pad, pad)

        AlertDialog.Builder(this)
            .setTitle("粘贴解码")
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
            address = DetailPage.MANUAL_ADDRESS,
            rssi = 0,
            receivedAtMs = System.nanoTime() / 1_000_000
        )
        onManualPacket(pkt)
        log("已手动解码 ${pktRaw.size} 字节: " + pktRaw.toHexJoined())
    }

    private fun onManualPacket(pkt: DecodedPacket) {
        listRoot.visibility = View.GONE
        configRoot.visibility = View.GONE
        detailPage.root.visibility = View.VISIBLE
        explainRoot.visibility = View.GONE
        setActiveTab(Tab.MAIN)
        detailPage.showManual(pkt)
    }

    // ------------------------------------------------------------------ log
    private fun log(msg: String) {
        AppState.addLog(msg)
        renderLog()
    }

    /** 把共享日志缓冲渲染到界面；服务在后台产生的日志由每秒 ticker 拾取。 */
    private fun renderLog() {
        if (!::txtLog.isInitialized) return
        txtLog.text = AppState.logText
    }

    /** 从 AppState.scanning 同步按钮与状态行（服务状态变更经每秒 ticker 反映）。 */
    private fun renderScanState() {
        val scanning = AppState.scanning
        btnStart.isEnabled = !scanning
        btnStop.isEnabled = scanning
        txtScanState.text = if (scanning) "● 扫描中" else "● 已停止"
        txtScanState.setTextColor(if (scanning) Theme.PRIMARY else Theme.TEXT_MUTED)
    }
}
