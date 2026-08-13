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
 * 基于 BluetoothLeScanner 的统一扫描封装：一次扫描同时发现两类信号源。
 * 行为对齐 PC 版 rid/ble_scanner.py + gui/workers.py：
 * - 全接受扫描，回调内手动按报文字节分类（不依赖广播名）
 *   - Service Data UUID 0xFFFF → 空中态广播信号源（纯 GB 包）
 *   - Service Class UUID 0xFFF0 → 地面态可 GATT 配置模块
 * - 相同原始包去重（固件每事件在 3 个信道重发同包）
 * - 回调在主线程 → 可直接刷新 UI
 */
class BleScanner(private val listener: Listener) {

    interface Listener {
        /** 收到一个去重后的原始 GB 包（主线程）。 */
        fun onDistinctPacket(address: String, rssi: Int, raw: ByteArray)

        /** 发现一个可 GATT 配置的模块（地面态，主线程）。 */
        fun onConfigDevice(address: String, rssi: Int, name: String?)

        fun onScanState(scanning: Boolean)

        fun onLog(text: String)
    }

    companion object {
        const val SERVICE_UUID_16BIT = 0xFFFF
        val SERVICE_UUID_128: ParcelUuid =
            ParcelUuid.fromString("0000ffff-0000-1000-8000-00805f9b34fb")

        const val CONFIG_SERVICE_UUID_16BIT = 0xFFF0
        val CONFIG_SERVICE_UUID_128: ParcelUuid =
            ParcelUuid.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
    }

    private var adapter: BluetoothAdapter? = null
    // 每台设备独立的最后原始包（用于去重：固件每事件在 3 个信道重发同包）
    private val perDeviceLastRaw = HashMap<String, ByteArray>()
    private val seenDevices = HashSet<String>()
    private val seenConfigDevices = HashSet<String>()

    var isScanning: Boolean = false
        private set

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val rec = result.scanRecord ?: return
            val address = try {
                result.device.address
            } catch (e: SecurityException) {
                return
            }
            val name = try {
                result.device.name
            } catch (e: SecurityException) {
                null
            }

            if (isBroadcastTarget(rec)) {
                val raw = extractPacket(rec) ?: return
                val prev = perDeviceLastRaw[address]
                if (prev != null && raw.contentEquals(prev)) return
                perDeviceLastRaw[address] = raw.copyOf()
                if (seenDevices.add(address)) {
                    listener.onLog("首次发现设备: $address  RSSI=${result.rssi}  名称=${name ?: "(无)"}")
                }
                listener.onDistinctPacket(address, result.rssi, raw)
            } else if (isConfigTarget(rec)) {
                if (seenConfigDevices.add(address)) {
                    listener.onLog("发现可配置模块: $address  RSSI=${result.rssi}  名称=${name ?: "(无)"}")
                }
                listener.onConfigDevice(address, result.rssi, name)
            }
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
        perDeviceLastRaw.clear()
        // setLegacy(false)：接收 BLE5 扩展广播。地面态传统可连接广播（0xFFF0）与
        // 空中态扩展广播（0xFFFF）在扩展扫描下都能到达回调，一次扫描两类信号源。
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
        listener.onLog("蓝牙扫描已开始，等待广播（0xFFFF）或可配置模块（0xFFF0）...")
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

    /** 是否来自空中态广播：含 UUID 0xFFFF 的 Service Data（或原始 AD 字节可解析出 0xFFFF）。 */
    private fun isBroadcastTarget(rec: ScanRecord): Boolean {
        val sd = rec.getServiceData()
        if (sd != null && sd.containsKey(SERVICE_UUID_128)) return true

        val uuids = rec.serviceUuids
        if (uuids != null && uuids.contains(SERVICE_UUID_128)) return true

        val raw = rec.getBytes()
        return raw.isNotEmpty() &&
            Decoder.parseAdServiceData(raw).containsKey(SERVICE_UUID_16BIT)
    }

    /** 是否来自地面态可配置模块：广播 16-bit Service Class UUID 0xFFF0（不依赖广播名）。 */
    private fun isConfigTarget(rec: ScanRecord): Boolean {
        val uuids = rec.serviceUuids
        if (uuids != null && uuids.contains(CONFIG_SERVICE_UUID_128)) return true

        val raw = rec.getBytes()
        return raw.isNotEmpty() &&
            Decoder.parseAdServiceUuids(raw).contains(CONFIG_SERVICE_UUID_16BIT)
    }

    /** 提取原始 GB 包：优先归一化 Service Data，兜底解析原始 AD 字节。
     *  固件广播为纯 GB 包（dataType 0xFF 起首，无 ASTM 头），直接返回。 */
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
