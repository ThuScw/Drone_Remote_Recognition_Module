package com.ridcheck.ui

import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.text.InputFilter
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import com.ridcheck.ble.GattConfigClient
import com.ridcheck.core.GattConfig
import com.ridcheck.core.RidConfigData

/**
 * GATT 双向配置页：针对主界面选中的某一台可配置模块（按 MAC 地址绑定），
 * 读取/写入 4 个身份字段 + 状态。与 PC 版 app/gui/config_panel.py 行为对齐。
 * 设备识别由统一扫描器按报文字节（0xFFF0）完成，本页不再自行扫描。
 */
class ConfigPage(
    private val activity: Activity,
    private val address: String,
    private val onBack: () -> Unit
) {

    companion object {
        private const val MAX_LOG_LINES = 200
    }

    val root: ScrollView

    private lateinit var lblState: TextView
    private lateinit var editUas: EditText
    private lateinit var editRealname: EditText
    private lateinit var spinnerOp: Spinner
    private lateinit var spinnerUa: Spinner
    private lateinit var btnRead: Button
    private lateinit var btnWrite: Button
    private lateinit var logView: TextView

    private val logLines = ArrayDeque<String>()
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

        // 顶部：返回 + 标题
        val topRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val btnBack = button("← 返回列表")
        btnBack.setOnClickListener { onBack() }
        topRow.addView(btnBack)
        topRow.addView(title("GATT 配置").apply {
            setPadding(dp(10), 0, 0, 0)
            gravity = android.view.Gravity.CENTER_VERTICAL
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(topRow)

        col.addView(label("设备: $address"))
        lblState = label("状态: --")
        col.addView(lblState)

        // 操作行
        val actRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        btnRead = button("连接读取")
        btnRead.setOnClickListener { readConfig() }
        actRow.addView(btnRead, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        btnWrite = button("写入配置")
        btnWrite.isEnabled = false
        btnWrite.setOnClickListener { writeConfig() }
        actRow.addView(btnWrite, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(actRow)

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
        val writeRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val btnClear = button("清空输入")
        btnClear.setOnClickListener { clearFields() }
        writeRow.addView(btnClear, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(writeRow)

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

    private fun remoteDevice(): BluetoothDevice? =
        (activity.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)
            ?.adapter?.getRemoteDevice(address)

    // ------------------------------------------------------------ actions
    private fun readConfig() {
        val device = remoteDevice() ?: run { log("蓝牙不可用"); return }
        val client = GattConfigClient(device, gattListener)
        gattClient = client
        setBusy(true)
        client.startRead(activity)
    }

    private fun writeConfig() {
        val device = remoteDevice() ?: run { log("蓝牙不可用"); return }
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
        btnRead.isEnabled = !b
        btnWrite.isEnabled = !b && connectedCfg != null
    }

    // ---------------------------------------------------------- callbacks
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

    // -------------------------------------------------------------- log
    private fun log(msg: String) {
        logLines.addLast(msg)
        while (logLines.size > MAX_LOG_LINES) logLines.removeFirst()
        logView.text = logLines.joinToString("\n")
    }

    fun shutdown() {
        gattClient?.close()
        gattClient = null
    }
}
