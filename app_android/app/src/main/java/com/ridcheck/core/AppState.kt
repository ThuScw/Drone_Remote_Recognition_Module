package com.ridcheck.core

/**
 * 全局共享状态：前台扫描服务与 UI 活动共用同一个注册表与日志。
 * 所有读写都在主线程（服务回调、1Hz ticker、UI 刷新均跑在主 Looper），无需加锁。
 * 进程内单例——后台服务持续记录期间，重新打开 App 仍能读到同一份数据。
 */
object AppState {
    const val MAX_LOG_LINES = 200

    val registry = DeviceRegistry()

    /** 是否正在扫描（由 RidScanService 维护）。 */
    var scanning = false

    private val logLines = ArrayDeque<String>()

    val logText: String get() = logLines.joinToString("\n")

    fun addLog(line: String) {
        logLines.addLast(line)
        while (logLines.size > MAX_LOG_LINES) logLines.removeFirst()
    }
}
