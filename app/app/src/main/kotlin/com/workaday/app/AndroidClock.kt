package com.workaday.app

import android.os.SystemClock
import com.workaday.core.Clock
import java.util.TimeZone

/**
 * The real clock behind `core/`'s [Clock].
 *
 * This exists so that `core/` can hold every decision without holding a single
 * wall-clock read (Law 3). It is the whole of the Android side of time, and it is
 * deliberately three one-line functions with nothing to get wrong.
 *
 * The two halves are different on purpose, because they fail differently:
 *
 * - [monotonicMillis] is `elapsedRealtime()`, **not** `uptimeMillis()`. It keeps
 *   counting while the phone is suspended, which is the only version of "elapsed"
 *   that means anything to PROTOCOL.md §5.2's 15 s exchange budget on a device
 *   that may sleep between two GATT callbacks.
 * - [utcEpochSeconds] and [utcOffsetMinutes] are the wall clock — what the user
 *   set, what NTP corrected — and they are what goes on the wire in a §3.1 Time
 *   payload. They can jump in both directions, which is why nothing in `core/`
 *   measures a duration with them.
 */
internal object AndroidClock : Clock {

    override fun monotonicMillis(): Long = SystemClock.elapsedRealtime()

    override fun utcEpochSeconds(): Long = System.currentTimeMillis() / 1_000L

    /**
     * `local = utc + offset·60`, exactly as §3.1 defines it, with DST already
     * folded in — `getOffset` returns the total offset in effect at that instant.
     *
     * Read fresh every time rather than cached: the zone changes when the user
     * travels, and a DST transition reaches the watch as a different offset on
     * the next sync, which is the only mechanism it has (it carries no timezone
     * database and must never try to derive one).
     */
    override fun utcOffsetMinutes(): Int =
        TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000
}
