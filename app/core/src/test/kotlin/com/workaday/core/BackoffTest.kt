package com.workaday.core

import com.workaday.core.protocol.WatchProtocol
import kotlin.random.Random
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * PROTOCOL.md §5.2's retry schedule, pinned to exact numbers.
 *
 * The jitter source is injected so every expectation here is an equality rather
 * than a range: a range-based test cannot tell "jitter applied once" from
 * "jitter applied twice", or "cap then jitter" from "jitter then cap".
 */
class BackoffTest {

    private fun backoff(jitter: JitterSource) = Backoff(jitter)

    @Test
    fun `attempt 0 is the section 5_2 base delay`() {
        assertEquals(30_000L, backoff(JitterSource.NONE).delayMillisFor(0))
        assertEquals(WatchProtocol.BACKOFF_BASE_MS, backoff(JitterSource.NONE).delayMillisFor(0))
    }

    @Test
    fun `attempt 0 with jitter at both extremes`() {
        assertEquals(24_000L, backoff { -1.0 }.delayMillisFor(0))
        assertEquals(36_000L, backoff { 1.0 }.delayMillisFor(0))
        assertEquals(33_000L, backoff { 0.5 }.delayMillisFor(0))
    }

    @Test
    fun `the nominal ladder doubles until it reaches the cap`() {
        val b = backoff(JitterSource.NONE)
        assertEquals(30_000L, b.nominalDelayMillis(0))
        assertEquals(60_000L, b.nominalDelayMillis(1))
        assertEquals(120_000L, b.nominalDelayMillis(2))
        assertEquals(240_000L, b.nominalDelayMillis(3))
        assertEquals(480_000L, b.nominalDelayMillis(4))
        // 960 000 would exceed the 15 min cap.
        assertEquals(900_000L, b.nominalDelayMillis(5))
    }

    @Test
    fun `attempt at cap, and every attempt past it, stays at the cap`() {
        val b = backoff(JitterSource.NONE)
        assertEquals(5, b.attemptsToReachCap)
        assertEquals(WatchProtocol.BACKOFF_CAP_MS, b.nominalDelayMillis(b.attemptsToReachCap))
        for (attempt in listOf(5, 6, 10, 64, 1_000, Int.MAX_VALUE)) {
            assertEquals(900_000L, b.nominalDelayMillis(attempt), "attempt $attempt")
        }
    }

    @Test
    fun `at the cap the jitter band is 12 to 18 minutes, still far under the hourly window`() {
        assertEquals(720_000L, backoff { -1.0 }.delayMillisFor(5))
        assertEquals(900_000L, backoff { 0.0 }.delayMillisFor(5))
        assertEquals(1_080_000L, backoff { 1.0 }.delayMillisFor(5))
        // Section 5.2 chooses the cap so the app is always armed again before the
        // watch's next hourly window. 18 min is the worst case and clears it.
        assertTrue(1_080_000L < 60 * 60 * 1000L)
    }

    @Test
    fun `a huge attempt index can never produce a negative or zero delay`() {
        // Shifting or pow() here would overflow to a negative delay, which
        // schedules a retry in the past: a hot loop, and a battery fire on both
        // sides. Doubling with an early exit cannot.
        val b = backoff { -1.0 }
        for (attempt in listOf(31, 32, 33, 62, 63, 64, 1_000_000, Int.MAX_VALUE)) {
            assertEquals(720_000L, b.delayMillisFor(attempt), "attempt $attempt")
        }
    }

    @Test
    fun `a negative attempt index is treated as attempt 0`() {
        assertEquals(30_000L, backoff(JitterSource.NONE).delayMillisFor(-1))
        assertEquals(30_000L, backoff(JitterSource.NONE).delayMillisFor(Int.MIN_VALUE))
    }

    @Test
    fun `a jitter source outside the unit band is clamped, not trusted`() {
        assertEquals(36_000L, backoff { 99.0 }.delayMillisFor(0))
        assertEquals(24_000L, backoff { -99.0 }.delayMillisFor(0))
    }

    @Test
    fun `a nonsensical jitter source still yields a schedulable delay`() {
        // Nothing in production produces these, but "the retry was never
        // scheduled because roundToLong threw" is the one outcome Law 2 has no
        // answer for.
        assertEquals(30_000L, backoff { Double.NaN }.delayMillisFor(0))
        assertEquals(30_000L, backoff { Double.POSITIVE_INFINITY }.delayMillisFor(0))
        assertEquals(30_000L, backoff { Double.NEGATIVE_INFINITY }.delayMillisFor(0))
    }

    @Test
    fun `the production jitter source stays inside the band over many draws`() {
        val b = backoff(JitterSource.uniform(Random(20260817)))
        var sawBelowNominal = false
        var sawAboveNominal = false
        repeat(10_000) {
            val delay = b.delayMillisFor(0)
            assertTrue(delay in 24_000L..36_000L, "out of band: $delay")
            if (delay < 30_000L) sawBelowNominal = true
            if (delay > 30_000L) sawAboveNominal = true
        }
        // A jitter source that always returned 0.0 would satisfy the range check.
        assertTrue(sawBelowNominal && sawAboveNominal, "jitter is not actually varying")
    }

    @Test
    fun `attemptsToReachCap is derived from base and cap, not written down`() {
        assertEquals(0, Backoff(JitterSource.NONE, baseMillis = 900_000, capMillis = 900_000).attemptsToReachCap)
        assertEquals(1, Backoff(JitterSource.NONE, baseMillis = 500, capMillis = 1_000).attemptsToReachCap)
        assertEquals(10, Backoff(JitterSource.NONE, baseMillis = 1, capMillis = 1_024).attemptsToReachCap)
    }

    @Test
    fun `nonsense configuration is refused at construction`() {
        assertFailsWith<IllegalArgumentException> { Backoff(JitterSource.NONE, baseMillis = 0) }
        assertFailsWith<IllegalArgumentException> { Backoff(JitterSource.NONE, baseMillis = -1) }
        assertFailsWith<IllegalArgumentException> {
            Backoff(JitterSource.NONE, baseMillis = 60_000, capMillis = 30_000)
        }
        assertFailsWith<IllegalArgumentException> {
            Backoff(JitterSource.NONE, jitterFraction = 1.5)
        }
    }

    @Test
    fun `Backoff holds no state of its own, so nothing can reset it by accident`() {
        // The attempt index lives in HealthSnapshot, which is the thing that
        // survives process death. Calling this a hundred times does not advance
        // anything.
        val b = backoff(JitterSource.NONE)
        repeat(100) { assertEquals(30_000L, b.delayMillisFor(0)) }
    }
}
