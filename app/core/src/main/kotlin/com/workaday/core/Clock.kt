package com.workaday.core

/**
 * The only way time enters `core/`.
 *
 * Law 3 forbids `System.currentTimeMillis()` here, and the reason is not purity
 * for its own sake: a clock read hidden inside a decision makes that decision
 * untestable, and this app's whole job is to be correct on the day the phone's
 * clock jumps, the timezone changes, or Doze stretches a wait to twenty minutes.
 *
 * Two kinds of time, deliberately separated because they fail differently:
 *
 * - [monotonicMillis] never goes backwards and never jumps. It is what durations
 *   and deadlines are measured with. On Android it is `SystemClock.elapsedRealtime()`,
 *   which also keeps counting through suspend — the one that matters when the
 *   phone is asleep between a connect and a callback.
 * - [utcEpochSeconds] / [utcOffsetMinutes] are the wall clock. They are what the
 *   user set, what NTP corrected, and what goes on the wire in a §3.1 Time
 *   payload. They can and do jump in both directions, so nothing in `core/`
 *   measures a duration with them.
 */
interface Clock {

    /**
     * A monotonically non-decreasing millisecond counter with an arbitrary
     * origin. Only differences are meaningful.
     *
     * Callers still clamp differences at zero: "monotonic" is a promise from the
     * platform, and an app that lives for months does not stake a stall on it.
     */
    fun monotonicMillis(): Long

    /** Wall clock, seconds since 1970-01-01T00:00:00Z. */
    fun utcEpochSeconds(): Long

    /**
     * The current local offset from UTC in minutes, DST already included —
     * `local = utc + offset·60`, exactly as PROTOCOL.md §3.1 defines it.
     *
     * The watch has no timezone database and must never try to derive one; a DST
     * transition reaches it as a different offset on the next sync.
     */
    fun utcOffsetMinutes(): Int
}
