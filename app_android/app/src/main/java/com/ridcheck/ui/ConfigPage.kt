package com.ridcheck.ui

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.text.InputFilter
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import com.ridcheck.ble.ConfigScanner
import com.ridcheck.ble.GattConfigClient
import com.ridcheck.core.GattConfig
import com.ridcheck.core.RidConfigData

/**
 * GATT 双向配置页：扫描地面态模块，读取/写入 4 个身份字段 + 状态。
 * 与 PC 版 app/gui/config_panel.py 行为对齐。
 */
class ConfigPage(private val activity: Activity) {

    companion object {
        const val REQ_CONFIG_PERMISSIONS = 1003
        private const val MAX_LOG_LINES = 200
    }

    val root: ScrollView

    private lateinit var deviceSpinner: Spinner
    private lateinit var btnScan: Button
    private lateinit var btnRead: Button
    private lateinit var lblState: TextView
    private lateinit var editUas: EditText
    private lateinit var editRealname: EditText
    private lateinit var spinnerOp: Spinner
    private lateinit var spinnerUa: Spinner
    private lateinit var btnWrite: Button
    private lateinit var logView: TextView

    private val devices = ArrayList<String>()
    private val logLines = ArrayDeque<String>()
    private var scanner: ConfigScanner? = null
    private var gattClient: GattConfigClient? = null
    private var connectedCfg: RidConfigData? = null
    private var busy = false

    init {
        root = build()
    }

    // ------------------------------------------------------------------ UI
    private fun build(): ScrollView {
        val scroll = ScrollView(activity)
        val col = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(8), dp(12), dp(24))
        }
        scroll.addView(col, ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ))

        col.addView(title("GATT 双向配置"))

        // 设备行
        val deviceRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        deviceSpinner = Spinner(activity)
        deviceRow.addView(deviceSpinner, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        btnScan = button("扫描设备")
        btnScan.setOnClickListener { startScan() }
        btnRead = button("连接读取")
        btnRead.setOnClickListener { readConfig() }
        deviceRow.addView(btnScan)
        deviceRow.addView(btnRead)
        refreshDeviceSpinner()
        col.addView(deviceRow)

        lblState = label("状态: --")
        col.addView(lblState)

        col.addView(sectionLabel("身份字段（起飞前写入，起飞后广播）"))

        col.addView(fieldLabel("唯一产品识别码（20 位 [0-9A-Z]，不含 O/I）"))
        editUas = EditText(activity).apply {
            setSingleLine(true)
            filters = arrayOf<InputFilter>(InputFilter.LengthFilter(GattConfig.UAS_ID_LEN))
        }
        col.addView(editUas, lpMatch())

        col.addView(fieldLabel("实名登记标志（8 位数字，UOM 实名号后 8 位）"))
        editRealname = EditText(activity).apply {
            setSingleLine(true)
            filters = arrayOf<InputFilter>(InputFilter.LengthFilter(GattConfig.REALNAME_LEN))
        }
        col.addView(editRealname, lpMatch())

        col.addView(fieldLabel("运行类别"))
        spinnerOp = Spinner(activity)
        spinnerOp.adapter = ArrayAdapter(
            activity, android.R.layout.simple_spinner_item,
            GattConfig.OP_CATEGORY_NAMES.toSortedMap().map { (k, v) -> "$k — $v" }
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
        spinnerOp.setSelection(1) // 开放类
        col.addView(spinnerOp, lpMatch())

        col.addView(fieldLabel("无人机分类"))
        spinnerUa = Spinner(activity)
        spinnerUa.adapter = ArrayAdapter(
            activity, android.R.layout.simple_spinner_item,
            GattConfig.UA_CLASS_NAMES.toSortedMap().map { (k, v) -> "$k — $v" }
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
        spinnerUa.setSelection(1) // 轻型
        col.addView(spinnerUa, lpMatch())

        // 操作行
        val actRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        btnWrite = button("写入配置")
        btnWrite.isEnabled = false
        btnWrite.setOnClickListener { writeConfig() }
        val btnClear = button("清空输入")
        btnClear.setOnClickListener { clearFields() }
        actRow.addView(btnWrite, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actRow.addView(btnClear, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(actRow)

        col.addView(body(
            "说明：模块仅在地面态接受写入；空中态写锁定。写入成功后即持久化到 NVS，" +
                "下次起飞即广播本次录入值。未写入直接起飞则广播占位值或上次存储值。"
        ))

        col.addView(sectionLabel("日志"))
        logView = TextView(activity).apply {
            textSize = 12f
            setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
            setTextColor(Color.rgb(80, 80, 80))
        }
        col.addView(logView, lpMatch())

        return scroll
    }

    private fun title(text: String): TextView = TextView(activity).apply {
        this.text = text
        textSize = 18f
        setTypeface(null, Typeface.BOLD)
        setTextColor(Theme.PRIMARY)
    }

    private fun sectionLabel(text: String): TextView = TextView(activity).apply {
        this.text = text
        textSize = 14f
        setTypeface(null, Typeface.BOLD)
        setTextColor(Theme.PRIMARY)
        setPadding(0, dp(10), 0, dp(2))
    }

    private fun fieldLabel(text: String): TextView = TextView(activity).apply {
        this.text = text
        textSize = 12f
        setTextColor(Color.rgb(90, 90, 90))
        setPadding(0, dp(6), 0, dp(2))
    }

    private fun label(text: String): TextView = TextView(activity).apply {
        this.text = text
        textSize = 14f
        setTextColor(Theme.PRIMARY)
        setPadding(0, dp(8), 0, dp(2))
    }

    private fun body(text: String): TextView = TextView(activity).apply {
        this.text = text
        textSize = 12f
        setTextColor(Color.rgb(70, 70, 70))
        setLineSpacing(dp(4).toFloat(), 1.0f)
        setPadding(0, dp(4), 0, dp(4))
    }

    private fun button(text: String): Button = Button(activity).apply {
        this.text = text
        isAllCaps = false
    }

    private fun lpMatch(): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

    private fun dp(v: Int): Int = (v * activity.resources.displayMetrics.density).toInt()

    // ------------------------------------------------------------- device
    private fun selectedDevice(): String? = if (devices.isEmpty()) null
    else devices[deviceSpinner.selectedItemPosition.coerceIn(0, devices.size - 1)]

    private fun remoteDevice(address: String) =
        (activity.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)
            ?.adapter?.getRemoteDevice(address)

    private fun refreshDeviceSpinner() {
        deviceSpinner.adapter = ArrayAdapter(
            activity, android.R.layout.simple_spinner_item,
            if (devices.isEmpty()) listOf("（未发现模块）") else devices
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
        btnRead.isEnabled = devices.isNotEmpty()
    }

    // ------------------------------------------------------------ actions
    private fun startScan() {
        if (!hasPermissions()) {
            activity.requestPermissions(neededPermissions(), REQ_CONFIG_PERMISSIONS)
            return
        }
        doScan()
    }

    private fun doScan() {
        if (scanner != null) return
        devices.clear()
        refreshDeviceSpinner()
        btnScan.isEnabled = false
        val sc = ConfigScanner(scanListener)
        scanner = sc
        if (!sc.start(activity)) {
            log("扫描启动失败：无蓝牙或权限不足")
            btnScan.isEnabled = true
            scanner = null
        }
    }

    private fun readConfig() {
        val addr = selectedDevice() ?: return
        val device = remoteDevice(addr) ?: run { log("蓝牙不可用"); return }
        val client = GattConfigClient(device, gattListener)
        gattClient = client
        setBusy(true)
        client.startRead(activity)
    }

    private fun writeConfig() {
        val addr = selectedDevice() ?: return
        val device = remoteDevice(addr) ?: run { log("蓝牙不可用"); return }
        val cfg = collectFields()
        val errs = cfg.validate()
        if (errs.isNotEmpty()) {
            for (e in errs) log("校验失败: $e")
            return
        }
        val client = GattConfigClient(device, gattListener)
        gattClient = client
        setBusy(true)
        client.startWrite(activity, cfg)
    }

    private fun collectFields(): RidConfigData = RidConfigData(
        uasId = editUas.text.toString().trim(),
        realname = editRealname.text.toString().trim(),
        opCategory = spinnerOp.selectedItemPosition,
        uaClass = spinnerUa.selectedItemPosition
    )

    private fun populateFields(cfg: RidConfigData) {
        editUas.setText(cfg.uasId)
        editRealname.setText(cfg.realname)
        if (cfg.opCategory in 0..3) spinnerOp.setSelection(cfg.opCategory)
        if (cfg.uaClass in 0..4) spinnerUa.setSelection(cfg.uaClass)
        lblState.text = "状态: ${cfg.stateName}"
        btnWrite.isEnabled = true
    }

    private fun clearFields() {
        editUas.setText("")
        editRealname.setText("")
        spinnerOp.setSelection(1)
        spinnerUa.setSelection(1)
        lblState.text = "状态: --"
    }

    private fun setBusy(b: Boolean) {
        busy = b
        btnScan.isEnabled = !b
        btnRead.isEnabled = !b && devices.isNotEmpty()
        btnWrite.isEnabled = !b && connectedCfg != null
    }

    // ---------------------------------------------------------- callbacks
    private val scanListener = object : ConfigScanner.Listener {
        override fun onDevice(address: String) {
            devices.add(address)
            refreshDeviceSpinner()
        }

        override fun onLog(text: String) = log(text)

        override fun onDone() {
            if (!busy) btnScan.isEnabled = true
            scanner = null
            if (devices.isEmpty()) log("未发现可配置模块（确认模块处于地面可连接态）")
        }
    }

    private val gattListener = object : GattConfigClient.Listener {
        override fun onLog(text: String) = log(text)

        override fun onRead(config: RidConfigData) {
            connectedCfg = config
            populateFields(config)
            log("读取成功 — 状态: ${config.stateName}")
            setBusy(false)
        }

        override fun onWritten(config: RidConfigData) {
            connectedCfg = config
            populateFields(config)
            log("写入成功 — 已持久化到模块 NVS")
            setBusy(false)
        }

        override fun onError(message: String) {
            log("错误: $message")
            setBusy(false)
        }
    }

    // -------------------------------------------------------- permissions
    private fun neededPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun hasPermissions(): Boolean =
        neededPermissions().all {
            activity.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED
        }

    /** 由 MainActivity.onRequestPermissionsResult 转发，授权后继续扫描。 */
    fun onRequestPermissionsResult(requestCode: Int) {
        if (requestCode != REQ_CONFIG_PERMISSIONS) return
        if (hasPermissions()) doScan() else log("缺少蓝牙/定位权限，无法扫描")
    }

    // -------------------------------------------------------------- log
    private fun log(msg: String) {
        logLines.addLast(msg)
        while (logLines.size > MAX_LOG_LINES) logLines.removeFirst()
        logView.text = logLines.joinToString("\n")
    }

    fun shutdown() {
        scanner?.stop()
        scanner = null
        gattClient?.close()
        gattClient = null
    }
}
