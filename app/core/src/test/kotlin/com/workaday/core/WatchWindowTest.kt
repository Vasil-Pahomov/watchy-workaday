package com.workaday.core

import com.workaday.core.protocol.WatchProtocol
import com.workaday.core.protocol.hex
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * One watch window, with a watch in it.
 *
 * **This file is the repair for a gap in `ConnectionStateMachineTest`**, which is
 * the most heavily tested thing in this repo - 44 tests, exhaustive triple walks
 * from six prefixes, 1,500 weighted random streams, a per-state coverage floor -
 * and which nonetheless passed a machine that started a second exchange 16 ms
 * after finishing a successful one, on every sync, on real hardware.
 *
 * It could not have caught it, and the reasons compound:
 *
 * 1. **`FakeTransport` never answers.** It executes actions and records their
 *    consequences; it never *produces* an event. `Action.ArmAutoConnect` is a
 *    bookkeeping flag there, while on a phone it is a standing request that a
 *    connectable peer answers in milliseconds. Every test therefore chose for
 *    itself whether a `Connected` followed an arm, and they all chose "later" -
 *    because an hourly watch is what the design had in mind. Nothing in the suite
 *    modelled a peripheral that is *still advertising* when the exchange ends,
 *    which is the only situation in which the bug exists.
 * 2. **The invariant was "never stranded", not "never runs away".** Law 2 asks
 *    that no path leave the app neither connected nor waiting, and the fake checks
 *    exactly that after every event. A second exchange violates nothing: it
 *    connects, exchanges, and ends somewhere recoverable. A repeat of a healthy
 *    sequence is the healthiest-looking failure there is.
 * 3. **`timeWritesThisConnection` counts per connection**, which is PROTOCOL.md
 *    §4's rule and was enforced correctly throughout - one Time push per
 *    connection, always, including in the second exchange. The rule that broke is
 *    §1's and §5.1's, *one exchange per window*, and a window belongs to the watch.
 *    The fake had no watch in it, so the property had nowhere to live.
 *
 * So this file supplies the missing half: a peer that stays connectable for the
 * length of its window and answers an `autoConnect` at once. Everything else is
 * the real machine and the real [FakeTransport].
 *
 * **It is deliberately not folded into `FakeTransport`.** Making that answer arms
 * on its own would rewrite the premise of all 44 existing tests, whose value comes
 * precisely from choosing their own events - a machine driven by an adversary is
 * what found the stranding bugs. The peer belongs beside it, not inside it.
 */
class WatchWindowTest {

    /** §7.2's Status frame: result Ok, battery 78 %, fw_build 1. */
    private val okStatus = hex("01 81 00 4E F0 FF 82 6A 01 00 00 00")

    /** A GATT round trip. Small enough that many fit in a window, which is the point. */
    private val roundTripMillis = 50L

    private class Window(val clock: FakeClock, val transport: FakeTransport) {
        /** §5.1: the watch tears its radio down at the cap, whatever is happening. */
        val closesAtMonotonic = clock.monotonic + WatchProtocol.WATCH_SESSION_CAP_MS
        val isOpen: Boolean get() = clock.monotonic < closesAtMonotonic
        var successes = 0
    }

    private fun timeWrites(t: FakeTransport) = t.emitted.count { it is Action.WriteTime }

    /**
     * Run one window to its close, with the watch answering as a watch does.
     *
     * The loop is a simulator, not a script: at each step it looks at what the
     * machine is waiting for and supplies what a real watch and a real Bluetooth
     * stack would supply. That is the difference between this and every other test
     * of the machine, where the test chooses the events.
     */
    private fun runWindow(w: Window) {
        val t = w.transport
        var steps = 0
        while (w.isOpen) {
            if (steps++ > 1_000) fail("the window simulator did not terminate; state ${t.machine.state}")
            when (val state = t.machine.state) {
                // A pending autoConnect against a peer that is advertising and
                // connectable. The controller fires; the app does not get a say.
                ConnectionState.Armed -> {
                    w.clock.advance(roundTripMillis)
                    t.send(TransportEvent.Connected(GATT_SUCCESS))
                }

                ConnectionState.Discovering -> {
                    w.clock.advance(roundTripMillis)
                    t.send(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
                }

                ConnectionState.EnablingNotifications -> {
                    w.clock.advance(roundTripMillis)
                    t.send(TransportEvent.DescriptorWritten(GATT_SUCCESS))
                }

                ConnectionState.WritingTime -> {
                    w.clock.advance(roundTripMillis)
                    t.send(TransportEvent.CharacteristicWritten(GATT_SUCCESS))
                }

                // §4: the watch notifies only after processing a Time write, and
                // here it notifies result 0 because the write was valid - which is
                // what the hardware did, on the first exchange and on the second.
                ConnectionState.AwaitingStatus -> {
                    w.clock.advance(roundTripMillis)
                    t.send(TransportEvent.NotificationReceived(okStatus.copyOf()))
                    w.successes++
                }

                // The app is deliberately holding off. Jump to whichever comes
                // first: its timer, or the end of the window.
                is ConnectionState.Settling -> if (!advanceToTimer(w, state.delayMillis)) return
                is ConnectionState.WaitingForRetry -> if (!advanceToTimer(w, state.delayMillis)) return

                ConnectionState.Idle, is ConnectionState.Blocked ->
                    fail("unreachable in this simulation: $state")
            }
        }
    }

    /**
     * @return false when the timer falls outside the window — the watch is gone
     *   before the app wakes, which is the whole intent of a settle.
     */
    private fun advanceToTimer(w: Window, delayMillis: Long): Boolean {
        val firesAt = w.clock.monotonic + delayMillis
        if (firesAt >= w.closesAtMonotonic) {
            w.clock.monotonic = w.closesAtMonotonic
            return false
        }
        w.clock.advance(delayMillis)
        val state = w.transport.machine.state
        if (state is ConnectionState.Settling) w.transport.fireOperationTimeout() else w.transport.fireRetryTimer()
        return true
    }

    @Test
    fun `one window produces one exchange, not as many as fit in it`() {
        // The defect, reproduced. On the pre-fix machine a success went straight
        // back to Armed while the watch was still advertising, so the peer answered
        // the fresh autoConnect at once and the machine - correctly, from its own
        // point of view - began another exchange. On hardware that took 16 ms;
        // here, with a 50 ms round trip, this loop fills the window with them.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started)

        val window = Window(clock, t)
        runWindow(window)

        assertEquals(1, window.successes, "PROTOCOL.md §4: the phone pushes the time once per connection")
        assertEquals(
            1,
            timeWrites(t),
            "the watch validated and committed a Time write for each of these, and paid radio and RTC for it",
        )
    }

    @Test
    fun `a successful sync is not followed by a failed one`() {
        // The part that actually cost something, straight from the phone log: the
        // second exchange began 16 ms after the success, the watch shut its window
        // half way through it, and the phone sat in AwaitingStatus until the link
        // dropped five seconds later - recording DisconnectedMidExchange, a 36 s
        // backoff step and a health increment, for an exchange that had no reason
        // to exist. Every successful sync was followed by a spurious failed one,
        // so health never stayed clean and the backoff never stayed at its base.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started)

        val window = Window(clock, t)
        runWindow(window)

        // The watch is gone. Anything the app had in flight now dies with it.
        if (t.linkOpen) t.send(TransportEvent.Disconnected(GATT_SUCCESS))

        assertEquals(
            listOf(ExchangeOutcome.Succeeded),
            t.reports.map { it.outcome },
            "a window that synced once must report once, and must not report a failure",
        )
        assertEquals(0, t.machine.health.consecutiveFailures, "a successful sync must leave health clean")
        assertEquals(1L, t.machine.health.totalSuccesses)
        assertEquals(0L, t.machine.health.totalFailures)
    }

    @Test
    fun `a connection arriving during the settle is not the start of a new exchange`() {
        // Belt and braces against the same mechanism from the other direction. In
        // app/ there is no GATT client during a settle and WatchLink's epoch gate
        // drops anything late, so this should be unreachable - but "should be
        // unreachable" is what was believed about a second exchange too.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started)
        runWindow(Window(clock, t))

        val settling = t.machine.state
        assertTrue(settling is ConnectionState.Settling)
        assertTrue(t.send(TransportEvent.Connected(GATT_SUCCESS)).isEmpty(), "the window is spent; do not act on it")
        assertEquals(settling, t.machine.state)
    }

    @Test
    fun `the app is armed again long before the watch's next window`() {
        // The other half, and the one that fails if the settle is made too long:
        // holding off is only correct while it still leaves the app waiting before
        // the watch next appears. §5.1 puts that an hour out.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started)

        val window = Window(clock, t)
        runWindow(window)

        val settling = t.machine.state
        assertTrue(settling is ConnectionState.Settling, "the app should be waiting out the window, was $settling")
        assertTrue(
            settling.delayMillis < WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS * 1_000L,
            "a settle of ${settling.delayMillis} ms would still be running when the watch reappears",
        )

        t.fireOperationTimeout()
        assertEquals(ConnectionState.Armed, t.machine.state, "the settle must end at the resting state")
        assertEquals(0, t.machine.health.consecutiveFailures, "waiting out a window is not a failure")
    }

    @Test
    fun `a failed exchange was never able to re-enter the window, and still cannot`() {
        // Worth pinning because it explains the shape of the bug: the failure path
        // has always gone through a §5.2 backoff of at least 30 s, which already
        // outlasts the watch's 12 s cap. Success was the only path that re-armed
        // with no delay at all - which is why the second exchange only ever
        // followed a *successful* one, and why no failure behaved this way.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started, TransportEvent.Connected(GATT_SUCCESS))

        val window = Window(clock, t)
        clock.advance(roundTripMillis)
        t.send(TransportEvent.ServicesDiscovered(133))

        val retry = t.machine.state
        assertTrue(retry is ConnectionState.WaitingForRetry)
        assertTrue(
            clock.monotonic + retry.delayMillis >= window.closesAtMonotonic,
            "a backoff of ${retry.delayMillis} ms would re-arm inside the watch's own window",
        )
    }

    @Test
    fun `a watch that hangs up after the exchange is not waited out unnecessarily long`() {
        // The normal ending per §4: the watch disconnects and sleeps. The app is in
        // its settle, and the disconnect is the echo of its own close — it must not
        // shorten the settle, and it must not be counted as a fault either.
        val clock = FakeClock()
        val t = FakeTransport(ConnectionStateMachine(clock, Backoff(JitterSource.NONE)))
        t.send(TransportEvent.Started)
        runWindow(Window(clock, t))

        val before = t.machine.state
        assertTrue(before is ConnectionState.Settling)

        assertTrue(t.send(TransportEvent.Disconnected(GATT_SUCCESS)).isEmpty(), "our own hang-up is not an event")
        assertEquals(before, t.machine.state)
        assertEquals(0, t.machine.health.consecutiveFailures)
        assertEquals(1L, t.machine.health.totalSuccesses)
    }
}
