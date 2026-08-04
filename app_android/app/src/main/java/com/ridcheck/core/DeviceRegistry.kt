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

    /** 历史采样（环形，仅保留最近 DeviceRegistry.SAMPLE_CAP 条）。 */
    val samples = ArrayDeque<SamplePoint>()
}

/**
 * 所有信号源的注册表，按首次发现顺序有序。
 * 每台设备独立累积去重后的数据包，并独立做流式健康判定。
 */
class DeviceRegistry {
    companion object {
        /** 采样上限：10 分钟 @ 1Hz。 */
        const val SAMPLE_CAP = 600
    }

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

    /**
     * 每秒调用一次：为每台设备追加一个采样点（RSSI/速率/判定），超上限截断最旧。
     * 与主线程 ticker 同线程，无需加锁。
     */
    fun sampleAll(nowMs: Long) {
        for (entry in devices.values) {
            val last = entry.samples.lastOrNull()
            if (last != null && nowMs - last.timeMs < 1000) continue
            val rep = entry.assessor.report()
            entry.samples.addLast(SamplePoint(nowMs, entry.rssi, rep.avgRateHz, rep.level))
            while (entry.samples.size > SAMPLE_CAP) entry.samples.removeFirst()
        }
    }

    val list: List<DeviceEntry>
        get() = devices.values.toList()

    val size: Int
        get() = devices.size

    fun clear() = devices.clear()
}
