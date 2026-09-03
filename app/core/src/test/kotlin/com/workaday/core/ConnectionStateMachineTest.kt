package com.workaday.core

import com.workaday.core.protocol.SyncResult
import com.workaday.core.protocol.hex
import kotlin.random.Random
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * The connection state machine, driven entirely by a fake clock and a fake
 * transport. No Robolectric, no emulator, no device, no real delay.
 *
 * Two kinds of test live here. The first kind walks a named scenario — usually
 * one row of PROTOCOL.md §6.2 — and pins the exact actions it produces. The
 * second kind ([noThreeEventSequenceStrandsTheApp] and its random sibling)
 * makes no claim about any particular scenario and instead asserts Law 2's
 * invariant over every sequence it can reach, because "however rare" is not a
 * property a hand-written scenario can establish.
 */
class ConnectionStateMachineTest {

    // PROTOCOL.md §7.2, transcribed: result Ok, battery 78 %, fw_build 1.
    private val okStatus = hex("01 81 00 4E F0 FF 82 6A 01 00 00 00")

    // The same frame with `result` = 6 (Busy) and 2 (BadVersion), nothing applied.
    private val busyStatus = hex("01 81 06 4E 00 00 00 00 01 00 00 00")
    private val badVersionStatus = hex("01 81 02 4E 00 00 00 00 01 00 00 00")

    // PROTOCOL.md §7.4: §7.2 with flags.FIND_PHONE set — the watch is looking for
    // this phone. And the Busy answer with the same flag, for the failure path.
    private val findStatus = hex("01 81 00 4E F0 FF 82 6A 01 00 01 00")
    private val findBusyStatus = hex("01 81 06 4E 00 00 00 00 01 00 01 00")

    // PROTOCOL.md §7.1: the app must put exactly these bytes on the wire when the
    // phone's clock reads 2026-08-17T12:34:56Z at UTC+180.
    private val goldenTimePayload = hex("01 01 F0 FF 82 6A B4 00 00 00 00 00")

    private fun harness(
        clock: FakeClock = FakeClock(),
        jitter: JitterSource = JitterSource.NONE,
        health: HealthSnapshot = HealthSnapshot(),
        coverage: StateCoverage? = null,
    ) = FakeTransport(ConnectionStateMachine(clock, Backoff(jitter), health), coverage)

    /** Start, connect, discover, enable notifications, write Time. */
    private fun FakeTransport.runToAwaitingStatus() {
        send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
            TransportEvent.CharacteristicWritten(GATT_SUCCESS),
        )
        assertEquals(ConnectionState.AwaitingStatus, machine.state)
    }

    // ── §4, the exchange ─────────────────────────────────────────────────────

    @Test
    fun `the section 4 exchange happens in the required order`() {
        val t = harness()

        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.Started).summary())
        assertEquals(ConnectionState.Armed, t.machine.state)

        assertEquals(
            listOf("ArmOperationTimeout(5000)", "DiscoverServices"),
            t.send(TransportEvent.Connected(GATT_SUCCESS)).summary(),
        )
        assertEquals(
            listOf("ArmOperationTimeout(5000)", "EnableStatusNotifications"),
            t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS)).summary(),
        )
        // The Time write comes only now — after notifications are enabled.
        // Writing first races the notification and loses it (§4).
        assertEquals(
            listOf("ArmOperationTimeout(5000)", "WriteTime(12)"),
            t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS)).summary(),
        )
        // Waiting for the notify is not one of §5.2's per-operation timeouts, so
        // it gets what is left of the 15 s whole-exchange budget.
        assertEquals(
            listOf("ArmOperationTimeout(15000)"),
            t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS)).summary(),
        )
        assertEquals(
            listOf(
                "CancelTimers",
                "CloseConnection",
                "PersistHealth",
                "ReportExchange(Succeeded)",
                // NOT ArmAutoConnect. Re-arming here reconnects inside the window
                // that has just been used - 16 ms later, on hardware, every sync.
                "ArmOperationTimeout(15000)",
            ),
            t.send(TransportEvent.NotificationReceived(okStatus)).summary(),
        )
        assertEquals(ConnectionState.Settling(15_000), t.machine.state)

        // And only when that expires does the app go back to waiting for the next
        // window, which is an hour away.
        assertEquals(listOf("ArmAutoConnect"), t.fireOperationTimeout().summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `the Time write carries the section 7_1 golden bytes`() {
        // End to end: the fake clock is set to the document's instant and offset,
        // and what reaches the transport is the document's byte string.
        val t = harness(FakeClock(epochSeconds = 1_786_970_096L, offsetMinutes = 180))
        t.runToAwaitingStatus()
        assertContentEquals(goldenTimePayload, t.lastTimePayload)
    }

    @Test
    fun `notifications are enabled before the Time write, always`() {
        val t = harness()
        t.runToAwaitingStatus()
        val order = t.emitted.mapNotNull {
            when (it) {
                Action.EnableStatusNotifications -> "notify"
                is Action.WriteTime -> "write"
                else -> null
            }
        }
        assertEquals(listOf("notify", "write"), order)
    }

    @Test
    fun `exactly one GATT operation is outstanding at a time, each with a timeout`() {
        val t = harness()
        val opActions = setOf<Action>(Action.DiscoverServices, Action.EnableStatusNotifications)
        t.runToAwaitingStatus()

        // Every operation was immediately preceded by arming a timeout, and no
        // two operations were ever issued from the same event.
        var sawTimeoutSinceLastOp = false
        var opsInOneBatch = 0
        for (action in t.emitted) {
            when {
                action is Action.ArmOperationTimeout -> {
                    sawTimeoutSinceLastOp = true
                    opsInOneBatch = 0
                }
                action in opActions || action is Action.WriteTime -> {
                    assertTrue(sawTimeoutSinceLastOp, "operation $action issued with no timeout armed")
                    opsInOneBatch++
                    assertEquals(1, opsInOneBatch, "two operations issued back to back")
                    sawTimeoutSinceLastOp = false
                }
            }
        }
    }

    @Test
    fun `the per-operation timeout is clamped to what is left of the 15 s exchange budget`() {
        val clock = FakeClock()
        val t = harness(clock)
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
        assertEquals(5_000L, t.pendingTimerDelayMillis)

        clock.advance(13_000)
        t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
        // 15 000 - 13 000 = 2 000 left, which is less than the 5 s per-operation
        // allowance. The exchange cannot outlive its budget even though only one
        // timer is ever armed.
        assertEquals(2_000L, t.pendingTimerDelayMillis)

        clock.advance(5_000)
        t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))
        assertEquals(0L, t.pendingTimerDelayMillis, "an expired budget arms a timer that fires at once")
    }

    // ── §6.2, row by row ─────────────────────────────────────────────────────

    @Test
    fun `row 1 - a successful exchange closes, re-arms, and does not back off`() {
        val t = harness(health = HealthSnapshot(consecutiveFailures = 4))
        t.runToAwaitingStatus()
        val actions = t.send(TransportEvent.NotificationReceived(okStatus))

        assertTrue(Action.CloseConnection in actions, "close() is required, not disconnect() alone")
        assertTrue(actions.none { it == Action.ArmAutoConnect }, "re-arming here re-enters the same window")
        assertTrue(actions.none { it is Action.ScheduleRetry }, "a success must not schedule a backoff")
        assertIs<ConnectionState.Settling>(t.machine.state)
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertEquals(HealthLevel.Healthy, t.machine.healthLevel)
        assertEquals(ExchangeOutcome.Succeeded, t.reports.single().outcome)
        assertEquals(78, t.machine.lastStatus?.batteryPercent)
        assertEquals(1_786_970_096L, t.machine.lastStatus?.appliedUtcEpochSeconds)
    }

    @Test
    fun `row 2 - a disconnect mid-exchange closes, backs off one step, re-arms`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))

        // GATT_CONN_TERMINATE_PEER_USER; the code is irrelevant, the timing is not.
        val actions = t.send(TransportEvent.Disconnected(19))
        assertEquals(
            listOf(
                "CancelTimers",
                "CloseConnection",
                "PersistHealth",
                "ReportExchange(DisconnectedMidExchange)",
                "ScheduleRetry(30000)",
            ),
            actions.summary(),
        )
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
        assertEquals(1, t.machine.health.consecutiveFailures)

        assertEquals(listOf("ArmAutoConnect"), t.fireRetryTimer().summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `row 3 - status 133 while armed closes the stale client, backs off, re-arms`() {
        // §6.2 calls 133 "commonly a stale BluetoothDevice or a leaked client",
        // which is exactly why the remedy is close-and-recreate rather than retry
        // on the same client.
        for (event in listOf(TransportEvent.Disconnected(133), TransportEvent.Connected(133))) {
            val t = harness()
            t.send(TransportEvent.Started)
            val actions = t.send(event)
            assertEquals(
                listOf("CloseConnection", "PersistHealth", "ReportExchange(ConnectionAttemptFailed)", "ScheduleRetry(30000)"),
                actions.summary(),
                "$event",
            )
            assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
            t.fireRetryTimer()
            assertEquals(ConnectionState.Armed, t.machine.state)
        }
    }

    @Test
    fun `row 3b - a non-zero status on any GATT operation is retryable, not fatal`() {
        val steps = listOf<Pair<String, (FakeTransport) -> Unit>>(
            "discovery" to { t -> t.send(TransportEvent.ServicesDiscovered(133)) },
            "CCCD write" to { t ->
                t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
                t.send(TransportEvent.DescriptorWritten(133))
            },
            "Time write" to { t ->
                t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
                t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))
                t.send(TransportEvent.CharacteristicWritten(133))
            },
        )
        for ((name, step) in steps) {
            val t = harness()
            t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
            step(t)
            assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state, name)
            assertEquals(ExchangeOutcome.GattOperationFailed, t.reports.single().outcome, name)
            assertEquals(1, t.machine.health.consecutiveFailures, name)
            t.fireRetryTimer()
            assertEquals(ConnectionState.Armed, t.machine.state, name)
        }
    }

    @Test
    fun `row 3c - an operation the local stack never issued fails immediately, not in five seconds`() {
        // discoverServices() returning false, a characteristic missing from the
        // discovered profile, writeCharacteristic() refusing the request: no
        // callback will ever arrive, so the Android layer reports the failure with
        // GATT_LOCAL_FAILURE instead of letting the operation burn its §5.2
        // timeout first. It has to be distinguishable from success and from every
        // real status, and it has to take the ordinary §6.2 remedy.
        assertTrue(GATT_LOCAL_FAILURE != GATT_SUCCESS)
        assertTrue(GATT_LOCAL_FAILURE < 0, "a real GATT status or BluetoothStatusCodes value is never negative")

        val steps = listOf<Pair<String, (FakeTransport) -> Unit>>(
            "connectGatt returned null" to { t -> t.send(TransportEvent.Connected(GATT_LOCAL_FAILURE)) },
            "discoverServices returned false" to { t ->
                t.send(TransportEvent.Connected(GATT_SUCCESS))
                t.send(TransportEvent.ServicesDiscovered(GATT_LOCAL_FAILURE))
            },
            "the Status CCCD was missing" to { t ->
                t.send(TransportEvent.Connected(GATT_SUCCESS))
                t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
                t.send(TransportEvent.DescriptorWritten(GATT_LOCAL_FAILURE))
            },
            "writeCharacteristic was refused" to { t ->
                t.send(TransportEvent.Connected(GATT_SUCCESS))
                t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
                t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))
                t.send(TransportEvent.CharacteristicWritten(GATT_LOCAL_FAILURE))
            },
        )
        for ((name, step) in steps) {
            val t = harness()
            t.send(TransportEvent.Started)
            step(t)
            assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state, name)
            assertEquals(1, t.machine.health.consecutiveFailures, name)
            t.fireRetryTimer()
            assertEquals(ConnectionState.Armed, t.machine.state, name)
        }
    }

    @Test
    fun `row 4 - an operation timeout closes rather than merely disconnecting`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
        assertEquals(ConnectionState.Discovering, t.machine.state)

        val actions = t.fireOperationTimeout()
        assertEquals(
            listOf("CloseConnection", "PersistHealth", "ReportExchange(OperationTimedOut)", "ScheduleRetry(30000)"),
            actions.summary(),
        )
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
        t.fireRetryTimer()
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `row 4b - the whole-exchange backstop fires while waiting for the notify`() {
        val t = harness()
        t.runToAwaitingStatus()
        t.fireOperationTimeout()
        assertEquals(ExchangeOutcome.OperationTimedOut, t.reports.single().outcome)
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
    }

    @Test
    fun `row 5 - a notify with a non-zero result is a failed exchange`() {
        val t = harness()
        t.runToAwaitingStatus()
        val actions = t.send(TransportEvent.NotificationReceived(busyStatus))

        assertEquals(
            listOf(
                "CancelTimers",
                "CloseConnection",
                "PersistHealth",
                "ReportExchange(WatchReportedFailure)",
                "ScheduleRetry(30000)",
            ),
            actions.summary(),
        )
        assertEquals(1, t.machine.health.consecutiveFailures, "backoff must not be reset by a non-zero result")
        assertEquals(SyncResult.Busy, t.machine.lastStatus?.result)
        assertEquals(6, t.machine.health.lastStatusResultCode)
        // §6.2 requires the code to be surfaced, not just counted.
        assertEquals(6, t.reports.single().status?.resultCode)
    }

    @Test
    fun `row 5b - BadVersion is surfaced and backed off, never hot-looped`() {
        val t = harness()
        t.runToAwaitingStatus()
        t.send(TransportEvent.NotificationReceived(badVersionStatus))

        assertEquals(SyncResult.BadVersion, t.reports.single().status?.result)
        assertEquals(2, t.machine.health.lastStatusResultCode)
        val retry = assertIs<ConnectionState.WaitingForRetry>(t.machine.state)
        assertEquals(30_000L, retry.delayMillis, "a drifted peer must still wait a full backoff step")
    }

    @Test
    fun `row 5c - an unknown result code from a newer watch is never read as success`() {
        val t = harness()
        t.runToAwaitingStatus()
        val fromTheFuture = okStatus.copyOf().also { it[2] = 0x2A }
        t.send(TransportEvent.NotificationReceived(fromTheFuture))

        assertEquals(ExchangeOutcome.WatchReportedFailure, t.reports.single().outcome)
        assertNull(t.machine.lastStatus?.result)
        assertEquals(0x2A, t.machine.health.lastStatusResultCode)
        assertEquals(1, t.machine.health.consecutiveFailures)
    }

    @Test
    fun `row 6 - the adapter going off is a transition, not a fault`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))

        val actions = t.send(TransportEvent.AdapterOff)
        assertEquals(listOf("CancelTimers", "CloseConnection"), actions.summary())
        assertEquals(ConnectionState.Blocked(adapterOff = true, permissionMissing = false), t.machine.state)
        // No backoff step, no health increment: the watch did nothing wrong, and
        // penalising it would slow the first attempt after airplane mode ends.
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertTrue(t.reports.isEmpty())

        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.AdapterOn).summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `row 6b - permission revoked at runtime, then granted again`() {
        val t = harness()
        t.send(TransportEvent.Started)
        t.send(TransportEvent.PermissionRevoked)
        assertEquals(ConnectionState.Blocked(adapterOff = false, permissionMissing = true), t.machine.state)
        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.PermissionGranted).summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `row 6c - one condition clearing while the other holds does not arm`() {
        // Arming into a guaranteed failure would burn a GATT client slot every
        // time the user toggled the adapter with the permission still revoked.
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.AdapterOff, TransportEvent.PermissionRevoked)
        assertEquals(ConnectionState.Blocked(adapterOff = true, permissionMissing = true), t.machine.state)

        assertTrue(t.send(TransportEvent.AdapterOn).isEmpty())
        assertEquals(ConnectionState.Blocked(adapterOff = false, permissionMissing = true), t.machine.state)

        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.PermissionGranted).summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `row 6d - the radio going away during a backoff resumes immediately when it returns`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)

        assertEquals(listOf("CancelTimers"), t.send(TransportEvent.AdapterOff).summary())
        // Straight back to armed: the outage already supplied the delay, and the
        // watch's next window is the thing we are trying not to miss.
        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.AdapterOn).summary())
        // The failure count is untouched, so the next failure resumes the ladder.
        assertEquals(1, t.machine.health.consecutiveFailures)
        t.send(TransportEvent.Disconnected(133))
        assertEquals(ConnectionState.WaitingForRetry(60_000), t.machine.state)
    }

    @Test
    fun `row 6e - the adapter can be off before the service ever starts`() {
        val t = harness()
        t.send(TransportEvent.AdapterOff)
        assertEquals(ConnectionState.Idle, t.machine.state)
        assertTrue(t.send(TransportEvent.Started).isEmpty(), "must not call connectGatt with the adapter off")
        assertEquals(ConnectionState.Blocked(adapterOff = true, permissionMissing = false), t.machine.state)
        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.AdapterOn).summary())
    }

    // ── Backoff and health, §5.2's reset rule ────────────────────────────────

    @Test
    fun `a successful connect does not reset the backoff - only the notify does`() {
        // Reset-on-connect turns a peer that connects and immediately fails into
        // a hot loop, which is why §5.2 spells out that it is the notification.
        val t = harness(health = HealthSnapshot(consecutiveFailures = 3))
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
        assertEquals(3, t.machine.health.consecutiveFailures)

        t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
        t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))
        t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS))
        assertEquals(3, t.machine.health.consecutiveFailures, "getting all the way to the notify is still not a success")

        t.send(TransportEvent.NotificationReceived(okStatus))
        assertEquals(0, t.machine.health.consecutiveFailures)
    }

    @Test
    fun `a notify with result 0 is the only thing that resets the counter`() {
        val cases = listOf<Pair<String, ByteArray>>(
            "Busy" to busyStatus,
            "BadVersion" to badVersionStatus,
            "malformed" to hex("de ad be ef"),
        )
        for ((name, payload) in cases) {
            val t = harness(health = HealthSnapshot(consecutiveFailures = 2))
            t.runToAwaitingStatus()
            t.send(TransportEvent.NotificationReceived(payload))
            assertEquals(3, t.machine.health.consecutiveFailures, name)
        }
    }

    @Test
    fun `repeated failures escalate up the ladder and stop at the cap`() {
        val t = harness()
        t.send(TransportEvent.Started)
        val expected = listOf(30_000L, 60_000L, 120_000L, 240_000L, 480_000L, 900_000L, 900_000L, 900_000L)
        for ((index, delay) in expected.withIndex()) {
            t.send(TransportEvent.Disconnected(133))
            assertEquals(ConnectionState.WaitingForRetry(delay), t.machine.state, "failure ${index + 1}")
            t.fireRetryTimer()
        }
        assertEquals(HealthLevel.Failing, t.machine.healthLevel)
        assertEquals(8, t.machine.health.consecutiveFailures)

        // Still armed, still trying, still bounded — never a terminal state.
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `health is handed to the Android layer for persistence on every change`() {
        val t = harness()
        t.runToAwaitingStatus()
        t.send(TransportEvent.NotificationReceived(okStatus))
        val persisted = t.persisted.last()
        assertEquals(0, persisted.consecutiveFailures)
        assertEquals(1L, persisted.totalSuccesses)
        // Round-trippable with no help from APP-3.
        assertEquals(persisted, HealthSnapshot.decode(persisted.encode()))
    }

    @Test
    fun `health restored from disk drives the very first backoff`() {
        val restored = HealthSnapshot.decode(
            HealthSnapshot(consecutiveFailures = 4, lastOutcome = ExchangeOutcome.OperationTimedOut).encode(),
        )
        val t = harness(health = restored)
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        // Fifth consecutive failure -> attempt index 4 -> 480 s, not 30 s.
        assertEquals(ConnectionState.WaitingForRetry(480_000), t.machine.state)
    }

    @Test
    fun `jitter reaches the scheduled delay`() {
        val t = harness(jitter = { -1.0 })
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        assertEquals(ConnectionState.WaitingForRetry(24_000), t.machine.state)
    }

    // ── Boundaries ───────────────────────────────────────────────────────────

    @Test
    fun `a notification arriving after close is ignored`() {
        val t = harness()
        t.runToAwaitingStatus()
        t.send(TransportEvent.NotificationReceived(okStatus))
        assertIs<ConnectionState.Settling>(t.machine.state)
        val healthAfterSuccess = t.machine.health
        val reportsAfterSuccess = t.reports.size

        // The watch's second notify, or a duplicate delivery, after we already
        // hung up. Acting on it would double-count the exchange; worse, a stale
        // "Ok" could reset a backoff that a later failure had earned.
        assertTrue(t.send(TransportEvent.NotificationReceived(okStatus)).isEmpty())
        assertIs<ConnectionState.Settling>(t.machine.state)
        assertEquals(healthAfterSuccess, t.machine.health)
        assertEquals(reportsAfterSuccess, t.reports.size)
    }

    @Test
    fun `a notification arriving during a backoff is ignored`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
        assertTrue(t.send(TransportEvent.NotificationReceived(okStatus)).isEmpty())
        assertEquals(1, t.machine.health.consecutiveFailures, "a stray Ok must not clear the counter")
    }

    @Test
    fun `a notify before the Time write is not read as this exchange's result`() {
        // §3.2: a Status read before this connection's write reports the
        // *previous* sync. Treating that as our result would reset the backoff
        // for a sync that never happened.
        val t = harness()
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
        )
        assertEquals(ConnectionState.EnablingNotifications, t.machine.state)
        assertTrue(t.send(TransportEvent.NotificationReceived(okStatus)).isEmpty())
        assertEquals(ConnectionState.EnablingNotifications, t.machine.state)
    }

    @Test
    fun `a notify that beats its own write callback still completes the exchange`() {
        // Android delivers onCharacteristicWrite and onCharacteristicChanged
        // independently; the notification can win. Ignoring it here would mean a
        // 15 s timeout and a backoff step after a sync that actually succeeded.
        val t = harness()
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
        )
        assertEquals(ConnectionState.WritingTime, t.machine.state)

        t.send(TransportEvent.NotificationReceived(okStatus))
        assertIs<ConnectionState.Settling>(t.machine.state)
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertEquals(1L, t.machine.health.totalSuccesses)

        // The write callback then turns up late and changes nothing.
        assertTrue(t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS)).isEmpty())
        assertIs<ConnectionState.Settling>(t.machine.state)
    }

    @Test
    fun `a disconnect while the Time write is pending closes and backs off`() {
        val t = harness()
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
        )
        assertEquals(ConnectionState.WritingTime, t.machine.state)

        val actions = t.send(TransportEvent.Disconnected(8))
        assertTrue(Action.CloseConnection in actions)
        assertEquals(ExchangeOutcome.DisconnectedMidExchange, t.reports.single().outcome)
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)

        // And the write callback that was in flight is harmless afterwards.
        assertTrue(t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS)).isEmpty())
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
    }

    @Test
    fun `the adapter going off mid-operation tears down whatever was in flight`() {
        val states = listOf(
            ConnectionState.Discovering to 1,
            ConnectionState.EnablingNotifications to 2,
            ConnectionState.WritingTime to 3,
            ConnectionState.AwaitingStatus to 4,
        )
        val steps = listOf(
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
            TransportEvent.CharacteristicWritten(GATT_SUCCESS),
        )
        for ((expectedState, depth) in states) {
            val t = harness()
            t.send(TransportEvent.Started)
            steps.take(depth).forEach { t.send(it) }
            assertEquals(expectedState, t.machine.state)

            assertEquals(listOf("CancelTimers", "CloseConnection"), t.send(TransportEvent.AdapterOff).summary())
            assertEquals(ConnectionState.Blocked(adapterOff = true, permissionMissing = false), t.machine.state)
            t.send(TransportEvent.AdapterOn)
            assertEquals(ConnectionState.Armed, t.machine.state, "$expectedState")
        }
    }

    @Test
    fun `empty, truncated and oversized notifications are failed exchanges, not crashes`() {
        val payloads = listOf<Pair<String, ByteArray?>>(
            "null" to null,
            "empty" to ByteArray(0),
            "truncated" to okStatus.copyOfRange(0, 11),
            "oversized" to okStatus + byteArrayOf(0),
            "wrong version" to okStatus.copyOf().also { it[0] = 0x02 },
            "wrong msg_type" to okStatus.copyOf().also { it[1] = 0x01 },
        )
        for ((name, payload) in payloads) {
            val t = harness()
            t.runToAwaitingStatus()
            t.send(TransportEvent.NotificationReceived(payload))
            assertEquals(ExchangeOutcome.MalformedStatus, t.reports.single().outcome, name)
            assertNull(t.reports.single().status, name)
            assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state, name)
            assertEquals(1, t.machine.health.consecutiveFailures, name)
            t.fireRetryTimer()
            assertEquals(ConnectionState.Armed, t.machine.state, name)
        }
    }

    @Test
    fun `a stale timer firing cannot tear down a healthy connection`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
        val staleToken = assertNotNull(t.pendingTimerToken)

        // Discovery finishes; the discovery timeout is replaced. The old timer
        // then fires anyway, because it lost the race with its own cancellation.
        t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
        assertTrue(t.send(TransportEvent.OperationTimedOut(staleToken)).isEmpty())
        assertEquals(ConnectionState.EnablingNotifications, t.machine.state)

        // And the current one still works.
        t.fireOperationTimeout()
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
    }

    @Test
    fun `a stale retry timer cannot arm a second client`() {
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        val token = assertNotNull(t.pendingTimerToken)
        t.fireRetryTimer()
        assertEquals(ConnectionState.Armed, t.machine.state)

        // A duplicate delivery of the same alarm. Acting on it would call
        // connectGatt again and leak the first client.
        assertTrue(t.send(TransportEvent.RetryTimerFired(token)).isEmpty())
        assertTrue(t.send(TransportEvent.RetryTimerFired(token + 99)).isEmpty())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `a retry alarm firing cannot disarm an operation timeout`() {
        // What this pins: a RetryTimerFired carrying the live operation
        // timeout's token is ignored *and leaves that timeout armed*, so the
        // real timer still drives the machine afterwards. The original defect
        // here — found by the walk below — disarmed it and returned, stranding
        // a GATT operation with nothing watching it.
        //
        // It does not pin the TimerKind check specifically: the state guard in
        // onRetryTimerFired is sufficient on its own in the current shape, and
        // this test passes with the kind check removed. See the note on
        // ConnectionStateMachine.armedTimer for why the kind check is kept
        // anyway.
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))
        val operationToken = assertNotNull(t.pendingTimerToken)

        assertTrue(t.send(TransportEvent.RetryTimerFired(operationToken)).isEmpty())
        assertEquals(ConnectionState.Discovering, t.machine.state)

        t.fireOperationTimeout()
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
    }

    @Test
    fun `an operation timeout firing cannot cancel a pending backoff`() {
        // The mirror image, and the worse of the two defects: it left the app in
        // WaitingForRetry with no alarm - installed, looking fine, and silently
        // never talking to the watch again. What is pinned is the same shape as
        // above: the stray delivery is ignored and the retry alarm survives it.
        val t = harness()
        t.send(TransportEvent.Started, TransportEvent.Disconnected(133))
        val retryToken = assertNotNull(t.pendingTimerToken)

        assertTrue(t.send(TransportEvent.OperationTimedOut(retryToken)).isEmpty())
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)

        t.fireRetryTimer()
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `Started is idempotent`() {
        val t = harness()
        assertEquals(listOf("ArmAutoConnect"), t.send(TransportEvent.Started).summary())
        assertTrue(t.send(TransportEvent.Started).isEmpty(), "a second start must not open a second client")
        t.runToAwaitingStatus()
        assertTrue(t.send(TransportEvent.Started).isEmpty())
        assertEquals(ConnectionState.AwaitingStatus, t.machine.state)
    }

    // ── The clock ────────────────────────────────────────────────────────────

    @Test
    fun `a wall clock that jumps backwards is still encoded, just as an earlier time`() {
        val clock = FakeClock(epochSeconds = 1_786_970_096L, offsetMinutes = 180)
        val t = harness(clock)
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
        )
        // NTP corrects the phone backwards a full day just before the write.
        clock.epochSeconds -= 86_400
        t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))

        assertContentEquals(
            com.workaday.core.protocol.WatchProtocol.encodeTime(1_786_970_096L - 86_400, 180),
            t.lastTimePayload,
        )
        assertEquals(ConnectionState.WritingTime, t.machine.state)
    }

    @Test
    fun `a monotonic clock that jumps backwards cannot lengthen the exchange budget`() {
        val clock = FakeClock()
        val t = harness(clock)
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))

        // Should be impossible; the arithmetic is clamped anyway, because a
        // budget that never expires is a stall and stalls are what Law 2 forbids.
        clock.monotonic -= 1_000_000
        t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
        val armed = assertNotNull(t.pendingTimerDelayMillis)
        assertTrue(armed <= 15_000L, "budget grew to $armed ms after the clock went backwards")
        assertEquals(5_000L, armed)
    }

    @Test
    fun `a phone clock outside the wire format fails the exchange instead of sending garbage`() {
        // Epoch 0x1_0000_0000 is 2106 and does not fit the u32. Truncating it
        // would set the watch to 1970 and report success.
        val clock = FakeClock(epochSeconds = 0x1_0000_0000L)
        val t = harness(clock)
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
        )
        val actions = t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))

        assertTrue(actions.none { it is Action.WriteTime }, "nothing may go on the wire")
        assertEquals(ExchangeOutcome.LocalClockUnusable, t.reports.single().outcome)
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
        t.fireRetryTimer()
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `a pre-1970 phone clock is refused the same way`() {
        val clock = FakeClock(epochSeconds = -1)
        val t = harness(clock)
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
        )
        assertEquals(ExchangeOutcome.LocalClockUnusable, t.reports.single().outcome)
        assertEquals(ConnectionState.WaitingForRetry(30_000), t.machine.state)
    }

    // ── §4.1, find phone ─────────────────────────────────────────────────────

    /** The §4 exchange, ending in the watch's flagged Status: the phone is now ringing. */
    private fun FakeTransport.runToRinging() {
        runToAwaitingStatus()
        send(TransportEvent.NotificationReceived(findStatus))
        assertEquals(ConnectionState.Ringing, machine.state)
    }

    @Test
    fun `find phone - a Status carrying FIND_PHONE keeps the link open and starts the alarm`() {
        val t = harness(health = HealthSnapshot(consecutiveFailures = 2))
        t.runToAwaitingStatus()
        assertEquals(
            listOf(
                "CancelTimers",
                "PersistHealth",
                "ReportExchange(Succeeded)",
                "StartFindAlarm",
                // The §5.2 ring backstop, not a per-operation timeout: nothing is
                // outstanding on the link, and this is how long the phone will ring
                // with no word from the watch.
                "ArmOperationTimeout(135000)",
            ),
            t.send(TransportEvent.NotificationReceived(findStatus)).summary(),
        )
        assertEquals(ConnectionState.Ringing, t.machine.state)
        assertTrue(t.linkOpen, "the link is held open so the watch can end the ring by hanging up")
        assertTrue(t.alarmOn)
        // Recorded exactly as the ordinary success it also was: the counters, the
        // record, the battery — nothing about the flag changes what the diagnostic
        // screen learns.
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertEquals(1L, t.machine.health.totalSuccesses)
        assertEquals(ExchangeOutcome.Succeeded, t.reports.single().outcome)
        assertEquals(78, t.reports.single().status?.batteryPercent)
        assertEquals(true, t.reports.single().status?.findPhoneRequested)
    }

    @Test
    fun `find phone - the watch hanging up stops the alarm and re-arms at once`() {
        // §4.1's normal ending: Back pressed on the watch, or its two-minute cap.
        // Also what a lost link looks like, and the answer is the same — the watch
        // may still be looking, so no settle and no backoff.
        val t = harness()
        t.runToRinging()
        assertEquals(
            listOf("CancelTimers", "StopFindAlarm", "CloseConnection", "ArmAutoConnect"),
            t.send(TransportEvent.Disconnected(19)).summary(),
        )
        assertEquals(ConnectionState.Armed, t.machine.state)
        assertFalse(t.alarmOn)
        assertEquals(0, t.machine.health.consecutiveFailures, "the watch ending its search is not a fault")
        assertEquals(1, t.reports.size, "nothing new to report — the exchange was recorded when the Status arrived")
    }

    @Test
    fun `find phone - dismissing on the phone silences it, writes Find, and closes on the reply`() {
        val t = harness()
        t.runToRinging()

        assertEquals(
            listOf("CancelTimers", "StopFindAlarm", "ArmOperationTimeout(5000)", "WriteFindDismiss(4)"),
            t.send(TransportEvent.FindDismissedOnPhone).summary(),
        )
        assertEquals(ConnectionState.DismissingFind, t.machine.state)
        assertFalse(t.alarmOn, "the alarm stops at the tap, not when the watch answers")
        assertTrue(t.linkOpen, "the link stays up for the write")
        // PROTOCOL.md §7.5, byte for byte.
        assertContentEquals(hex("01 02 00 00"), t.lastFindPayload)

        assertEquals(
            listOf("CancelTimers", "CloseConnection", "ArmAutoConnect"),
            t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS)).summary(),
        )
        assertEquals(ConnectionState.Armed, t.machine.state)
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertEquals(1, t.reports.size)
    }

    @Test
    fun `find phone - a dismiss write that fails still ends closed and armed, and is not a failed exchange`() {
        // An older watch has no Find characteristic, so the Android layer reports a
        // write it could not issue (GATT_LOCAL_FAILURE); or the stack refuses it.
        // Either way the user already silenced the phone, the exchange finished
        // when the Status arrived, and there is nothing to back off from.
        for (status in listOf(GATT_LOCAL_FAILURE, 133)) {
            val t = harness()
            t.runToRinging()
            t.send(TransportEvent.FindDismissedOnPhone)
            t.send(TransportEvent.CharacteristicWritten(status))
            assertEquals(ConnectionState.Armed, t.machine.state, "status $status")
            assertEquals(0, t.machine.health.consecutiveFailures, "status $status")
            assertEquals(1, t.reports.size, "status $status")
        }
    }

    @Test
    fun `find phone - the watch hanging up mid-dismiss still ends armed`() {
        // The watch processed the frame and ended its search before our write
        // callback came back, or the link dropped. Same ending.
        val t = harness()
        t.runToRinging()
        t.send(TransportEvent.FindDismissedOnPhone)
        assertEquals(
            listOf("CancelTimers", "CloseConnection", "ArmAutoConnect"),
            t.send(TransportEvent.Disconnected(19)).summary(),
        )
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `find phone - a dismiss write that never completes times out into armed`() {
        val t = harness()
        t.runToRinging()
        t.send(TransportEvent.FindDismissedOnPhone)
        assertEquals(5_000L, t.pendingTimerDelayMillis, "the §5.2 per-operation timeout, unclamped by a long-spent exchange budget")
        assertEquals(listOf("CloseConnection", "ArmAutoConnect"), t.fireOperationTimeout().summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
    }

    @Test
    fun `find phone - the ring backstop ends a ring the watch never hung up`() {
        // §5.2: longer than the watch's cap, so this only fires when the disconnect
        // never reached us. Stop, close, re-arm — the net, not the normal ending.
        val t = harness()
        t.runToRinging()
        assertEquals(135_000L, t.pendingTimerDelayMillis)
        assertEquals(listOf("StopFindAlarm", "CloseConnection", "ArmAutoConnect"), t.fireOperationTimeout().summary())
        assertEquals(ConnectionState.Armed, t.machine.state)
        assertFalse(t.alarmOn)
    }

    @Test
    fun `find phone - the radio going away silences the alarm`() {
        for (event in listOf(TransportEvent.AdapterOff, TransportEvent.PermissionRevoked)) {
            val t = harness()
            t.runToRinging()
            assertEquals(listOf("CancelTimers", "StopFindAlarm", "CloseConnection"), t.send(event).summary(), "$event")
            assertIs<ConnectionState.Blocked>(t.machine.state, "$event")
            assertFalse(t.alarmOn, "$event")
            // And it comes back armed, as from any Blocked.
            t.send(TransportEvent.AdapterOn)
            t.send(TransportEvent.PermissionGranted)
            assertEquals(ConnectionState.Armed, t.machine.state, "$event")
        }
    }

    @Test
    fun `find phone - a refused clock still rings, and is still recorded as the failure it was`() {
        // §4.1: "whatever result says". The watch could not set its clock — Busy —
        // but it is looking for this phone, and the two facts are read apart. The
        // failure is counted like any other; the alarm starts like any other.
        val t = harness()
        t.runToAwaitingStatus()
        assertEquals(
            listOf(
                "CancelTimers",
                "PersistHealth",
                "ReportExchange(WatchReportedFailure)",
                "StartFindAlarm",
                "ArmOperationTimeout(135000)",
            ),
            t.send(TransportEvent.NotificationReceived(findBusyStatus)).summary(),
        )
        assertEquals(ConnectionState.Ringing, t.machine.state)
        assertEquals(1, t.machine.health.consecutiveFailures)
        assertEquals(6, t.machine.health.lastStatusResultCode)

        // And when the watch hangs up, the phone still re-arms at once: a ring that
        // ended is not a reason to wait 30 s before the watch can find us again.
        val actions = t.send(TransportEvent.Disconnected(19))
        assertTrue(actions.none { it is Action.ScheduleRetry })
        assertEquals(ConnectionState.Armed, t.machine.state)
        // The failure count survives for the next ordinary failure's backoff.
        assertEquals(1, t.machine.health.consecutiveFailures)
    }

    @Test
    fun `find phone - a notify that beats its own write callback still rings`() {
        val t = harness()
        t.send(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
        )
        assertEquals(ConnectionState.WritingTime, t.machine.state)
        t.send(TransportEvent.NotificationReceived(findStatus))
        assertEquals(ConnectionState.Ringing, t.machine.state)
        // The late write callback changes nothing — and in particular does not
        // read as the dismiss write completing.
        assertTrue(t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS)).isEmpty())
        assertEquals(ConnectionState.Ringing, t.machine.state)
    }

    @Test
    fun `find phone - stray events while ringing change nothing`() {
        val t = harness()
        t.runToRinging()
        val strays = listOf(
            TransportEvent.Started,
            TransportEvent.Connected(GATT_SUCCESS),
            TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            TransportEvent.DescriptorWritten(GATT_SUCCESS),
            TransportEvent.CharacteristicWritten(GATT_SUCCESS),
            TransportEvent.NotificationReceived(okStatus),
            TransportEvent.NotificationReceived(findStatus),
            TransportEvent.NotificationReceived(ByteArray(3)),
            TransportEvent.RetryTimerFired(t.pendingTimerToken ?: -1L),
            TransportEvent.AdapterOn,
            TransportEvent.PermissionGranted,
        )
        for (stray in strays) {
            assertTrue(t.send(stray).isEmpty(), "$stray")
            assertEquals(ConnectionState.Ringing, t.machine.state, "$stray")
        }
        assertEquals(1, t.reports.size, "a second flagged Status must not be double-counted")
    }

    @Test
    fun `find phone - dismissing when nothing is ringing is ignored`() {
        val armed = harness().also { it.send(TransportEvent.Started) }
        assertTrue(armed.send(TransportEvent.FindDismissedOnPhone).isEmpty())
        assertEquals(ConnectionState.Armed, armed.machine.state)

        val awaiting = harness().also { it.runToAwaitingStatus() }
        assertTrue(awaiting.send(TransportEvent.FindDismissedOnPhone).isEmpty())
        assertEquals(ConnectionState.AwaitingStatus, awaiting.machine.state)

        val settling = harness().also { it.runToAwaitingStatus(); it.send(TransportEvent.NotificationReceived(okStatus)) }
        assertTrue(settling.send(TransportEvent.FindDismissedOnPhone).isEmpty())
        assertIs<ConnectionState.Settling>(settling.machine.state)

        // And twice in a row is once.
        val dismissing = harness().also { it.runToRinging(); it.send(TransportEvent.FindDismissedOnPhone) }
        assertTrue(dismissing.send(TransportEvent.FindDismissedOnPhone).isEmpty())
        assertEquals(ConnectionState.DismissingFind, dismissing.machine.state)
    }

    @Test
    fun `find phone - a plain Status still settles - the flag is what makes the difference`() {
        // The control: identical bytes but for bit 0 of byte 10, and the machine
        // takes the §4 path it always took.
        val t = harness()
        t.runToAwaitingStatus()
        val actions = t.send(TransportEvent.NotificationReceived(okStatus))
        assertTrue(actions.none { it == Action.StartFindAlarm })
        assertIs<ConnectionState.Settling>(t.machine.state)
        assertFalse(t.linkOpen)
    }

    // ── Law 2's invariant, over everything reachable ─────────────────────────

    /**
     * The events fed to the two walks below. Tokens are resolved against the
     * transport at the moment the event is built, so "the current timer fires"
     * is a real event rather than a guess.
     */
    private val eventFactories: List<Pair<String, (FakeTransport) -> TransportEvent>> = listOf(
        "Started" to { _ -> TransportEvent.Started },
        "Connected(0)" to { _ -> TransportEvent.Connected(GATT_SUCCESS) },
        "Connected(133)" to { _ -> TransportEvent.Connected(133) },
        "Disconnected(0)" to { _ -> TransportEvent.Disconnected(GATT_SUCCESS) },
        "Disconnected(133)" to { _ -> TransportEvent.Disconnected(133) },
        "ServicesDiscovered(0)" to { _ -> TransportEvent.ServicesDiscovered(GATT_SUCCESS) },
        "ServicesDiscovered(133)" to { _ -> TransportEvent.ServicesDiscovered(133) },
        "DescriptorWritten(0)" to { _ -> TransportEvent.DescriptorWritten(GATT_SUCCESS) },
        "DescriptorWritten(133)" to { _ -> TransportEvent.DescriptorWritten(133) },
        "CharacteristicWritten(0)" to { _ -> TransportEvent.CharacteristicWritten(GATT_SUCCESS) },
        "Notify(ok)" to { _ -> TransportEvent.NotificationReceived(okStatus.copyOf()) },
        "Notify(busy)" to { _ -> TransportEvent.NotificationReceived(busyStatus.copyOf()) },
        "Notify(garbage)" to { _ -> TransportEvent.NotificationReceived(ByteArray(3)) },
        "Notify(find)" to { _ -> TransportEvent.NotificationReceived(findStatus.copyOf()) },
        "FindDismissed" to { _ -> TransportEvent.FindDismissedOnPhone },
        "Timeout(current)" to { t -> TransportEvent.OperationTimedOut(t.pendingTimerToken ?: -1L) },
        "Retry(current)" to { t -> TransportEvent.RetryTimerFired(t.pendingTimerToken ?: -1L) },
        "AdapterOff" to { _ -> TransportEvent.AdapterOff },
        "AdapterOn" to { _ -> TransportEvent.AdapterOn },
        "PermissionRevoked" to { _ -> TransportEvent.PermissionRevoked },
        "PermissionGranted" to { _ -> TransportEvent.PermissionGranted },
    )

    /**
     * The prefixes the exhaustive walk is seeded from — one per state that
     * takes a specific sequence of successes to reach.
     *
     * Three events after `Started` cannot reach [ConnectionState.AwaitingStatus]
     * at all: it needs four callbacks in order. So the original walk covered it
     * zero times while being presented as the evidence that nothing can strand
     * the app — and an injected stranding bug in exactly that state sailed
     * through it. Running the same triples from each prefix fixes that by
     * construction rather than by hoping the random walk wanders in.
     */
    private val walkPrefixes: List<Triple<String, List<TransportEvent>, ConnectionState>> = listOf(
        // Nothing sent yet, so the triples run against a service that has not
        // started. Cheap, and the only way Idle is walked with real events.
        Triple("unstarted", emptyList(), ConnectionState.Idle),
        Triple("armed", listOf(TransportEvent.Started), ConnectionState.Armed),
        Triple(
            "discovering",
            listOf(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS)),
            ConnectionState.Discovering,
        ),
        Triple(
            "enabling-notifications",
            listOf(
                TransportEvent.Started,
                TransportEvent.Connected(GATT_SUCCESS),
                TransportEvent.ServicesDiscovered(GATT_SUCCESS),
            ),
            ConnectionState.EnablingNotifications,
        ),
        Triple(
            "writing-time",
            listOf(
                TransportEvent.Started,
                TransportEvent.Connected(GATT_SUCCESS),
                TransportEvent.ServicesDiscovered(GATT_SUCCESS),
                TransportEvent.DescriptorWritten(GATT_SUCCESS),
            ),
            ConnectionState.WritingTime,
        ),
        Triple(
            "awaiting-status",
            listOf(
                TransportEvent.Started,
                TransportEvent.Connected(GATT_SUCCESS),
                TransportEvent.ServicesDiscovered(GATT_SUCCESS),
                TransportEvent.DescriptorWritten(GATT_SUCCESS),
                TransportEvent.CharacteristicWritten(GATT_SUCCESS),
            ),
            ConnectionState.AwaitingStatus,
        ),
        // The two find-phone states need the whole exchange plus the flagged
        // Status to reach, so a triple from any earlier prefix visits them rarely
        // or never; seeded like AwaitingStatus, for the same reason.
        Triple(
            "ringing",
            listOf(
                TransportEvent.Started,
                TransportEvent.Connected(GATT_SUCCESS),
                TransportEvent.ServicesDiscovered(GATT_SUCCESS),
                TransportEvent.DescriptorWritten(GATT_SUCCESS),
                TransportEvent.CharacteristicWritten(GATT_SUCCESS),
                TransportEvent.NotificationReceived(findStatus),
            ),
            ConnectionState.Ringing,
        ),
        Triple(
            "dismissing-find",
            listOf(
                TransportEvent.Started,
                TransportEvent.Connected(GATT_SUCCESS),
                TransportEvent.ServicesDiscovered(GATT_SUCCESS),
                TransportEvent.DescriptorWritten(GATT_SUCCESS),
                TransportEvent.CharacteristicWritten(GATT_SUCCESS),
                TransportEvent.NotificationReceived(findStatus),
                TransportEvent.FindDismissedOnPhone,
            ),
            ConnectionState.DismissingFind,
        ),
    )

    @Test
    fun noThreeEventSequenceStrandsTheApp() {
        // Every event triple, exhaustively, from every state the machine can be
        // sitting in when the triple starts. FakeTransport re-checks Law 2's
        // invariant after each event, and refuses a leaked or double-closed GATT
        // client along the way.
        val coverage = StateCoverage()
        val n = eventFactories.size
        var sequences = 0
        for ((name, prefix, expectedState) in walkPrefixes) {
            for (a in 0 until n) {
                for (b in 0 until n) {
                    for (c in 0 until n) {
                        val t = harness(coverage = coverage)
                        prefix.forEach { t.send(it) }
                        // The seeding is itself checked: a prefix that stopped
                        // reaching its state would otherwise quietly turn this
                        // back into six copies of the same shallow walk.
                        assertEquals(expectedState, t.machine.state, "prefix $name")
                        for (index in intArrayOf(a, b, c)) {
                            t.send(eventFactories[index].second(t))
                        }
                        sequences++
                    }
                }
            }
        }
        assertEquals(walkPrefixes.size * n * n * n, sequences)
        assertTrue(sequences > 30_000, "the walk should be exhaustive, not a token sample")
        // The part that keeps this fixed rather than fixed-once: without it, the
        // next edit to eventFactories or walkPrefixes can silently stop reaching
        // a state and nothing says so.
        //
        // The threshold is well under what this walk actually achieves — the
        // thinnest state, Settling, is visited about 3,600 times, and the two
        // find-phone states 25,000 or more thanks to their own prefixes — so an
        // ordinary edit to the event list cannot trip it, while losing a state
        // to unreachability does.
        coverage.assertEveryStateVisited(atLeast = 2_000, walk = "the exhaustive triple walk")
    }

    /**
     * The same events, weighted so a random stream spends real time mid-exchange.
     *
     * Unweighted, `Blocked` is near-absorbing — leaving it needs one of two
     * specific events — and it soaked up 60 % of the observed state-time while
     * `AwaitingStatus` was reached once in 61,500 steps. The weights below are
     * about reachability, not realism: advancing the exchange is made common,
     * and the two clearing events are made likelier than the two blocking ones.
     */
    private val weightedEventFactories: List<(FakeTransport) -> TransportEvent> = run {
        val weights = mapOf(
            "Connected(0)" to 6,
            "ServicesDiscovered(0)" to 6,
            "DescriptorWritten(0)" to 6,
            "CharacteristicWritten(0)" to 6,
            "Retry(current)" to 6,
            // Settling needs a plain Ok inside the 15 s budget, and it now shares
            // its two source states with the flagged Status below; six keeps it
            // above the floor once the find events joined the pool.
            "Notify(ok)" to 6,
            // Heavy, like the exchange steps: Ringing is reached only through a
            // flagged Status from WritingTime or AwaitingStatus, and DismissingFind
            // only from Ringing, so both are two rare draws deep without these.
            "Notify(find)" to 6,
            "FindDismissed" to 8,
            // Blocked is near-absorbing (see above); a little more weight on the
            // two events that leave it buys every other state more of the stream.
            "AdapterOn" to 4,
            "PermissionGranted" to 4,
            "Notify(busy)" to 2,
            "Notify(garbage)" to 2,
            "Timeout(current)" to 2,
            "Disconnected(133)" to 2,
        )
        eventFactories.flatMap { (name, factory) ->
            List(weights[name] ?: 1) { factory }
        }
    }

    @Test
    fun `long random event streams always end somewhere the app can recover from`() {
        val coverage = StateCoverage()
        val random = Random(20260817)
        // 3,000 streams, up from 1,500 when the two find-phone states joined: each
        // is one more rare draw deep than AwaitingStatus, and the floor below is
        // meant to be cleared with room, not grazed.
        repeat(3_000) { run ->
            val clock = FakeClock()
            val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)), coverage)
            // A few events before the service starts, so Idle is walked too and
            // "the adapter was already off at boot" turns up by accident as well
            // as on purpose.
            repeat(random.nextInt(0, 3)) {
                t.send(weightedEventFactories[random.nextInt(weightedEventFactories.size)](t))
            }
            t.send(TransportEvent.Started)
            repeat(60) {
                // 0-4 s per event, not the 0-20 s this used to advance.
                //
                // The old figure meant a five-event exchange averaged 25 s of
                // clock, so **no random run ever completed one inside its 15 s
                // budget** — 61,500 steps of a machine that never quite succeeded
                // in time. Nothing said so until Settling was added and the
                // coverage floor reported it as reached zero times.
                //
                // Nothing is lost by shortening it: the timers here fire because
                // the walk *sends* OperationTimedOut, not because the clock
                // advanced, and the budget clamping the long jumps exercised has
                // its own test above. The weights in this walk are about
                // reachability rather than realism, and so is this.
                clock.advance(random.nextLong(0, 4_000))
                t.send(weightedEventFactories[random.nextInt(weightedEventFactories.size)](t))
            }
            driveBackToArmed(t, "run $run")
        }
        // Thinnest observed is DismissingFind at ~350 visits, then AwaitingStatus
        // at ~500 and Settling at ~630; the walk is deterministic (fixed seed,
        // fixed weights), so this only moves when somebody edits the event list,
        // the weights or the stream count.
        coverage.assertEveryStateVisited(atLeast = 200, walk = "the random walk")
    }

    /**
     * From wherever the stream left it, a bounded number of *defined* events
     * puts the app back in its resting state. That is what "no terminal error
     * state" means operationally: not that nothing goes wrong, but that nothing
     * that goes wrong is permanent.
     */
    private fun driveBackToArmed(t: FakeTransport, label: String) {
        t.send(TransportEvent.AdapterOn)
        t.send(TransportEvent.PermissionGranted)
        var steps = 0
        while (t.machine.state != ConnectionState.Armed) {
            if (steps++ > 8) fail("$label: still not armed after $steps recovery steps, state ${t.machine.state}")
            when (t.machine.state) {
                is ConnectionState.WaitingForRetry -> t.fireRetryTimer()
                ConnectionState.Discovering,
                ConnectionState.EnablingNotifications,
                ConnectionState.WritingTime,
                ConnectionState.AwaitingStatus,
                // Settling is a waiting state too, and the same timer ends it.
                is ConnectionState.Settling,
                // As are a ring (the §5.2 backstop) and a dismiss (its write timeout).
                ConnectionState.Ringing,
                ConnectionState.DismissingFind,
                -> t.fireOperationTimeout()
                else -> fail("$label: stuck in ${t.machine.state}")
            }
        }
    }
}

/** Actions rendered without their timer tokens, which are an internal detail. */
private fun List<Action>.summary(): List<String> = map { action ->
    when (action) {
        is Action.ArmOperationTimeout -> "ArmOperationTimeout(${action.timeoutMillis})"
        is Action.ScheduleRetry -> "ScheduleRetry(${action.delayMillis})"
        is Action.PersistHealth -> "PersistHealth"
        is Action.ReportExchange -> "ReportExchange(${action.outcome})"
        is Action.WriteTime -> "WriteTime(${action.payload.size})"
        is Action.WriteFindDismiss -> "WriteFindDismiss(${action.payload.size})"
        else -> action.toString()
    }
}
