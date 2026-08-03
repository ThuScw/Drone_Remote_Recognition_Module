package com.ridcheck.ble

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanRecord
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.ParcelUuid
import com.ridcheck.core.Decoder

/**
 * 基于 BluetoothLeScanner 的 RID 广播扫描封装。
 * 行为对齐 PC 版 rid/ble_scanner.py + gui/workers.py：
 * - 全接受扫描，回调内手动过滤 UUID 0x0D50（规避 ScanFilter 的 Service Data 匹配差异）
 * - 相同原始包去重（固件每事件在 3 个信道重发同包）
 * - 回调在主线程 → 可直接刷新 UI
 */
class BleScanner(private val listener: Listener) {

    interface Listener {
        /** 收到一个去重后的原始 GB 包（主线程）。 */
        fun onDistinctPacket(address: String, rssi: Int, raw: ByteArray)

        fun onScanState(scanning: Boolean)

        fun onLog(text: String)
    }

    companion object {
        const val SERVICE_UUID_16BIT = 0x0D50
        val SERVICE_UUID_128: ParcelUuid =
            ParcelUuid.fromString("00000d50-0000-1000-8000-00805f9b34fb")
    }

    private var adapter: BluetoothAdapter? = null
    private var lastRaw: ByteArray? = null
    private val seenDevices = HashSet<String>()

    var isScanning: Boolean = false
        private set

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val rec = result.scanRecord ?: return
            if (!isTarget(rec)) return
            val raw = extractPacket(rec) ?: return
            if (lastRaw != null && raw.contentEquals(lastRaw)) return
            lastRaw = raw.copyOf()
            val address = try {
                result.device.address
            } catch (e: SecurityException) {
                return
            }
            if (seenDevices.add(address)) {
                val name = try {
                    result.device.name
                } catch (e: SecurityException) {
                    null
                }
                listener.onLog("首次发现模块: $address  RSSI=${result.rssi}  名称=${name ?: "(无)"}")
            }
            listener.onDistinctPacket(address, result.rssi, raw)
        }

        override fun onScanFailed(errorCode: Int) {
            listener.onLog("扫描失败 (errorCode=$errorCode)")
            isScanning = false
            listener.onScanState(false)
        }
    }

    /** 开始扫描。需要调用方已获得相应运行时权限。返回是否真正开始。 */
    fun start(context: Context): Boolean {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val a = manager.adapter ?: return false
        val scanner = a.bluetoothLeScanner ?: return false
        if (!a.isEnabled) return false
        adapter = a
        lastRaw = null
        // setLegacy(false)：接收 BLE5 扩展广播。默认只上报传统广播，扩展广播包根本到不了回调。
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setLegacy(false)
            .build()
        try {
            scanner.startScan(null, settings, callback)
        } catch (e: SecurityException) {
            listener.onLog("缺少权限，无法扫描: ${e.message}")
            return false
        }
        isScanning = true
        listener.onScanState(true)
        listener.onLog("蓝牙扫描已开始，等待模块广播（UUID 0x0D50）...")
        return true
    }

    fun stop() {
        val a = adapter ?: return
        try {
            a.bluetoothLeScanner?.stopScan(callback)
        } catch (e: SecurityException) {
            // 权限已撤销时忽略
        }
        isScanning = false
        listener.onScanState(false)
        listener.onLog("扫描已停止")
    }

    /** 是否来自 RID 模块：含 UUID 0x0D50 的 Service Data（或原始 AD 字节可解析出 0x0D50）。 */
    private fun isTarget(rec: ScanRecord): Boolean {
        val sd = rec.getServiceData()
        if (sd != null && sd.containsKey(SERVICE_UUID_128)) return true

        val uuids = rec.serviceUuids
        if (uuids != null && uuids.contains(SERVICE_UUID_128)) return true

        val raw = rec.getBytes()
        return raw.isNotEmpty() &&
            Decoder.parseAdServiceData(raw).containsKey(SERVICE_UUID_16BIT)
    }

    /** 提取原始 GB 包：优先归一化 Service Data，兜底解析原始 AD 字节。 */
    private fun extractPacket(rec: ScanRecord): ByteArray? {
        val sd = rec.getServiceData()
        val data = sd?.get(SERVICE_UUID_128)
        if (data != null && data.isNotEmpty()) return data

        val raw = rec.getBytes()
        if (raw.isNotEmpty()) {
            val found = Decoder.parseAdServiceData(raw)[SERVICE_UUID_16BIT]
            if (found != null && found.isNotEmpty()) return found
        }
        return null
    }
}
