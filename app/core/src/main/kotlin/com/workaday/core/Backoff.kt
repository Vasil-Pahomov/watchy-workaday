package com.workaday.core

import com.workaday.core.protocol.WatchProtocol
import kotlin.math.roundToLong
import kotlin.random.Random

/**
 * The ±20 % jitter of PROTOCOL.md §5.2, injected so a test can pin an exact
 * delay instead of asserting a range.
 *
 * A test that can only say "somewhere between 24 s and 36 s" cannot tell a
 * correct implementation from one that applies the jitter twice, or before the
 * cap instead of after. Pinning the value is the difference between a test that
 * documents the arithmetic and one that merely watches it happen.
 */
fun interface JitterSource {

    /**
     * A value in `[-1.0, +1.0]` scaling the jitter band: `-1` is the bottom of
     * the ±20 % window, `0` the nominal delay, `+1` the top. Values outside the
     * range are clamped by [Backoff] rather than trusted.
     */
    fun nextUnitJitter(): Double

    companion object {
        /** No jitter — nominal delays exactly. Tests only; never ship this. */
        val NONE: JitterSource = JitterSource { 0.0 }

        /** Uniform over the whole band. This is the production source. */
        fun uniform(random: Random = Random.Default): JitterSource =
            JitterSource { random.nextDouble(-1.0, 1.0) }
    }
}

/**
 * PROTOCOL.md §5.2's retry schedule: base 30 s, ×2 per attempt, capped at
 * 15 min, ±20 % jitter.
 *
 * **This class holds no attempt counter, on purpose.** §5.2 says backoff is
 * reset by a Status notify with `result == 0` and by nothing else — not by a
 * successful connect — and Law 2 says the same thing about the health counters.
 * Two counters obeying one rule is one counter too many: they drift, and the
 * drift is invisible until the app is quietly hot-looping in someone's pocket.
 * So the attempt index *is* [HealthSnapshot.retryAttemptIndex], the class that
 * already has to survive process death, and this one is a pure function of it.
 *
 * The cap is what keeps the app useful: 15 min is comfortably under the watch's
 * hourly window (§5.1), so however long the failure has been going on the app is
 * always armed again before the watch next appears.
 */
class Backoff(
    private val jitter: JitterSource,
    private val baseMillis: Long = WatchProtocol.BACKOFF_BASE_MS,
    private val capMillis: Long = WatchProtocol.BACKOFF_CAP_MS,
    private val jitterFraction: Double = WatchProtocol.BACKOFF_JITTER_FRACTION,
) {
    init {
        require(baseMillis > 0) { "baseMillis must be positive, was $baseMillis" }
        require(capMillis >= baseMillis) { "capMillis $capMillis is below baseMillis $baseMillis" }
        require(jitterFraction in 0.0..1.0) { "jitterFraction out of range: $jitterFraction" }
    }

    /**
     * The first attempt index whose nominal delay is the cap — 5, for §5.2's
     * numbers (30 s, 60, 120, 240, 480, then 960 → clamped to 900).
     *
     * Derived rather than written down so that "the backoff is pinned at the
     * cap" stays a single fact. [healthLevelFor] uses it to decide when the app
     * is [HealthLevel.Failing]; hard-coding a threshold there would be a second
     * copy of §5.2's arithmetic.
     */
    val attemptsToReachCap: Int = run {
        var delay = baseMillis
        var attempts = 0
        while (delay < capMillis) {
            delay *= 2
            attempts++
        }
        attempts
    }

    /**
     * The un-jittered delay for a 0-based [attempt]: `base × 2^attempt`, clamped
     * to the cap.
     *
     * Doubling in a loop with an early exit rather than shifting or `pow`: the
     * loop cannot overflow, because it stops as soon as the cap is reached, so
     * even `Int.MAX_VALUE` attempts returns the cap instead of a negative delay.
     * A negative delay would schedule a retry in the past — a hot loop, which is
     * the exact failure Law 2 forbids.
     */
    fun nominalDelayMillis(attempt: Int): Long {
        var delay = baseMillis
        var i = 0
        val target = attempt.coerceAtLeast(0)
        while (i < target && delay < capMillis) {
            delay *= 2
            i++
        }
        return delay.coerceAtMost(capMillis)
    }

    /**
     * [nominalDelayMillis] with §5.2's ±20 % applied.
     *
     * §5.2 fixes the order as well as the numbers: jitter applied **after** the
     * cap, so the band at the top of the ladder is 12–18 min rather than 12–15.
     * Still far under the hour the cap is chosen against, so the app is armed
     * before every window. The order is phone-side only — the watch cannot
     * observe it, so it cannot desynchronise the two sides.
     */
    fun delayMillisFor(attempt: Int): Long {
        val nominal = nominalDelayMillis(attempt)
        // A NaN or infinite jitter would blow up in roundToLong(). Treating a
        // nonsensical source as "no jitter" keeps a retry scheduled — the one
        // thing that must never fail to happen.
        val raw = jitter.nextUnitJitter()
        val unit = (if (raw.isFinite()) raw else 0.0).coerceIn(-1.0, 1.0)
        val offset = (nominal.toDouble() * jitterFraction * unit).roundToLong()
        return (nominal + offset).coerceAtLeast(0L)
    }
}
