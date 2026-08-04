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

    /** 结构错误包累计数（会话全程）。 */
    var structErrCount: Int = 0

    /** 历史采样（环形，仅保留最近 DeviceRegistry.SAMPLE_CAP 条）。 */
    val samples = ArrayDeque<SamplePoint>()

    /** 逐帧接收记录（环形，仅保留最近 DeviceRegistry.FRAME_CAP 条）。 */
    val frames = ArrayDeque<FrameRecord>()

    /** 表3 序号 1..21 → 该字段被携带过的帧数（会话全程累计，不随 frames 截断）。 */
    val fieldSeen = IntArray(22)

    /** 运行状态变化记录（opStatus 变化时追加，会话全程）。 */
    val statusLog = ArrayDeque<StatusLogEntry>()

    /** 问题时段记录器（由每秒快照喂入，报告输出「XX 问题在 XX~XX 出现」）。 */
    val issueTimeline = IssueTimeline()

    /** 表3 21 项会话统计（身份/枚举/范围/未知占比/坐标跨度等）。 */
    val sessionStats = SessionStats()

    /** 相对轨迹点（1Hz 采样，环形，仅保留最近 DeviceRegistry.TRACK_CAP 条）。 */
    val track = ArrayDeque<TrackPoint>()
}

/**
 * 所有信号源的注册表，按首次发现顺序有序。
 * 每台设备独立累积去重后的数据包，并独立做流式健康判定。
 */
class DeviceRegistry {
    companion object {
        /** 采样上限：10 分钟 @ 1Hz。 */
        const val SAMPLE_CAP = 600

        /** 逐帧存档上限（约 50 分钟 @ 10Hz；超出截断最旧）。 */
        const val FRAME_CAP = 30000

        /** 轨迹点上限：20 分钟 @ 1Hz；超出截断最旧。 */
        const val TRACK_CAP = 1200

        /** 运行状态变化记录上限。 */
        const val STATUS_LOG_CAP = 256
    }

    private val devices = LinkedHashMap<String, DeviceEntry>()

    /** 收录/更新一个数据包。返回对应的设备条目。 */
    fun onPacket(pkt: DecodedPacket, nowMs: Long): DeviceEntry {
        val entry = devices.getOrPut(pkt.address) { DeviceEntry(pkt.address) }
        val prevStatus = entry.lastPkt?.opStatus
        entry.rssi = pkt.rssi
        entry.lastSeenMs = nowMs
        if (entry.packetCount == 0) entry.firstSeenMs = nowMs
        entry.packetCount++
        entry.lastRaw = pkt.raw.copyOf()
        entry.lastPkt = pkt
        entry.assessor.push(pkt)
        if (pkt.structureError.isNotEmpty()) entry.structErrCount++
        entry.sessionStats.record(pkt)
        for (k in pkt.fieldHex.keys) {
            val idx = k.toIntOrNull()
            if (idx != null && idx in 1..21) entry.fieldSeen[idx]++
        }
        if (pkt.opStatus != prevStatus) {
            entry.statusLog.addLast(StatusLogEntry(nowMs, pkt.opStatus))
            while (entry.statusLog.size > STATUS_LOG_CAP) entry.statusLog.removeFirst()
        }
        entry.frames.addLast(FrameRecord(nowMs, pkt.rssi, pkt.raw.copyOf()))
        while (entry.frames.size > FRAME_CAP) entry.frames.removeFirst()
        return entry
    }

    /**
     * 每秒调用一次：为每台设备追加一个采样点（RSSI/速率/判定）、喂入问题时段记录器、
     * 追加相对轨迹点，超上限截断最旧。与主线程 ticker 同线程，无需加锁。
     */
    fun sampleAll(nowMs: Long) {
        for (entry in devices.values) {
            val last = entry.samples.lastOrNull()
            if (last != null && nowMs - last.timeMs < 1000) continue
            val rep = entry.assessor.report()
            entry.samples.addLast(SamplePoint(nowMs, entry.rssi, rep.avgRateHz, rep.level))
            while (entry.samples.size > SAMPLE_CAP) entry.samples.removeFirst()
            entry.issueTimeline.update(rep.issues, nowMs)
            sampleTrack(entry, nowMs)
        }
    }

    /** 有效位置 → 追加一个相对轨迹点（相对首点/起飞点），origin 由首个有效点确定。 */
    private fun sampleTrack(entry: DeviceEntry, nowMs: Long) {
        val pkt = entry.lastPkt ?: return
        if (pkt.uaLat.isNaN() || pkt.uaLon.isNaN()) return
        val origin = entry.track.firstOrNull()
        val refLat = origin?.lat ?: pkt.uaLat
        val refLon = origin?.lon ?: pkt.uaLon
        val relN = Geo.yMeters(pkt.uaLat, refLat)
        val relE = Geo.xMeters(pkt.uaLon, refLon, refLat)
        entry.track.addLast(TrackPoint(nowMs, pkt.uaLat, pkt.uaLon, relN, relE))
        while (entry.track.size > TRACK_CAP) entry.track.removeFirst()
    }

    val list: List<DeviceEntry>
        get() = devices.values.toList()

    val size: Int
        get() = devices.size

    fun clear() = devices.clear()

    /** 收尾：关闭所有设备当前进行中的问题时段（停止扫描时调用，报告不再显示「进行中」）。 */
    fun closeIssueTimelines(nowMs: Long) {
        for (entry in devices.values) entry.issueTimeline.closeOpen(nowMs)
    }
}
