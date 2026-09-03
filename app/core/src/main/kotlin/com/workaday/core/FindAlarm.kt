package com.workaday.core

/**
 * How loud the find-phone alarm is, as a function of how long it has been ringing.
 *
 * The brief is "starts quiet and rises to full volume": a phone that is on the
 * desk next to the user should not open at maximum, and one that is in a coat in
 * another room needs to reach maximum. The shape is a straight line from
 * [START_VOLUME] at the first instant to `1.0` at [RAMP_MS], flat afterwards.
 *
 * A pure function of elapsed time rather than a counter the Android layer steps,
 * so that a ramp step which fires late — a busy handler, a suspended CPU — lands
 * on the volume the clock says rather than one step further along than the last.
 * `app/` applies it to the player every [STEP_MS]; the step is presentation, the
 * curve is the decision, and only the curve is tested.
 */
object FindAlarm {

    /** Twenty seconds from quiet to full. Long enough to notice, short enough to find. */
    const val RAMP_MS: Long = 20_000

    /** Where the ramp starts. Not silent: the very first second should already be audible. */
    const val START_VOLUME: Float = 0.15f

    /** How often `app/` re-applies [volumeAt] to the player while the ramp is rising. */
    const val STEP_MS: Long = 1_000

    /**
     * The player volume, in `0.0..1.0`, [elapsedMillis] after the alarm started.
     *
     * Total: a negative elapsed — a clock that stepped — is the start, and anything
     * past the ramp is full. Never below [START_VOLUME] and never above `1.0`, so a
     * caller can hand it straight to a volume setter.
     */
    fun volumeAt(elapsedMillis: Long): Float {
        if (elapsedMillis <= 0L) return START_VOLUME
        if (elapsedMillis >= RAMP_MS) return 1.0f
        val fraction = elapsedMillis.toFloat() / RAMP_MS.toFloat()
        return (START_VOLUME + (1.0f - START_VOLUME) * fraction).coerceIn(START_VOLUME, 1.0f)
    }
}
