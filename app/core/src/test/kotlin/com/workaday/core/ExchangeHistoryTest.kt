package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The "what has been happening" block, and the relationship between its lines.
 *
 * This file exists because of a defect no per-line test could have caught. On a
 * fresh install the screen read:
 *
 *     Last sync: never
 *     The watch has never synced with this phone.
 *     Health: the last exchange succeeded      <- false, and false only in company
 *     Last attempt: none recorded yet
 *
 * Every one of those strings is correct in isolation, and every function that
 * produced one was covered. What was wrong is that two of them contradict each
 * other, and a property *between* lines needs a test that can see more than one
 * line — which is why [ExchangeHistory] is a value rather than four calls at the
 * call site, and why [ExchangeHistory.isSelfConsistent] is the assertion that
 * carries this file.
 *
 * The snapshots below are built by driving [Health] through real transitions
 * rather than by hand. A hand-built [HealthSnapshot] can claim anything about
 * itself — zero failures with a `Succeeded` outcome and no success timestamp, say
 * — and proves nothing about states the app can actually be in.
 */
class ExchangeHistoryTest {

    /** §5.2's ladder length, asked of the same [Backoff] the service runs. */
    private val failingThreshold = Backoff(JitterSource.NONE).attemptsToReachCap + 1

    private val startEpoch = 1_786_970_096L

    /** The report the service writes alongside every health record. */
    private fun report(at: Long, outcome: ExchangeOutcome) = ExchangeReport(
        atUtcEpochSeconds = at,
        outcome = outcome,
        statusResultCode = if (outcome == ExchangeOutcome.Succeeded) 0 else HealthSnapshot.NO_RESULT_CODE,
    )

    private fun history(
        health: HealthSnapshot,
        lastAttempt: ExchangeReport?,
        now: Long = startEpoch,
    ) = exchangeHistoryFor(health, lastAttempt, now, failingThreshold)

    // ── The fresh install, line by line ──────────────────────────────────────

    @Test
    fun `on a fresh install nothing in the block claims an exchange has happened`() {
        // The regression. Before the fix the third field came back Healthy, which
        // app/ renders as "the last exchange succeeded" — between two lines saying
        // nothing had ever happened.
        val fresh = history(HealthSnapshot(), lastAttempt = null)

        assertNull(fresh.lastSuccessUtcEpochSeconds, "Last sync: never")
        assertEquals(SyncRecency.NeverSynced, fresh.recency)
        assertEquals(HealthLevel.NothingRecorded, fresh.health, "a fresh install has succeeded at nothing")
        assertNull(fresh.lastAttempt, "Last attempt: none recorded yet")

        assertFalse(fresh.claimsAnExchangeSucceeded, "a first launch must not claim a sync it has never had")
        assertTrue(fresh.isSelfConsistent)
    }

    @Test
    fun `Healthy is never the answer for a snapshot that has recorded nothing`() {
        // The one-line version of the bug, stated separately: this is the assertion
        // that fails if NothingRecorded is ever folded back into Healthy.
        assertEquals(HealthLevel.NothingRecorded, healthLevelFor(HealthSnapshot(), failingThreshold))
        // ...while the two-number rule still answers what it always answered. It
        // cannot do better: zero failures is zero failures either way, which is
        // exactly why the snapshot overload had to exist.
        assertEquals(HealthLevel.Healthy, healthLevelFor(0, failingThreshold))
    }

    @Test
    fun `the machine reports NothingRecorded before it has ever completed an exchange`() {
        val machine = ConnectionStateMachine(FakeClock(), Backoff(JitterSource.NONE))
        assertEquals(HealthLevel.NothingRecorded, machine.healthLevel)
    }

    // ── Each reachable state ─────────────────────────────────────────────────

    @Test
    fun `one successful exchange makes every line agree that it happened`() {
        val health = Health().apply { recordSuccess(startEpoch, 0) }.snapshot
        val h = history(health, report(startEpoch, ExchangeOutcome.Succeeded))

        assertEquals(startEpoch, h.lastSuccessUtcEpochSeconds)
        assertEquals(SyncRecency.Recent, h.recency)
        assertEquals(HealthLevel.Healthy, h.health)
        assertTrue(h.claimsAnExchangeSucceeded)
        assertFalse(h.claimsNoExchangeHasSucceeded)
        assertTrue(h.isSelfConsistent)
    }

    @Test
    fun `a first exchange that failed says so without claiming a sync`() {
        // "Last sync: never" beside "1 failed attempt in a row" is not a
        // contradiction: nothing has succeeded, and something has been attempted.
        // The distinction is why claimsNoExchangeHasSucceeded is about success
        // rather than about activity.
        val health = Health().apply {
            recordFailure(startEpoch, ExchangeOutcome.OperationTimedOut)
        }.snapshot
        val h = history(health, report(startEpoch, ExchangeOutcome.OperationTimedOut))

        assertNull(h.lastSuccessUtcEpochSeconds)
        assertEquals(SyncRecency.NeverSynced, h.recency)
        assertEquals(HealthLevel.Degraded, h.health)
        assertFalse(h.claimsAnExchangeSucceeded)
        assertTrue(h.isSelfConsistent)
    }

    @Test
    fun `a success followed by failures still reports the sync that did happen`() {
        val counter = Health()
        counter.recordSuccess(startEpoch, 0)
        repeat(3) { counter.recordFailure(startEpoch + 60, ExchangeOutcome.DisconnectedMidExchange) }
        val h = history(
            counter.snapshot,
            report(startEpoch + 60, ExchangeOutcome.DisconnectedMidExchange),
            now = startEpoch + 60,
        )

        assertEquals(startEpoch, h.lastSuccessUtcEpochSeconds)
        assertEquals(HealthLevel.Degraded, h.health)
        assertTrue(h.claimsAnExchangeSucceeded, "the sync a minute ago really did happen")
        assertFalse(h.claimsNoExchangeHasSucceeded)
        assertTrue(h.isSelfConsistent)
    }

    @Test
    fun `a long silence after an old success is Stalled and still consistent`() {
        val counter = Health()
        counter.recordSuccess(startEpoch, 0)
        repeat(failingThreshold) { counter.recordFailure(startEpoch, ExchangeOutcome.ConnectionAttemptFailed) }
        val day = startEpoch + 24 * 3_600L
        val h = history(counter.snapshot, report(day, ExchangeOutcome.ConnectionAttemptFailed), now = day)

        assertEquals(SyncRecency.Stalled, h.recency)
        assertEquals(HealthLevel.Failing, h.health)
        assertTrue(h.isSelfConsistent)
    }

    // ── The invariant, over every reachable state ────────────────────────────

    @Test
    fun `no reachable sequence of exchanges can make two lines contradict each other`() {
        // The property, over every state a real run can reach. Each step feeds the
        // block the same pair of records the service would have written, at the
        // same instant, and asks whether the screen would contradict itself.
        //
        // Run this against the pre-fix code — NothingRecorded folded into Healthy —
        // and the very first check fails, before any exchange has happened at all.
        val outcomes = ExchangeOutcome.entries
        var checked = 0
        for (sequenceLength in 0..3) {
            for (seed in 0 until pow(outcomes.size, sequenceLength)) {
                val counter = Health()
                var lastAttempt: ExchangeReport? = null
                var at = startEpoch
                var code = seed

                // The empty sequence is the fresh install, checked before anything.
                assertConsistent(counter.snapshot, lastAttempt, at, "fresh")
                checked++

                repeat(sequenceLength) { step ->
                    val outcome = outcomes[code % outcomes.size]
                    code /= outcomes.size
                    at += 900L
                    if (outcome == ExchangeOutcome.Succeeded) {
                        counter.recordSuccess(at, 0)
                    } else {
                        counter.recordFailure(at, outcome)
                    }
                    lastAttempt = report(at, outcome)
                    // Checked at the instant it happened, an hour later and a day
                    // later, so every SyncRecency band is crossed by every history.
                    for (elapsed in listOf(0L, 3_600L, 24 * 3_600L)) {
                        assertConsistent(counter.snapshot, lastAttempt, at + elapsed, "seed=$seed step=$step +$elapsed")
                        checked++
                    }
                }
            }
        }
        assertTrue(checked > 3_000, "the walk collapsed to $checked checks; it is not exercising anything")
    }

    private fun assertConsistent(
        health: HealthSnapshot,
        lastAttempt: ExchangeReport?,
        now: Long,
        label: String,
    ) {
        val h = history(health, lastAttempt, now)
        assertTrue(
            h.isSelfConsistent,
            "$label: one line says an exchange succeeded while another says none has\n  $h",
        )
    }

    private fun pow(base: Int, exponent: Int): Int {
        var result = 1
        repeat(exponent) { result *= base }
        return result
    }

    // ── The two persisted records coming apart ───────────────────────────────

    @Test
    fun `a lost health record does not leave the block arguing with itself`() {
        // HealthSnapshot.decode answers a zeroed snapshot for a value it cannot
        // read, while ExchangeReport.decode answers null — so losing only the
        // health string would otherwise put "Last sync: never" directly above
        // "Last attempt: just now — the watch's clock was set".
        val h = history(HealthSnapshot(), report(startEpoch, ExchangeOutcome.Succeeded))

        assertEquals(startEpoch, h.lastSuccessUtcEpochSeconds, "the surviving record is the evidence")
        assertEquals(HealthLevel.Healthy, h.health)
        assertEquals(SyncRecency.Recent, h.recency)
        assertTrue(h.isSelfConsistent)
    }

    @Test
    fun `a lost health record with a failed attempt does not invent a success`() {
        val h = history(HealthSnapshot(), report(startEpoch, ExchangeOutcome.WatchReportedFailure))

        assertNull(h.lastSuccessUtcEpochSeconds)
        assertEquals(HealthLevel.Degraded, h.health, "no count survived, so nothing justifies Failing")
        assertFalse(h.claimsAnExchangeSucceeded)
        assertTrue(h.isSelfConsistent)
    }

    @Test
    fun `a lost exchange record leaves the counters in charge`() {
        // The reachable half of the split: PersistHealth is performed before
        // ReportExchange, so a process death between the two commits loses the
        // attempt and keeps the counters.
        val health = Health().apply { recordSuccess(startEpoch, 0) }.snapshot
        val h = history(health, lastAttempt = null)

        assertEquals(startEpoch, h.lastSuccessUtcEpochSeconds)
        assertEquals(HealthLevel.Healthy, h.health)
        assertNull(h.lastAttempt)
        assertTrue(h.isSelfConsistent)
    }

    // ── Clock jumps ──────────────────────────────────────────────────────────

    @Test
    fun `a wall clock that jumped backwards reads as a recent sync, not a stalled one`() {
        val health = Health().apply { recordSuccess(startEpoch, 0) }.snapshot
        val h = history(health, report(startEpoch, ExchangeOutcome.Succeeded), now = startEpoch - 86_400L)

        assertEquals(SyncRecency.Recent, h.recency)
        assertTrue(h.isSelfConsistent)
    }
}
