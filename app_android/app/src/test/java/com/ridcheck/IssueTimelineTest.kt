package com.ridcheck

import com.ridcheck.core.HealthIssue
import com.ridcheck.core.HealthLevel
import com.ridcheck.core.IssueTimeline
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** 问题时段记录器测试：开/闭/重开、最新描述、closeOpen、容量上限。 */
class IssueTimelineTest {

    private fun issue(code: String, level: HealthLevel = HealthLevel.WARN, message: String = "msg-$code") =
        HealthIssue(level, code, message, "clause")

    @Test
    fun opensEpisodeWhenIssueAppears() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("STALE")), nowMs = 1000)
        val eps = tl.snapshot()
        assertEquals(1, eps.size)
        assertEquals("STALE", eps[0].code)
        assertEquals(1000, eps[0].startMs)
        assertTrue(eps[0].isOpen)
    }

    @Test
    fun closesEpisodeWhenIssueDisappears() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("STALE")), nowMs = 1000)
        tl.update(emptyList(), nowMs = 3000)
        val eps = tl.snapshot()
        assertEquals(1, eps.size)
        assertFalse(eps[0].isOpen)
        assertEquals(3000, eps[0].endMs)
        assertEquals(2000, eps[0].durationMs(3000))
    }

    @Test
    fun reopensEpisodeOnSecondAppearance() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("STALE")), nowMs = 1000)
        tl.update(emptyList(), nowMs = 3000)
        tl.update(listOf(issue("STALE")), nowMs = 5000)
        val eps = tl.snapshot()
        assertEquals(2, eps.size)
        assertFalse(eps[0].isOpen)
        assertTrue(eps[1].isOpen)
        assertEquals(1000, eps[0].startMs)
        assertEquals(5000, eps[1].startMs)
    }

    @Test
    fun keepsLatestMessageWhileOpen() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("RATE_LOW")), nowMs = 1000)
        tl.update(listOf(issue("RATE_LOW", message = "广播速率仅 0.3 包/s")), nowMs = 2000)
        val eps = tl.snapshot()
        assertEquals(1, eps.size)
        assertEquals("广播速率仅 0.3 包/s", eps[0].message)
        assertEquals(1000, eps[0].startMs)
    }

    @Test
    fun closesAllOpenOnCloseOpen() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("STALE"), issue("RATE_ZERO")), nowMs = 1000)
        tl.closeOpen(9000)
        val eps = tl.snapshot()
        assertEquals(2, eps.size)
        assertTrue(eps.all { !it.isOpen })
        assertTrue(eps.all { it.endMs == 9000L })
    }

    @Test
    fun capsEpisodeCount() {
        val tl = IssueTimeline(cap = 3)
        // 5 个 code 依次出现又消失 → 只保留最近 3 条
        for (i in 1..5) {
            tl.update(listOf(issue("C$i")), nowMs = i * 1000L)
            tl.update(emptyList(), nowMs = i * 1000L + 500)
        }
        assertEquals(3, tl.size)
    }

    @Test
    fun distinctCodesTrackedIndependently() {
        val tl = IssueTimeline()
        tl.update(listOf(issue("RATE_LOW"), issue("STALE")), nowMs = 1000)
        tl.update(listOf(issue("RATE_LOW")), nowMs = 2000) // STALE 消失，RATE_LOW 保持
        val eps = tl.snapshot()
        assertEquals(2, eps.size)
        val rateLow = eps.first { it.code == "RATE_LOW" }
        val stale = eps.first { it.code == "STALE" }
        assertTrue(rateLow.isOpen)
        assertFalse(stale.isOpen)
        assertEquals(2000, stale.endMs)
    }
}
