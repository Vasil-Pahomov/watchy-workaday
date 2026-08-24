package com.workaday.core

/**
 * How an attempt to exchange with the watch ended.
 *
 * One value per row of PROTOCOL.md §6.2 that the app can distinguish, so the
 * diagnostic screen can say *why* rather than "not working", and so a test can
 * assert which row fired.
 *
 * Names go on disk (see [HealthSnapshot.encode]), so they are renamed only
 * deliberately — a rename is a format change, and [HealthSnapshot.decode] treats
 * an unrecognised name as "no recorded outcome" rather than failing.
 */
enum class ExchangeOutcome {
    /** §4: a Status notify with `result == 0`. The only thing that counts. */
    Succeeded,

    /** A well-formed Status notify with a non-zero, or unrecognised, `result`. */
    WatchReportedFailure,

    /** A notify that was not a §3.2 Status frame at all. */
    MalformedStatus,

    /** §6.2 "Operation timeout", or the §5.2 whole-exchange backstop expiring. */
    OperationTimedOut,

    /** §6.2 "Disconnect mid-exchange". */
    DisconnectedMidExchange,

    /** A GATT callback reported a non-zero status — discovery, CCCD or Time write. */
    GattOperationFailed,

    /** §6.2 "status 133": the link never came up, or dropped while merely armed. */
    ConnectionAttemptFailed,

    /**
     * The phone's own wall clock is outside what §3.1's layout can carry, so
     * there was nothing honest to send. Not the watch's fault, but still a
     * failed exchange — the watch's clock did not get set.
     */
    LocalClockUnusable,
}

/**
 * How worried the diagnostic screen should be.
 *
 * Derived, never stored: see [healthLevelFor].
 */
enum class HealthLevel {
    /**
     * Nothing has ever been recorded: a fresh install, or one whose counters were
     * lost.
     *
     * Kept apart from [Healthy] because *zero consecutive failures* is true of
     * both, and only one of them means the watch has ever answered. Collapsing the
     * two is what put "Health: the last exchange succeeded" on a first-launch
     * screen, directly between "Last sync: never" and "Last attempt: none recorded
     * yet" — each line defensible alone, the middle one false in place.
     */
    NothingRecorded,

    /** The last exchange succeeded. */
    Healthy,

    /** Failing, but the backoff is still growing — this is what a watch that is
     *  simply out of range looks like, and it is not a fault. */
    Degraded,

    /** Failing long enough that the backoff is pinned at its §5.2 cap. */
    Failing,
}

/**
 * The persisted half of Law 2's "health is counted and persisted across process
 * death, and escalates into longer backoff instead of crash-looping".
 *
 * Immutable, and made of nothing but primitives and one enum name, because APP-3
 * has to write it to disk and Law 3 says the *deciding* happens here while the
 * *writing* happens there. [encode] / [decode] are the whole storage contract:
 * APP-3 puts one string somewhere durable and hands it back at startup. No
 * schema, no parsing logic, and no chance of a half-migrated field living in the
 * Android layer where no test can reach it.
 */
data class HealthSnapshot(
    /**
     * Consecutive failed exchanges. This is also the retry attempt counter —
     * see [Backoff] for why there is deliberately only one.
     *
     * Cleared by a Status notify with `result == 0` and by nothing else (§5.2).
     */
    val consecutiveFailures: Int = 0,

    /** Lifetime failures. Diagnostic; nothing branches on it. */
    val totalFailures: Long = 0,

    /** Lifetime successes. Diagnostic; nothing branches on it. */
    val totalSuccesses: Long = 0,

    /** Wall clock of the last success; 0 means "never" (1970 is not a plausible sync). */
    val lastSuccessUtcEpochSeconds: Long = 0,

    /** Wall clock of the last recorded outcome, success or failure; 0 means never. */
    val lastOutcomeUtcEpochSeconds: Long = 0,

    /** The last recorded outcome, or null if nothing has been recorded yet. */
    val lastOutcome: ExchangeOutcome? = null,

    /**
     * The watch's `result` byte from the last exchange that produced one, or
     * [NO_RESULT_CODE]. §6.2 requires this to be surfaced — `BadVersion` in
     * particular means the two sides have drifted and no amount of retrying will
     * help, which is only actionable if the user can see it.
     */
    val lastStatusResultCode: Int = NO_RESULT_CODE,
) {
    /**
     * The 0-based attempt index to hand [Backoff.delayMillisFor].
     *
     * The first failure is attempt 0 and therefore the base delay; the floor
     * means a caller that asks before recording anything gets the base delay
     * rather than a negative index.
     */
    val retryAttemptIndex: Int get() = (consecutiveFailures - 1).coerceAtLeast(0)

    /**
     * How long since the last success, or null if there has never been one.
     *
     * Clamped at zero: the wall clock can jump backwards — NTP, the user, a
     * timezone database update — and "synced −4000 s ago" on the diagnostic
     * screen is worse than "synced just now".
     */
    fun secondsSinceLastSuccess(nowUtcEpochSeconds: Long): Long? =
        if (lastSuccessUtcEpochSeconds == 0L) {
            null
        } else {
            (nowUtcEpochSeconds - lastSuccessUtcEpochSeconds).coerceAtLeast(0L)
        }

    /**
     * One line, version-tagged, no dependencies. APP-3 writes the string and
     * reads it back; everything about the format lives here.
     */
    fun encode(): String = listOf(
        FORMAT_VERSION.toString(),
        consecutiveFailures.toString(),
        totalFailures.toString(),
        totalSuccesses.toString(),
        lastSuccessUtcEpochSeconds.toString(),
        lastOutcomeUtcEpochSeconds.toString(),
        lastOutcome?.name ?: NO_OUTCOME,
        lastStatusResultCode.toString(),
    ).joinToString(SEPARATOR)

    companion object {
        /** [lastStatusResultCode] when the watch never got as far as answering. */
        const val NO_RESULT_CODE: Int = -1

        /** Bumped only if the field list changes. [decode] discards other versions. */
        const val FORMAT_VERSION: Int = 1

        private const val SEPARATOR = "|"
        private const val NO_OUTCOME = "-"
        private const val FIELD_COUNT = 8

        /**
         * The inverse of [encode], and deliberately total: **it never throws and
         * never returns garbage.**
         *
         * Null (nothing stored yet), a truncated write, a file from a future
         * format version, or bytes from a different app all decode to a fresh
         * zeroed snapshot. That is the firmware's magic-and-version check in
         * `core::health` applied to a different medium, and for the same reason:
         * losing the counters costs one extra retry, while throwing at startup
         * costs the service — and a service that will not start is exactly the
         * silent stall Law 2 exists to refuse.
         */
        fun decode(text: String?): HealthSnapshot {
            val parts = text?.split(SEPARATOR) ?: return HealthSnapshot()
            if (parts.size != FIELD_COUNT) return HealthSnapshot()
            if (parts[0].toIntOrNull() != FORMAT_VERSION) return HealthSnapshot()

            val consecutive = parts[1].toIntOrNull() ?: return HealthSnapshot()
            val totalFailures = parts[2].toLongOrNull() ?: return HealthSnapshot()
            val totalSuccesses = parts[3].toLongOrNull() ?: return HealthSnapshot()
            val lastSuccess = parts[4].toLongOrNull() ?: return HealthSnapshot()
            val lastOutcomeAt = parts[5].toLongOrNull() ?: return HealthSnapshot()
            val resultCode = parts[7].toIntOrNull() ?: return HealthSnapshot()

            // An unknown name means a build that recorded an outcome this one has
            // never heard of. The counters are still meaningful, so keep them and
            // drop only the label.
            val outcome = if (parts[6] == NO_OUTCOME) {
                null
            } else {
                ExchangeOutcome.entries.firstOrNull { it.name == parts[6] }
            }

            return HealthSnapshot(
                // Negative counters would make retryAttemptIndex lie and could
                // shorten the backoff. Clamp rather than reject: a corrupted
                // number is not a reason to forget everything.
                consecutiveFailures = consecutive.coerceAtLeast(0),
                totalFailures = totalFailures.coerceAtLeast(0),
                totalSuccesses = totalSuccesses.coerceAtLeast(0),
                lastSuccessUtcEpochSeconds = lastSuccess.coerceAtLeast(0),
                lastOutcomeUtcEpochSeconds = lastOutcomeAt.coerceAtLeast(0),
                lastOutcome = outcome,
                lastStatusResultCode = resultCode,
            )
        }
    }
}

/**
 * [HealthLevel] from a failure count.
 *
 * [failingThreshold] is the number of consecutive failures at which the backoff
 * has reached its §5.2 cap — `Backoff.attemptsToReachCap + 1`, because the first
 * failure is attempt 0. Passing it in rather than importing [Backoff] keeps this
 * a pure function of two numbers, and keeps §5.2's arithmetic in exactly one
 * place.
 */
fun healthLevelFor(consecutiveFailures: Int, failingThreshold: Int): HealthLevel = when {
    consecutiveFailures <= 0 -> HealthLevel.Healthy
    consecutiveFailures >= failingThreshold -> HealthLevel.Failing
    else -> HealthLevel.Degraded
}

/**
 * [HealthLevel] from a whole [HealthSnapshot] — the overload that can tell
 * "nothing has happened yet" from "the last thing that happened went well".
 *
 * Two numbers cannot: [HealthSnapshot.consecutiveFailures] is zero on a fresh
 * install and zero after a successful exchange, so the overload above is obliged
 * to answer [HealthLevel.Healthy] for both. [HealthSnapshot.lastOutcome] is the
 * field that separates them, and it is null exactly until something is recorded.
 *
 * Prefer this one anywhere the answer is shown to a person. The two-number
 * version stays because [Backoff]'s threshold arithmetic is the whole of the rest
 * of the rule, and it is worth being able to test that rule without building a
 * snapshot around it.
 */
fun healthLevelFor(snapshot: HealthSnapshot, failingThreshold: Int): HealthLevel =
    if (snapshot.lastOutcome == null) {
        HealthLevel.NothingRecorded
    } else {
        healthLevelFor(snapshot.consecutiveFailures, failingThreshold)
    }

/**
 * The mutable holder around [HealthSnapshot]. All it does is apply the two
 * transitions Law 2 defines; the escalation itself is [Backoff]'s job, driven by
 * [HealthSnapshot.consecutiveFailures].
 */
class Health(initial: HealthSnapshot = HealthSnapshot()) {

    var snapshot: HealthSnapshot = initial
        private set

    /**
     * §4: a Status notify with `result == 0`. The one thing that clears the
     * counter — not a successful connect, not a clean disconnect, not a notify
     * with any other code.
     */
    fun recordSuccess(nowUtcEpochSeconds: Long, statusResultCode: Int): HealthSnapshot {
        snapshot = snapshot.copy(
            consecutiveFailures = 0,
            totalSuccesses = snapshot.totalSuccesses + 1,
            lastSuccessUtcEpochSeconds = nowUtcEpochSeconds,
            lastOutcomeUtcEpochSeconds = nowUtcEpochSeconds,
            lastOutcome = ExchangeOutcome.Succeeded,
            lastStatusResultCode = statusResultCode,
        )
        return snapshot
    }

    /**
     * One step further into the backoff. [statusResultCode] is the watch's
     * `result` byte when there was one, [HealthSnapshot.NO_RESULT_CODE] otherwise.
     */
    fun recordFailure(
        nowUtcEpochSeconds: Long,
        outcome: ExchangeOutcome,
        statusResultCode: Int = HealthSnapshot.NO_RESULT_CODE,
    ): HealthSnapshot {
        snapshot = snapshot.copy(
            // Saturating rather than wrapping. This process is expected to live
            // for months; an Int that wraps to negative here would collapse the
            // backoff to its base delay at the worst possible moment.
            consecutiveFailures = if (snapshot.consecutiveFailures == Int.MAX_VALUE) {
                Int.MAX_VALUE
            } else {
                snapshot.consecutiveFailures + 1
            },
            totalFailures = snapshot.totalFailures + 1,
            lastOutcomeUtcEpochSeconds = nowUtcEpochSeconds,
            lastOutcome = outcome,
            lastStatusResultCode = statusResultCode,
        )
        return snapshot
    }
}
