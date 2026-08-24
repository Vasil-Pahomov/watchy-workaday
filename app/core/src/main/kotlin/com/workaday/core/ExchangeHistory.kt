package com.workaday.core

/**
 * The diagnostic screen's "what has been happening" block, decided.
 *
 * **Why this exists as a type rather than four calls at the call site.** The
 * defect that produced it — "Health: the last exchange succeeded" sitting between
 * "Last sync: never" and "Last attempt: none recorded yet" on a first launch —
 * was not a wrong line. Every line was defensible read alone. What was wrong was
 * the *relationship* between them, and a relationship between four separately
 * computed values has nowhere to be asserted. Gathering them into one value gives
 * it somewhere: [isSelfConsistent], checked in `ExchangeHistoryTest` over every
 * state a real sequence of exchanges can reach.
 *
 * That is the same shape as an earlier escape in this app — a pairing message that
 * contradicted the task list above it — and the same repair: the property that
 * failed lives between lines, so the test has to be able to see more than one.
 *
 * No strings and no formatting here. `app/` renders each field, exactly as it
 * renders [ServiceNotice]; this decides what there is to say.
 */
data class ExchangeHistory(

    /** Wall clock of the last successful exchange, or null if there has never been one. */
    val lastSuccessUtcEpochSeconds: Long?,

    /** [lastSuccessUtcEpochSeconds] read against the watch's window cadence. */
    val recency: SyncRecency,

    /** How worried to be. [HealthLevel.NothingRecorded] when nothing has happened. */
    val health: HealthLevel,

    /** The last exchange of any kind, or null if none has been recorded. */
    val lastAttempt: ExchangeReport?,
) {

    /** Some line of this block tells the user an exchange has succeeded. */
    val claimsAnExchangeSucceeded: Boolean
        get() = health == HealthLevel.Healthy ||
            lastAttempt?.outcome == ExchangeOutcome.Succeeded ||
            lastSuccessUtcEpochSeconds != null

    /** Some line of this block tells the user none ever has. */
    val claimsNoExchangeHasSucceeded: Boolean
        get() = health == HealthLevel.NothingRecorded ||
            recency == SyncRecency.NeverSynced ||
            lastSuccessUtcEpochSeconds == null

    /**
     * The invariant the screen must never break: **no line may claim an exchange
     * succeeded while another says none has.**
     *
     * Asserted over reachable states — snapshots produced by real [Health]
     * transitions and the [ExchangeReport] written alongside them — and not over
     * arbitrary hand-built values, because a snapshot that was never produced by
     * `recordSuccess` or `recordFailure` can assert anything it likes about
     * itself and proves nothing about the app.
     */
    val isSelfConsistent: Boolean
        get() = !(claimsAnExchangeSucceeded && claimsNoExchangeHasSucceeded)
}

/**
 * Assemble the block from the two records the app persists.
 *
 * They are written one after the other by the same pair of `Action`s, so they
 * normally agree. This resolves the one way they can come apart **once, here**,
 * rather than letting two lines argue on screen: [HealthSnapshot.decode] answers a
 * *zeroed* snapshot for a value it cannot read, while [ExchangeReport.decode]
 * answers null — so a health string that is lost or corrupted while the exchange
 * string survives would otherwise show "Last sync: never" directly above "Last
 * attempt: two minutes ago — the watch's clock was set".
 *
 * Where the counters are empty and an attempt survives, the attempt is the record
 * of record: it is the more specific evidence, and it is the half that was not
 * lost.
 *
 * @param failingThreshold `Backoff.attemptsToReachCap + 1`. See [healthLevelFor].
 */
fun exchangeHistoryFor(
    health: HealthSnapshot,
    lastAttempt: ExchangeReport?,
    nowUtcEpochSeconds: Long,
    failingThreshold: Int,
): ExchangeHistory {
    val countersAreEmpty = health.lastOutcome == null
    val attemptSucceeded = lastAttempt?.outcome == ExchangeOutcome.Succeeded

    val lastSuccess: Long? = when {
        health.lastSuccessUtcEpochSeconds != 0L -> health.lastSuccessUtcEpochSeconds
        countersAreEmpty && attemptSucceeded -> lastAttempt?.atUtcEpochSeconds
        else -> null
    }

    val level = when {
        !countersAreEmpty -> healthLevelFor(health, failingThreshold)
        lastAttempt == null -> HealthLevel.NothingRecorded
        // Counters lost, an attempt survives. Degraded rather than Failing for a
        // failure: without a count there is no evidence the backoff is at its cap,
        // and overstating it is how a screen turns a lost file into an alarm.
        attemptSucceeded -> HealthLevel.Healthy
        else -> HealthLevel.Degraded
    }

    // Clamped for the same reason HealthSnapshot.secondsSinceLastSuccess clamps:
    // the wall clock jumps in both directions, and "synced in 4000 seconds' time"
    // is worse than "synced just now".
    val secondsSince = lastSuccess?.let { (nowUtcEpochSeconds - it).coerceAtLeast(0L) }

    return ExchangeHistory(
        lastSuccessUtcEpochSeconds = lastSuccess,
        recency = syncRecencyFor(secondsSince),
        health = level,
        lastAttempt = lastAttempt,
    )
}
