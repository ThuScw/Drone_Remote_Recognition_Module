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
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.ridcheck.ble.BleScanner
import java.util.Locale
import com.ridcheck.core.Decoder
import com.ridcheck.core.DecodedPacket
import com.ridcheck.core.HealthLevel
import com.ridcheck.core.StreamAssessor

/** 安卓版 RID 检测工具：实时 BLE 扫描解码 + 粘贴 HEX 解码（自用，界面从简）。 */
class MainActivity : Activity() {

    companion object {
        private const val REQ_PERMISSIONS = 1001
        private const val REQ_BT_ENABLE = 1002
        private const val MAX_LOG_LINES = 200

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

    private val assessor = StreamAssessor()
    private var scanner: BleScanner? = null
    private var pendingStart = false
    private var packetCount = 0

    // UI
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var txtState: TextView
    private lateinit var txtCount: TextView
    private lateinit var txtVerdict: TextView
    private lateinit var txtIssues: TextView
    private lateinit var txtFields: TextView
    private lateinit var txtRaw: TextView
    private lateinit var txtLog: TextView

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
            txtState.text = if (scanning) "● 扫描中" else "● 已停止"
            txtState.setTextColor(if (scanning) Color.rgb(27, 94, 32) else Color.rgb(102, 102, 102))
        }

        override fun onLog(text: String) {
            log(text)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        log("就绪。点击“开始扫描”，或粘贴 nRF Connect 抓到的 HEX。")
    }

    // ------------------------------------------------------------------ UI
    private fun buildUi() {
        val root = ScrollView(this)
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        col.setPadding(dp(12), dp(8), dp(12), dp(24))
        root.addView(col, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

        val title = TextView(this)
        title.text = "RID 检测（GB 46750-2025）"
        title.textSize = 18f
        title.setTypeface(null, Typeface.BOLD)
        title.setTextColor(Color.rgb(27, 94, 32))
        col.addView(title)

        // 按钮行
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

        // 状态行
        val statusRow = LinearLayout(this)
        statusRow.orientation = LinearLayout.HORIZONTAL
        statusRow.setPadding(0, dp(4), 0, dp(4))
        col.addView(statusRow)

        txtState = TextView(this)
        txtState.text = "● 已停止"
        txtState.setTextColor(Color.rgb(102, 102, 102))
        txtState.setTypeface(null, Typeface.BOLD)
        statusRow.addView(txtState, lpWeight(1f))

        txtCount = TextView(this)
        txtCount.text = "已收包: 0"
        txtCount.gravity = Gravity.END
        statusRow.addView(txtCount)

        // 判定条
        txtVerdict = TextView(this)
        txtVerdict.setTextColor(Color.BLACK)
        txtVerdict.setPadding(dp(10), dp(10), dp(10), dp(10))
        txtVerdict.textSize = 15f
        col.addView(txtVerdict, lpMatch())

        // 问题列表
        col.addView(sectionLabel("问题清单"))
        txtIssues = TextView(this)
        txtIssues.textSize = 14f
        txtIssues.setTextColor(Color.DKGRAY)
        col.addView(txtIssues, lpMatch())

        // 最新包字段
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

        // 日志
        col.addView(sectionLabel("日志"))
        txtLog = TextView(this)
        txtLog.textSize = 12f
        txtLog.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
        txtLog.setTextColor(Color.rgb(80, 80, 80))
        col.addView(txtLog, lpMatch())

        setContentView(root)
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

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

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

    override fun onPause() {
        super.onPause()
        scanner?.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        scanner?.stop()
    }

    // ------------------------------------------------------------------ data
    private fun onPacket(pkt: DecodedPacket) {
        packetCount++
        txtCount.text = "已收包: $packetCount"
        assessor.push(pkt)
        renderPacket(pkt)
        renderReport()
    }

    private fun renderPacket(pkt: DecodedPacket) {
        val spaced = pkt.raw.joinToString(" ") { String.format("%02X", it.toInt() and 0xFF) }
        txtRaw.text = "${pkt.address}\n$spaced"

        val sb = StringBuilder()
        if (pkt.structureError.isNotEmpty()) {
            sb.append("结构错误: ").append(pkt.structureError).append('\n')
        }
        for ((k, v) in pkt.fmt) {
            sb.append(k).append(": ").append(v).append('\n')
        }
        txtFields.text = sb.toString()
    }

    private fun renderReport() {
        val rep = assessor.report()
        val extra = if (rep.packetsSeen > 0) {
            String.format(Locale.US, "   速率 %.1f 包/s", rep.avgRateHz)
        } else {
            ""
        }
        txtVerdict.text = "${rep.level.verdictLabel()}  ${rep.note}$extra"
        txtVerdict.setBackgroundColor(C_VERDICT_BG[rep.level] ?: Color.WHITE)
        txtVerdict.setTextColor(C_VERDICT_FG[rep.level] ?: Color.BLACK)

        txtIssues.text = if (rep.issues.isEmpty()) {
            "未发现问题"
        } else {
            rep.issues.joinToString("\n") { i ->
                "[${i.level.label()}] ${i.code}: ${i.message}"
            }
        }
        txtIssues.setTextColor(
            if (rep.issues.isEmpty()) Color.rgb(27, 94, 32)
            else C_ISSUE[rep.issues.maxOfOrNull { it.level }] ?: Color.DKGRAY
        )
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
        val before = raw
        val pktRaw = Decoder.extractGbFromAdv(raw)
        if (pktRaw.size < 6) {
            log("HEX 过短，无法解析")
            return
        }
        if (!pktRaw.contentEquals(before)) {
            log("从广播帧中提取到 GB 包（${pktRaw.size} 字节）")
        }
        val pkt = Decoder.decodeGbPacket(
            pktRaw,
            address = "手动",
            rssi = 0,
            receivedAtMs = System.nanoTime() / 1_000_000,
            source = "manual"
        )
        onPacket(pkt)
        log("已手动解码 ${pktRaw.size} 字节: " +
            pktRaw.joinToString("") { String.format("%02X", it.toInt() and 0xFF) })
    }

    // ------------------------------------------------------------------ log
    private fun log(msg: String) {
        val base = if (txtLog.text.isEmpty()) "" else txtLog.text.toString() + "\n"
        txtLog.text = base + msg
        val lines = txtLog.text.split("\n")
        if (lines.size > MAX_LOG_LINES) {
            txtLog.text = lines.takeLast(MAX_LOG_LINES).joinToString("\n")
        }
    }
}
