package com.ridcheck.ble

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper

/**
 * 扫描地面态可配置模块（名称 GBI_RID_001 / 服务 0xFFF0）。
 * 与 PC 版 gui/workers.py 的 ConfigScanWorker 行为对齐：按名称过滤，去重上报 MAC。
 * 地面态模块使用传统可连接广播（legacy PDU），故扫描用 setLegacy(true)。
 */
class ConfigScanner(private val listener: Listener) {

    interface Listener {
        fun onDevice(address: String)
        fun onLog(text: String)
        fun onDone()
    }

    companion object {
        const val EXPECTED_NAME = "GBI_RID_001"
    }

    private val main = Handler(Looper.getMainLooper())
    private var adapter: BluetoothAdapter? = null
    private val seen = HashSet<String>()
    private val stopRunnable = Runnable { stop() }

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = try {
                result.scanRecord?.deviceName ?: result.device.name
            } catch (e: SecurityException) {
                null
            }
            if (name?.trim() != EXPECTED_NAME) return
            val address = result.device.address
            if (seen.add(address)) {
                main.post {
                    listener.onDevice(address)
                    listener.onLog("发现模块: $address")
                }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                listener.onLog("扫描失败 (errorCode=$errorCode)")
                listener.onDone()
            }
        }
    }

    fun start(context: Context): Boolean {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val a = manager.adapter ?: return false
        val scanner = a.bluetoothLeScanner ?: return false
        if (!a.isEnabled) return false
        adapter = a
        seen.clear()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setLegacy(true)
            .build()
        try {
            scanner.startScan(null, settings, callback)
        } catch (e: SecurityException) {
            main.post { listener.onLog("缺少权限，无法扫描: ${e.message}") }
            return false
        }
        main.post { listener.onLog("扫描可配置模块（名称 $EXPECTED_NAME / 服务 0xFFF0）...") }
        // 8 秒后自动停止（与 PC 版 ~6s 扫描时长对齐），避免持续耗电
        main.postDelayed(stopRunnable, 8000)
        return true
    }

    fun stop() {
        main.removeCallbacks(stopRunnable)
        val a = adapter ?: return
        try {
            a.bluetoothLeScanner?.stopScan(callback)
        } catch (e: SecurityException) {
            // 权限已撤销时忽略
        }
        main.post { listener.onDone() }
    }
}
