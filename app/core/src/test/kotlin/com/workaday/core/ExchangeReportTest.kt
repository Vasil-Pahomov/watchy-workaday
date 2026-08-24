package com.workaday.core

import com.workaday.core.protocol.SyncResult
import com.workaday.core.protocol.WatchStatus
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

/**
 * The record that turns `Action.ReportExchange` from a log line into something the
 * diagnostic screen can read after the process that produced it has died.
 *
 * The storage format is the whole contract with `app/`, which puts one string
 * somewhere durable and hands it back — so the tests that matter are the ones about
 * what happens when that string is missing, truncated, or written by a build that
 * is not this one. [ExchangeReport.decode] must never throw: it is read while the
 * user is opening the one window whose job is to explain what went wrong.
 */
class ExchangeReportTest {

    private val status = WatchStatus(
        resultCode = SyncResult.Ok.code,
        result = SyncResult.Ok,
        batteryPercent = 78,
        appliedUtcEpochSeconds = 1_786_970_096L,
        fwBuild = 1,
    )

    // ── from(Action.ReportExchange) ──────────────────────────────────────────

    @Test
    fun `a successful exchange carries the watch's whole answer`() {
        // Breaks if the mapping drops a field. Battery and firmware build are the
        // only things in this app that ever come *from* the watch, and PROTOCOL.md
        // section 3.2 exists partly so the diagnostic screen can show them.
        val report = ExchangeReport.from(
            Action.ReportExchange(ExchangeOutcome.Succeeded, status, atUtcEpochSeconds = 1_700L),
        )
        assertEquals(1_700L, report.atUtcEpochSeconds)
        assertEquals(ExchangeOutcome.Succeeded, report.outcome)
        assertEquals(SyncResult.Ok.code, report.statusResultCode)
        assertEquals(78, report.batteryPercent)
        assertEquals(1, report.fwBuild)
        assertEquals(1_786_970_096L, report.appliedUtcEpochSeconds)
    }

    @Test
    fun `an exchange the watch never answered records no result code, not a zero one`() {
        // The trap: `result` 0 is Ok. A report that defaulted a missing answer to 0
        // would show "the watch accepted the time" for a connection that timed out
        // before the watch said anything at all.
        val report = ExchangeReport.from(
            Action.ReportExchange(ExchangeOutcome.OperationTimedOut, status = null, atUtcEpochSeconds = 5L),
        )
        assertEquals(HealthSnapshot.NO_RESULT_CODE, report.statusResultCode)
        assertNull(report.batteryPercent)
        assertNull(report.fwBuild)
        assertEquals(0L, report.appliedUtcEpochSeconds)
    }

    @Test
    fun `a watch that reported a failure still has its code and battery kept`() {
        // PROTOCOL.md section 6.2: the result code is the thing that must be
        // visible, and it is most valuable exactly when the exchange failed.
        val failed = WatchStatus(
            resultCode = SyncResult.BadVersion.code,
            result = SyncResult.BadVersion,
            batteryPercent = null,
            appliedUtcEpochSeconds = 0,
            fwBuild = 9,
        )
        val report = ExchangeReport.from(
            Action.ReportExchange(ExchangeOutcome.WatchReportedFailure, failed, atUtcEpochSeconds = 42L),
        )
        assertEquals(SyncResult.BadVersion.code, report.statusResultCode)
        assertNull(report.batteryPercent, "0xFF means no sample, and must not become a number")
        assertEquals(9, report.fwBuild)
    }

    // ── encode / decode ──────────────────────────────────────────────────────

    @Test
    fun `a full report survives a round trip`() {
        val report = ExchangeReport(
            atUtcEpochSeconds = 1_786_970_096L,
            outcome = ExchangeOutcome.Succeeded,
            statusResultCode = 0,
            batteryPercent = 78,
            fwBuild = 1,
            appliedUtcEpochSeconds = 1_786_970_096L,
        )
        assertEquals(report, ExchangeReport.decode(report.encode()))
    }

    @Test
    fun `a report with no watch answer survives a round trip`() {
        // The absent battery and firmware build have to come back absent, not as
        // whatever an empty field parses to.
        val report = ExchangeReport(
            atUtcEpochSeconds = 5L,
            outcome = ExchangeOutcome.DisconnectedMidExchange,
        )
        assertEquals(report, ExchangeReport.decode(report.encode()))
    }

    @Test
    fun `every outcome round trips`() {
        // The outcome name is what goes on disk, so a rename is a format change.
        for (outcome in ExchangeOutcome.entries) {
            val report = ExchangeReport(atUtcEpochSeconds = 1L, outcome = outcome)
            assertEquals(report, ExchangeReport.decode(report.encode()), "$outcome")
        }
    }

    @Test
    fun `nothing stored decodes to nothing recorded`() {
        assertNull(ExchangeReport.decode(null))
        assertNull(ExchangeReport.decode(""))
    }

    @Test
    fun `a truncated or over-long record is discarded rather than half-read`() {
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded).encode()
        assertNull(ExchangeReport.decode(encoded.substringBeforeLast("|")))
        assertNull(ExchangeReport.decode("$encoded|extra"))
    }

    @Test
    fun `a record from another format version is discarded`() {
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded).encode()
        assertNull(ExchangeReport.decode(encoded.replaceFirst("1|", "2|")))
        assertNull(ExchangeReport.decode(encoded.replaceFirst("1|", "x|")))
    }

    @Test
    fun `an outcome this build has never heard of discards the record`() {
        // The one field that is not optional: without knowing how the exchange
        // ended there is nothing worth showing, and inventing a placeholder would
        // put a made-up outcome on a diagnostic screen.
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded).encode()
        assertNull(ExchangeReport.decode(encoded.replace("Succeeded", "TeleportedAway")))
    }

    @Test
    fun `garbage in a numeric field discards the record rather than throwing`() {
        // decode() runs while the user is opening the screen that explains what
        // went wrong. Throwing there is the one outcome with no recovery.
        val parts = ExchangeReport(1L, ExchangeOutcome.Succeeded, 0, 50, 7, 9L).encode().split("|")
        for (i in parts.indices) {
            val corrupted = parts.toMutableList().also { it[i] = "!!" }.joinToString("|")
            // Some fields tolerate garbage by dropping just that field; none may
            // throw, and none may produce a report claiming something untrue.
            val decoded = ExchangeReport.decode(corrupted)
            if (decoded != null) {
                assertEquals(ExchangeOutcome.Succeeded, decoded.outcome, "field $i")
            }
        }
        assertNull(ExchangeReport.decode("!!"))
        assertNull(ExchangeReport.decode("||||||"))
    }

    @Test
    fun `an optional field that cannot be parsed comes back absent, not zero`() {
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded, 0, 50, 7, 9L).encode()
        val decoded = ExchangeReport.decode(encoded.replaceFirst("|50|", "|!!|"))
        assertNull(decoded?.batteryPercent, "an unreadable battery reading must not become 0 %")
        assertEquals(7, decoded?.fwBuild)
    }

    @Test
    fun `a battery percentage outside the protocol's domain is dropped on the way back in`() {
        // PROTOCOL.md section 3.2 promises 0..100 or "unknown" and the decoder held
        // the watch to that once already. Held again across a restart, because the
        // value on disk was written by a build that may not have.
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded, 0, 50, 7, 9L).encode()
        assertNull(ExchangeReport.decode(encoded.replaceFirst("|50|", "|200|"))?.batteryPercent)
        assertNull(ExchangeReport.decode(encoded.replaceFirst("|50|", "|-5|"))?.batteryPercent)
        assertEquals(0, ExchangeReport.decode(encoded.replaceFirst("|50|", "|0|"))?.batteryPercent)
        assertEquals(100, ExchangeReport.decode(encoded.replaceFirst("|50|", "|100|"))?.batteryPercent)
    }

    @Test
    fun `negative timestamps are clamped rather than shown`() {
        val encoded = ExchangeReport(1L, ExchangeOutcome.Succeeded, 0, 50, 7, 9L).encode()
        val decoded = ExchangeReport.decode(encoded.replaceFirst("|1|", "|-9|"))
        assertEquals(0L, decoded?.atUtcEpochSeconds)
    }

    @Test
    fun `an unknown result code is kept as a number for the screen to show`() {
        // A newer firmware may answer with a code this build does not know. Keeping
        // the byte is the difference between "result 9" and "something happened".
        val report = ExchangeReport(1L, ExchangeOutcome.WatchReportedFailure, statusResultCode = 99)
        assertEquals(99, ExchangeReport.decode(report.encode())?.statusResultCode)
        assertNull(SyncResult.fromCode(99), "99 must stay unknown for this test to mean anything")
    }
}
