package com.ridcheck.core

/** 一个健康问题的一次发作时段。endMs = -1 表示仍在进行中（时段未关闭）。 */
data class IssueEpisode(
    val code: String,
    val level: HealthLevel,
    val message: String,
    val startMs: Long,
    val endMs: Long = -1L
) {
    val isOpen: Boolean get() = endMs < 0

    /** 持续时长（毫秒）；进行中的时段以 nowMs 为终点。 */
    fun durationMs(nowMs: Long): Long =
        if (isOpen) maxOf(0L, nowMs - startMs) else maxOf(0L, endMs - startMs)
}

/**
 * 按 issue.code 记录每次问题出现/消失的时间区间（时段）。
 * 由每秒一次的 sampleAll 喂入当次快照的问题集合：问题出现 → 开时段，消失 → 关时段。
 * 报告据此给出「XX 问题在 XX~XX 出现」的时间轴。
 * 纯 JVM；时段对象不可变，关闭/更新通过替换实现。
 */
class IssueTimeline(
    private val cap: Int = 512,
    private val nowFunc: () -> Long = { System.currentTimeMillis() }
) {
    private val episodes = ArrayList<IssueEpisode>()
    private val openByCode = LinkedHashMap<String, IssueEpisode>()

    val size: Int get() = episodes.size

    /** 以当前快照的问题集合开/闭时段。 */
    fun update(issues: List<HealthIssue>, nowMs: Long = nowFunc()) {
        val present = HashSet<String>()
        for (issue in issues) {
            present.add(issue.code)
            val open = openByCode[issue.code]
            if (open == null) {
                val ep = IssueEpisode(issue.code, issue.level, issue.message, nowMs)
                episodes.add(ep)
                openByCode[issue.code] = ep
            } else if (open.message != issue.message) {
                // 保留起始时间，仅更新最新描述（替换保持不可变）
                val idx = episodes.indexOf(open)
                val updated = open.copy(message = issue.message)
                if (idx >= 0) episodes[idx] = updated
                openByCode[issue.code] = updated
            }
        }
        val it = openByCode.entries.iterator()
        while (it.hasNext()) {
            val e = it.next()
            if (e.key !in present) {
                val idx = episodes.indexOf(e.value)
                if (idx >= 0) episodes[idx] = e.value.copy(endMs = nowMs)
                it.remove()
            }
        }
        trim()
    }

    /** 全部时段（进行中的 endMs=-1，未关闭）。 */
    fun snapshot(): List<IssueEpisode> = episodes.toList()

    /** 收尾：关闭全部进行中的时段（生成报告前调用）。 */
    fun closeOpen(nowMs: Long = nowFunc()) {
        val toClose = openByCode.values.toList()
        for (ep in toClose) {
            val idx = episodes.indexOf(ep)
            if (idx >= 0) episodes[idx] = ep.copy(endMs = nowMs)
        }
        openByCode.clear()
    }

    fun clear() {
        episodes.clear()
        openByCode.clear()
    }

    private fun trim() {
        while (episodes.size > cap) {
            val removed = episodes.removeAt(0)
            if (removed.isOpen && openByCode[removed.code] === removed) {
                openByCode.remove(removed.code)
            }
        }
    }
}
