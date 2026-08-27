package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The resting notification's two facts, and the gauge drawn into the status bar.
 *
 * The notification is on screen for months and is re-posted only when something
 * changes, so everything it says has to be true at *any* later moment, not only at
 * the moment it was posted. That is the property this file is really about, and it
 * shows up twice: [RestingSummary] carries the **instant** of the last sync rather
 * than an elapsed time (so nothing here can go stale — `app/` hands the instant to
 * a chronometer), and [batteryGaugeFor] turns a measurement into whole steps rather
 * than into a precision the icon cannot draw.
 *
 * The health snapshots below are driven through real [Health] transitions rather
 * than hand-built, for the reason `ExchangeHistoryTest` gives: a hand-built
 * snapshot can claim anything about itself and proves nothing about the states the
 * app can actually be in.
 */
class RestingSummaryTest {

    private val startEpoch = 1_786_970_096L

    private fun statusReport(
        at: Long,
        outcome: ExchangeOutcome,
        resultCode: Int,
        battery: Int?,
    ) = ExchangeReport(
        atUtcEpochSeconds = at,
        outcome = outcome,
        statusResultCode = resultCode,
        batteryPercent = battery,
    )

    // ── The four shapes of the caption ───────────────────────────────────────

    @Test
    fun `a fresh install has neither fact`() {
        assertEquals(
            RestingSummary.NothingYet,
            restingSummaryFor(HealthSnapshot(), lastAttempt = null),
        )
    }

    @Test
    fun `an ordinary sync carries the battery and the instant it happened`() {
        val health = Health().apply { recordSuccess(startEpoch, statusResultCode = 0) }
        val summary = restingSummaryFor(
            health.snapshot,
            statusReport(startEpoch, ExchangeOutcome.Succeeded, resultCode = 0, battery = 63),
        )
        // The instant, not "0 minutes ago". This is the whole anti-staleness
        // design: nothing derived from the current time is stored, so the value
        // the notification was built from is still true an hour later.
        assertEquals(RestingSummary.BatteryAndSync(63, startEpoch), summary)
    }

    @Test
    fun `a watch that has never taken a battery sample still gets a sync line`() {
        // PROTOCOL.md §3.2's 0xFF, which the protocol decoder has already turned
        // into a null by the time it reaches an ExchangeReport.
        val health = Health().apply { recordSuccess(startEpoch, statusResultCode = 0) }
        val summary = restingSummaryFor(
            health.snapshot,
            statusReport(startEpoch, ExchangeOutcome.Succeeded, resultCode = 0, battery = null),
        )
        assertEquals(RestingSummary.SyncedOnly(startEpoch), summary)
        assertNull(summary.batteryPercent)
    }

    @Test
    fun `a watch that answers but refuses the time reports a battery and no sync`() {
        // §6.2's BadVersion: a well-formed Status frame, so it carries a battery
        // reading, and a failed exchange, so the clock was never set. The case
        // that stops "battery known" and "has synced" being the same question.
        val health = Health().apply {
            recordFailure(startEpoch, ExchangeOutcome.WatchReportedFailure, statusResultCode = 2)
        }
        val summary = restingSummaryFor(
            health.snapshot,
            statusReport(startEpoch, ExchangeOutcome.WatchReportedFailure, resultCode = 2, battery = 63),
        )
        assertEquals(RestingSummary.BatteryOnly(63), summary)
    }

    @Test
    fun `the battery is the one from the last exchange, even when that exchange failed`() {
        // A success an hour ago at 63 %, then a failed window that still got an
        // answer at 58 %. The newer reading is the one the watch measured most
        // recently, and it is the one shown — the sync instant stays at the older
        // one, because that is when a sync last happened.
        val health = Health().apply {
            recordSuccess(startEpoch, statusResultCode = 0)
            recordFailure(startEpoch + 3_600, ExchangeOutcome.WatchReportedFailure, statusResultCode = 6)
        }
        val summary = restingSummaryFor(
            health.snapshot,
            statusReport(startEpoch + 3_600, ExchangeOutcome.WatchReportedFailure, resultCode = 6, battery = 58),
        )
        assertEquals(RestingSummary.BatteryAndSync(58, startEpoch), summary)
    }

    @Test
    fun `a lost health file does not lose a surviving successful exchange`() {
        // The half-damaged store ExchangeHistory already resolves: counters gone,
        // the exchange record intact. The notification and the diagnostic screen
        // have to answer the same way, which is why both go through
        // lastSuccessfulExchangeAt.
        val attempt = statusReport(startEpoch, ExchangeOutcome.Succeeded, resultCode = 0, battery = 40)
        val summary = restingSummaryFor(HealthSnapshot(), attempt)
        assertEquals(RestingSummary.BatteryAndSync(40, startEpoch), summary)

        val history = exchangeHistoryFor(
            health = HealthSnapshot(),
            lastAttempt = attempt,
            nowUtcEpochSeconds = startEpoch + 60,
            failingThreshold = Backoff(JitterSource.NONE).attemptsToReachCap + 1,
        )
        assertIs<RestingSummary.BatteryAndSync>(summary)
        assertEquals(history.lastSuccessUtcEpochSeconds, summary.syncedAtUtcEpochSeconds)
    }

    @Test
    fun `a lost health file with only a failed exchange claims no sync`() {
        val summary = restingSummaryFor(
            HealthSnapshot(),
            statusReport(startEpoch, ExchangeOutcome.OperationTimedOut, HealthSnapshot.NO_RESULT_CODE, battery = null),
        )
        assertEquals(RestingSummary.NothingYet, summary)
    }

    // ── The gauge ────────────────────────────────────────────────────────────

    @Test
    fun `no reading is not the same thing as a flat watch`() {
        // The distinction the icon exists to keep: "we have not been told" draws a
        // dash, "the watch is flat" draws an empty case. Collapsing them would put
        // a dead-watch icon in the status bar of every install that has never
        // synced.
        assertEquals(BatteryGauge.NoReading, batteryGaugeFor(null))
        assertEquals(BatteryGauge.Filled(0), batteryGaugeFor(0))
        assertNotEquals<BatteryGauge>(batteryGaugeFor(null), batteryGaugeFor(0))
    }

    @Test
    fun `the ends are exact and half is exactly half`() {
        assertEquals(BatteryGauge.Filled(0), batteryGaugeFor(0))
        assertEquals(BatteryGauge.Filled(BatteryGauge.STEPS), batteryGaugeFor(100))
        // Half the *steps*, exactly, and an odd count could not express that. Note
        // what this file can and cannot see: that half the steps then becomes half
        // the *case* is a fact about `WatchGaugeIcon`'s geometry, which lives in
        // `app/` and is out of reach from here. `WatchGlyphTest` carries that end —
        // it asserts the fill's top edge against both the gauge's centre and the
        // case's. An even STEPS is necessary for the claim and not sufficient.
        assertEquals(BatteryGauge.Filled(BatteryGauge.STEPS / 2), batteryGaugeFor(50))
        assertEquals(0, BatteryGauge.STEPS % 2, "an odd STEPS cannot land on half a battery")
    }

    @Test
    fun `a watch with any charge left never draws empty`() {
        // 1 % through 4 % are all nearer to no steps than to one, and must still
        // draw something: an icon indistinguishable from a dead watch while the
        // watch is alive is the one error here that gets acted on.
        for (percent in 1..100) {
            val gauge = batteryGaugeFor(percent)
            assertIs<BatteryGauge.Filled>(gauge, "$percent %")
            assertTrue(gauge.steps >= 1, "$percent %")
        }
    }

    @Test
    fun `every reading lands on a step the icon can draw, and never goes backwards`() {
        var previous = 0
        for (percent in 0..100) {
            val gauge = batteryGaugeFor(percent)
            assertTrue(gauge is BatteryGauge.Filled, "$percent %")
            assertTrue(gauge.steps in 0..BatteryGauge.STEPS, "$percent % -> ${gauge.steps}")
            // Monotone: a watch that lost a point of charge can never draw fuller
            // than it did before. Nothing else in the app would notice if the
            // rounding rule broke this.
            assertTrue(gauge.steps >= previous, "$percent % went backwards")
            previous = gauge.steps
        }
        assertEquals(BatteryGauge.STEPS, previous)
    }

    @Test
    fun `the step boundaries are where the arithmetic says they are`() {
        // Nearest-step, so at ten steps every boundary is an exact `x.5` and each
        // of these pairs is one point either side of one. That is the whole point
        // of the case: truncation would answer the lower step on *both* halves of
        // every pair, so each pair fails the moment "round" quietly becomes
        // "truncate" in a later edit.
        assertEquals(BatteryGauge.Filled(1), batteryGaugeFor(5))
        assertEquals(BatteryGauge.Filled(1), batteryGaugeFor(14))
        assertEquals(BatteryGauge.Filled(2), batteryGaugeFor(15))
        assertEquals(BatteryGauge.Filled(2), batteryGaugeFor(24))
        assertEquals(BatteryGauge.Filled(3), batteryGaugeFor(25))
        assertEquals(BatteryGauge.Filled(9), batteryGaugeFor(94))
        // The top boundary, and the one honest cost of rounding to nearest: 95 %
        // and above draw a full case.
        assertEquals(BatteryGauge.Filled(10), batteryGaugeFor(95))

        // 4 % rounds to nothing and is lifted by the clamp, not by the rounding —
        // the one input where the two rules answer differently.
        assertEquals(BatteryGauge.Filled(1), batteryGaugeFor(4), "clamped up off zero")
    }

    @Test
    fun `a percentage from outside the protocol's range is clamped, not thrown`() {
        // Unreachable while both decoders hold §3.2's promise, which is the point:
        // if one ever stops, the notification draws a clamped gauge instead of
        // throwing inside the foreground service's notification build.
        assertEquals(BatteryGauge.Filled(0), batteryGaugeFor(-5))
        assertEquals(BatteryGauge.Filled(BatteryGauge.STEPS), batteryGaugeFor(250))
    }

    @Test
    fun `the summary's gauge is the gauge for the battery it carries`() {
        assertEquals(BatteryGauge.NoReading, RestingSummary.NothingYet.gauge)
        assertEquals(BatteryGauge.NoReading, RestingSummary.SyncedOnly(startEpoch).gauge)
        assertEquals(batteryGaugeFor(63), RestingSummary.BatteryOnly(63).gauge)
        assertEquals(batteryGaugeFor(63), RestingSummary.BatteryAndSync(63, startEpoch).gauge)
    }
}
