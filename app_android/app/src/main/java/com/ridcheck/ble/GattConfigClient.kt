package com.ridcheck.ble

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Handler
import android.os.Looper
import com.ridcheck.core.GattConfig
import com.ridcheck.core.RidConfigData
import java.util.UUID

/**
 * GATT 双向配置客户端：连接地面态模块，读取/写入 4 个身份字段 + 状态。
 * 与 PC 版 gui/workers.py 的 GattConfigWorker 行为对齐：
 * - 读：连上后依次读 FFF1~FFF5
 * - 写：先读 FFF5 判空态（空中写锁定拒绝），再写 FFF1~FFF4，最后读回确认
 * BluetoothGattCallback 运行于 binder 线程，所有回调经 main Handler 抛回主线程。
 */
class GattConfigClient(
    private val device: BluetoothDevice,
    private val listener: Listener
) {
    interface Listener {
        fun onLog(text: String)
        fun onRead(config: RidConfigData)
        fun onWritten(config: RidConfigData)
        fun onError(message: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null

    private enum class Action { READ, WRITE }
    private var action: Action? = null
    private var writeCfg: RidConfigData? = null

    /** 当前串行阶段的待执行 GATT 操作（读 = value 为 null，写 = 携带值）。 */
    private class Op(val uuid: String, val value: ByteArray?)

    private val ops = ArrayDeque<Op>()
    private var collected = RidConfigData()
    private var phase = ""  // "read" | "write_guard" | "write" | "read_back"
    private var done = false

    fun startRead(context: Context) {
        action = Action.READ
        connect(context)
    }

    fun startWrite(context: Context, cfg: RidConfigData) {
        action = Action.WRITE
        writeCfg = cfg
        connect(context)
    }

    fun close() {
        try {
            gatt?.disconnect()
        } catch (_: Exception) {
            // 已断开时忽略
        }
        gatt?.close()
        gatt = null
    }

    private fun connect(context: Context) {
        emitLog("连接 ${device.address} ...")
        try {
            gatt = device.connectGatt(context, false, callback)
        } catch (e: SecurityException) {
            emitError("缺少蓝牙权限，无法连接: ${e.message}")
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("连接异常 status=$status")
                    return
                }
                emitLog("已连接，发现服务...")
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                if (!done) {
                    done = true
                    emitError("连接中断（未完成）")
                } else {
                    emitLog("连接已断开")
                }
                close()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                fail("服务发现失败 status=$status")
                return
            }
            if (svc(gatt) == null) {
                fail("未找到 GATT 配置服务 0xFFF0（模块不在可配置态？）")
                return
            }
            when (action) {
                Action.READ -> beginRead(gatt)
                Action.WRITE -> beginWriteGuard(gatt)
                else -> fail("未指定操作")
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                fail("读取特征失败 status=$status")
                return
            }
            applyRead(characteristic)
            when (phase) {
                "read" -> {
                    if (ops.isEmpty()) {
                        done = true
                        emitRead(collected)
                        emitLog("读取完成 — 状态: ${collected.stateName}")
                        close()
                    } else {
                        processNext(gatt)
                    }
                }
                "write_guard" -> {
                    if (collected.isAirborne) {
                        fail("模块处于空中状态（写锁定），拒绝写入")
                        return
                    }
                    beginWrite(gatt)
                }
                "read_back" -> {
                    if (ops.isEmpty()) {
                        done = true
                        emitWritten(collected)
                        emitLog("写入完成并已读回确认")
                        close()
                    } else {
                        processNext(gatt)
                    }
                }
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                fail("写入特征失败 status=$status")
                return
            }
            if (ops.isEmpty()) {
                beginReadBack(gatt)
            } else {
                processNext(gatt)
            }
        }
    }

    private fun svc(gatt: BluetoothGatt) =
        gatt.getService(UUID.fromString(GattConfig.SERVICE_UUID_128))

    private fun beginRead(gatt: BluetoothGatt) {
        phase = "read"
        collected = RidConfigData()
        enqueueReads()
        processNext(gatt)
    }

    private fun beginWriteGuard(gatt: BluetoothGatt) {
        phase = "write_guard"
        collected = RidConfigData()
        val chr = svc(gatt)?.getCharacteristic(UUID.fromString(GattConfig.CHAR_STATE))
        if (chr == null) {
            fail("未找到状态特征 0xFFF5")
            return
        }
        gatt.readCharacteristic(chr)
    }

    private fun beginWrite(gatt: BluetoothGatt) {
        phase = "write"
        val cfg = writeCfg ?: return
        ops.clear()
        ops.addLast(Op(GattConfig.CHAR_UAS_ID, cfg.uasId.toByteArray(Charsets.US_ASCII)))
        ops.addLast(Op(GattConfig.CHAR_REALNAME, cfg.realname.toByteArray(Charsets.US_ASCII)))
        ops.addLast(Op(GattConfig.CHAR_OP_CATEGORY, byteArrayOf(cfg.opCategory.toByte())))
        ops.addLast(Op(GattConfig.CHAR_UA_CLASS, byteArrayOf(cfg.uaClass.toByte())))
        processNext(gatt)
    }

    private fun beginReadBack(gatt: BluetoothGatt) {
        phase = "read_back"
        collected = RidConfigData()
        enqueueReads()
        processNext(gatt)
    }

    private fun enqueueReads() {
        ops.clear()
        ops.addLast(Op(GattConfig.CHAR_UAS_ID, null))
        ops.addLast(Op(GattConfig.CHAR_REALNAME, null))
        ops.addLast(Op(GattConfig.CHAR_OP_CATEGORY, null))
        ops.addLast(Op(GattConfig.CHAR_UA_CLASS, null))
        ops.addLast(Op(GattConfig.CHAR_STATE, null))
    }

    private fun processNext(gatt: BluetoothGatt) {
        val op = ops.removeFirst()
        val chr = svc(gatt)?.getCharacteristic(UUID.fromString(op.uuid))
        if (chr == null) {
            fail("未找到特征 ${op.uuid.takeLast(4)}")
            return
        }
        if (op.value != null) {
            chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            chr.value = op.value
            gatt.writeCharacteristic(chr)
        } else {
            gatt.readCharacteristic(chr)
        }
    }

    private fun applyRead(characteristic: BluetoothGattCharacteristic) {
        val v = characteristic.value ?: ByteArray(0)
        collected = when (characteristic.uuid.toString()) {
            GattConfig.CHAR_UAS_ID -> collected.copy(uasId = asciiNoNull(v))
            GattConfig.CHAR_REALNAME -> collected.copy(realname = asciiNoNull(v))
            GattConfig.CHAR_OP_CATEGORY ->
                collected.copy(opCategory = if (v.isNotEmpty()) v[0].toInt() and 0xFF else -1)
            GattConfig.CHAR_UA_CLASS ->
                collected.copy(uaClass = if (v.isNotEmpty()) v[0].toInt() and 0xFF else -1)
            GattConfig.CHAR_STATE ->
                collected.copy(state = if (v.isNotEmpty()) v[0].toInt() and 0xFF else -1)
            else -> collected
        }
    }

    private fun asciiNoNull(bytes: ByteArray): String {
        var end = bytes.size
        while (end > 0 && (bytes[end - 1].toInt() and 0xFF) == 0) end--
        return String(bytes.copyOfRange(0, end), Charsets.US_ASCII)
    }

    private fun fail(message: String) {
        done = true
        emitError(message)
        close()
    }

    // ---- 主线程投递 ----
    private fun emitLog(text: String) = main.post { listener.onLog(text) }
    private fun emitRead(cfg: RidConfigData) = main.post { listener.onRead(cfg) }
    private fun emitWritten(cfg: RidConfigData) = main.post { listener.onWritten(cfg) }
    private fun emitError(message: String) = main.post { listener.onError(message) }
}
