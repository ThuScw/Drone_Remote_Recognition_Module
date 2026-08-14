package com.ridcheck.core

/**
 * 全局共享状态：前台扫描服务与 UI 活动共用同一个注册表与日志。
 * 所有读写都在主线程（服务回调、1Hz ticker、UI 刷新均跑在主 Looper），无需加锁。
 * 进程内单例——后台服务持续记录期间，重新打开 App 仍能读到同一份数据。
 */
object AppState {
    const val MAX_LOG_LINES = 200

    /** 可配置模块条目无广播刷新的存活时间；超过则从列表移除（如模块转空中态或关机）。 */
    private const val CONFIG_DEVICE_TTL_MS = 10_000L

    val registry = DeviceRegistry()

    /** 可 GATT 配置的模块（地面态，广播 0xFFF0），按 MAC 有序。 */
    private val configDevices = LinkedHashMap<String, ConfigDevice>()

    /** 是否正在扫描（由 RidScanService 维护）。 */
    var scanning = false

    private val logLines = ArrayDeque<String>()

    val logText: String get() = logLines.joinToString("\n")

    fun addLog(line: String) {
        logLines.addLast(line)
        while (logLines.size > MAX_LOG_LINES) logLines.removeFirst()
    }

    /** 收录/更新一个可配置模块。同时将该地址从广播信号源注册表移除：模块此刻
     *  处于地面配置态（不广播 GB 包），不应再出现在「正在广播的信号源」列表。 */
    fun onConfigDevice(address: String, rssi: Int, name: String?) {
        registry.remove(address)
        val d = configDevices.getOrPut(address) { ConfigDevice(address) }
        d.rssi = rssi
        if (name != null) d.name = name
        val now = System.currentTimeMillis()
        if (d.firstSeenMs == 0L) d.firstSeenMs = now
        d.lastSeenMs = now
    }

    /** 可配置模块列表，过滤掉超过存活时间（TTL）的过期条目。 */
    val configDeviceList: List<ConfigDevice>
        get() {
            val now = System.currentTimeMillis()
            return configDevices.values.filter { now - it.lastSeenMs < CONFIG_DEVICE_TTL_MS }
        }
}
