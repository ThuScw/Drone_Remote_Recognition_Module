package com.ridcheck.core

/** 单个 RID 信号源（按 MAC 地址）的累计状态。 */
class DeviceEntry(val address: String) {
    var rssi: Int = 0
    var lastSeenMs: Long = 0
    var firstSeenMs: Long = 0
    var packetCount: Int = 0
    var lastRaw: ByteArray = ByteArray(0)
    var lastPkt: DecodedPacket? = null
    val assessor = StreamAssessor()
}

/**
 * 所有信号源的注册表，按首次发现顺序有序。
 * 每台设备独立累积去重后的数据包，并独立做流式健康判定。
 */
class DeviceRegistry {
    private val devices = LinkedHashMap<String, DeviceEntry>()

    /** 收录/更新一个数据包。返回对应的设备条目。 */
    fun onPacket(pkt: DecodedPacket, nowMs: Long): DeviceEntry {
        val entry = devices.getOrPut(pkt.address) { DeviceEntry(pkt.address) }
        entry.rssi = pkt.rssi
        entry.lastSeenMs = nowMs
        if (entry.packetCount == 0) entry.firstSeenMs = nowMs
        entry.packetCount++
        entry.lastRaw = pkt.raw.copyOf()
        entry.lastPkt = pkt
        entry.assessor.push(pkt)
        return entry
    }

    val list: List<DeviceEntry>
        get() = devices.values.toList()

    val size: Int
        get() = devices.size

    fun clear() = devices.clear()
}
