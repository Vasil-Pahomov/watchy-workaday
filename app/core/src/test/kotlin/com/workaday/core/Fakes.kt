package com.workaday.core

import kotlin.test.assertNotNull
import kotlin.test.fail

/**
 * One label per [ConnectionState] variant.
 *
 * This enum is what [StateCoverage] requires a walk to have visited, and
 * [label] is the only thing that produces it. Adding a ninth [ConnectionState]
 * breaks [label]'s exhaustive `when` at compile time, and giving it an entry
 * here is then the obvious repair — which is what stops a new state slipping
 * into the machine without any walk ever reaching it.
 */
enum class StateLabel {
    Idle,
    Armed,
    WaitingForRetry,
    Settling,
    Blocked,
    Discovering,
    EnablingNotifications,
    WritingTime,
    AwaitingStatus,
    SubscribingFind,
    Ringing,
    DismissingFind,
}

fun ConnectionState.label(): StateLabel = when (this) {
    ConnectionState.Idle -> StateLabel.Idle
    ConnectionState.Armed -> StateLabel.Armed
    is ConnectionState.WaitingForRetry -> StateLabel.WaitingForRetry
    is ConnectionState.Settling -> StateLabel.Settling
    is ConnectionState.Blocked -> StateLabel.Blocked
    ConnectionState.Discovering -> StateLabel.Discovering
    ConnectionState.EnablingNotifications -> StateLabel.EnablingNotifications
    ConnectionState.WritingTime -> StateLabel.WritingTime
    ConnectionState.AwaitingStatus -> StateLabel.AwaitingStatus
    is ConnectionState.SubscribingFind -> StateLabel.SubscribingFind
    ConnectionState.Ringing -> StateLabel.Ringing
    ConnectionState.DismissingFind -> StateLabel.DismissingFind
}

/**
 * How often a walk observed each state.
 *
 * A walk that never reaches a state proves nothing about it, however many
 * millions of events it feeds. The exhaustive walk used to reach
 * [StateLabel.AwaitingStatus] exactly zero times — the state that owns the
 * whole-exchange backstop and every notification path — while still being
 * presented as the evidence that no sequence can strand the app. Counting, and
 * failing on a thin count, is what keeps that from silently coming back the
 * next time the event list is edited.
 */
class StateCoverage {
    private val counts = mutableMapOf<StateLabel, Int>()

    fun record(state: ConnectionState) {
        counts.merge(state.label(), 1) { a, b -> a + b }
    }

    operator fun get(label: StateLabel): Int = counts[label] ?: 0

    fun assertEveryStateVisited(atLeast: Int, walk: String) {
        val thin = StateLabel.entries.filter { this[it] < atLeast }
        if (thin.isEmpty()) return
        fail(
            "$walk does not exercise ${thin.joinToString(", ")} " +
                "(wanted >= $atLeast visits each)\n  observed: $this",
        )
    }

    override fun toString(): String =
        StateLabel.entries.joinToString(" ") { "${it.name}=${this[it]}" }
}

/**
 * The fake clock Law 3 asks for: no real time, no `Thread.sleep`, nothing
 * flaky by construction.
 *
 * The two clocks move independently on purpose, because on a real phone they do:
 * [monotonic] is `SystemClock.elapsedRealtime()` and cannot jump, [epochSeconds]
 * is the wall clock and can jump either way.
 */
class FakeClock(
    var monotonic: Long = 0L,
    var epochSeconds: Long = 1_786_970_096L,
    var offsetMinutes: Int = 180,
) : Clock {
    override fun monotonicMillis(): Long = monotonic
    override fun utcEpochSeconds(): Long = epochSeconds
    override fun utcOffsetMinutes(): Int = offsetMinutes

    /** Both clocks forward together — the ordinary case. */
    fun advance(millis: Long) {
        monotonic += millis
        epochSeconds += millis / 1000
    }
}

/**
 * The fake transport: it executes [Action]s the way the Android layer is
 * required to, and asserts the things that layer must never get wrong.
 *
 * Every `send` re-checks Law 2's invariant, so no individual test has to
 * remember to. From the emitted actions alone it tracks:
 *
 * - whether a GATT client is open, refusing a second open or a double close —
 *   the leak that eventually makes every connection fail with no visible cause;
 * - which timer is pending, so "no operation outstanding without a timeout" is
 *   checkable rather than asserted in prose;
 * - how many Time writes happened in one connection (PROTOCOL.md §4 allows one);
 * - whether the find-phone alarm is on, and in which mode: refusing a second
 *   start, a stop with nothing on, or a mode change with nothing to change, and
 *   holding it to exactly the ringing states — an alarm that outlives its link
 *   is the §4.1 failure nobody wants to hear.
 */
class FakeTransport(
    val machine: ConnectionStateMachine,
    /** Optional tally of the states this transport drove the machine through. */
    private val coverage: StateCoverage? = null,
) {

    val emitted = mutableListOf<Action>()
    val reports = mutableListOf<Action.ReportExchange>()
    val persisted = mutableListOf<HealthSnapshot>()

    var linkOpen: Boolean = false
        private set

    /** The find-phone alarm, as the Android layer would hold it: on or off. */
    var alarmOn: Boolean = false
        private set

    /** Whether the alarm is playing the tone as well as vibrating (§4.1). */
    var alarmSound: Boolean = false
        private set

    var findWritesThisConnection: Int = 0
        private set

    var lastFindPayload: ByteArray? = null
        private set

    var pendingTimerToken: Long? = null
        private set

    var pendingTimerDelayMillis: Long? = null
        private set

    /**
     * Which timer is pending. Modelled because on a real phone the two are
     * different objects — a posted callback and a Doze-proof alarm — so a
     * `RetryTimerFired` can never be the delivery of an operation timeout, even
     * if the numbers happened to line up.
     */
    private var pendingTimerIsRetry: Boolean = false

    var timeWritesThisConnection: Int = 0
        private set

    var lastTimePayload: ByteArray? = null
        private set

    private val history = mutableListOf<String>()

    fun send(event: TransportEvent): List<Action> {
        history += event.toString()

        // A timer that fires is no longer pending. Recording that before the
        // machine runs is what lets the invariant below tell "waiting for
        // something" from "nothing will ever happen again".
        val fired: Pair<Long, Boolean>? = when (event) {
            is TransportEvent.OperationTimedOut -> event.token to false
            is TransportEvent.RetryTimerFired -> event.token to true
            else -> null
        }
        if (fired != null && fired.first == pendingTimerToken && fired.second == pendingTimerIsRetry) {
            clearPendingTimer()
        }

        val actions = machine.onEvent(event)
        actions.forEach(::perform)
        emitted += actions
        assertInvariant()
        return actions
    }

    fun send(vararg events: TransportEvent) {
        events.forEach { send(it) }
    }

    /** Deliver the pending operation timeout. */
    fun fireOperationTimeout(): List<Action> {
        val token = assertNotNull(pendingTimerToken, "no timer pending")
        expect(!pendingTimerIsRetry) { "the pending timer is a retry alarm, not an operation timeout" }
        return send(TransportEvent.OperationTimedOut(token))
    }

    /** Deliver the pending backoff alarm. */
    fun fireRetryTimer(): List<Action> {
        val token = assertNotNull(pendingTimerToken, "no timer pending")
        expect(pendingTimerIsRetry) { "the pending timer is an operation timeout, not a retry alarm" }
        return send(TransportEvent.RetryTimerFired(token))
    }

    private fun clearPendingTimer() {
        pendingTimerToken = null
        pendingTimerDelayMillis = null
        pendingTimerIsRetry = false
    }

    private fun perform(action: Action) {
        when (action) {
            Action.ArmAutoConnect -> {
                expect(!linkOpen) { "connectGatt() while a GATT client is still open" }
                linkOpen = true
                timeWritesThisConnection = 0
                findWritesThisConnection = 0
            }

            Action.CloseConnection -> {
                expect(linkOpen) { "close() with no open GATT client" }
                linkOpen = false
            }

            Action.DiscoverServices, Action.EnableStatusNotifications, Action.EnableFindNotifications ->
                expect(linkOpen) { "GATT operation issued with no open client" }

            is Action.WriteTime -> {
                expect(linkOpen) { "Time write issued with no open client" }
                timeWritesThisConnection++
                lastTimePayload = action.payload
                expect(timeWritesThisConnection == 1) {
                    "PROTOCOL.md section 4 allows one Time push per connection, saw $timeWritesThisConnection"
                }
            }

            is Action.WriteFindDismiss -> {
                expect(linkOpen) { "Find write issued with no open client" }
                findWritesThisConnection++
                lastFindPayload = action.payload
                expect(findWritesThisConnection == 1) {
                    "PROTOCOL.md section 4.1 writes Find once per link, saw $findWritesThisConnection"
                }
            }

            is Action.StartFindAlarm -> {
                expect(!alarmOn) { "the find-phone alarm was started while already sounding" }
                expect(linkOpen) { "the find-phone alarm was started with no link to the watch" }
                alarmOn = true
                alarmSound = action.sound
            }

            is Action.SetFindAlarmSound -> {
                expect(alarmOn) { "the find-phone alarm's mode was changed while it was not sounding" }
                alarmSound = action.sound
            }

            Action.StopFindAlarm -> {
                expect(alarmOn) { "the find-phone alarm was stopped while not sounding" }
                alarmOn = false
                alarmSound = false
            }

            is Action.ArmOperationTimeout -> {
                pendingTimerToken = action.token
                pendingTimerDelayMillis = action.timeoutMillis
                pendingTimerIsRetry = false
            }

            is Action.ScheduleRetry -> {
                pendingTimerToken = action.token
                pendingTimerDelayMillis = action.delayMillis
                pendingTimerIsRetry = true
            }

            Action.CancelTimers -> clearPendingTimer()

            is Action.PersistHealth -> persisted += action.snapshot
            is Action.ReportExchange -> reports += action
        }
    }

    /**
     * Law 2, re-checked after every single event: the app is connected and
     * working, or it is waiting for something that is guaranteed to arrive.
     * There is no third option and no terminal error state.
     */
    private fun assertInvariant() {
        coverage?.record(machine.state)
        // PROTOCOL.md §4.1: the alarm sounds exactly while the find link is up —
        // Ringing, and the subscription round trip before it unless the user has
        // already tapped Stop. Anywhere else, an alarm still going is an alarm that
        // outlived its link.
        val alarmDue = when (val state = machine.state) {
            ConnectionState.Ringing -> true
            is ConnectionState.SubscribingFind -> !state.dismissRequested
            else -> false
        }
        expect(alarmOn == alarmDue) {
            if (alarmOn) "the find-phone alarm is sounding outside a ringing state" else "ringing with no alarm sounding"
        }
        when (val state = machine.state) {
            ConnectionState.Idle -> {
                expect(!linkOpen) { "idle but holding a GATT client" }
                expect(pendingTimerToken == null) { "idle with a timer pending" }
            }

            ConnectionState.Armed -> {
                // The resting state: an autoConnect is pending with the
                // controller. That is precisely "armed and waiting".
                expect(linkOpen) { "armed with no pending autoConnect" }
                expect(pendingTimerToken == null) { "the resting state should hold no timer" }
            }

            is ConnectionState.WaitingForRetry -> {
                expect(!linkOpen) { "backing off while still holding a GATT client" }
                expect(pendingTimerToken != null) { "backing off with no retry scheduled" }
                expect(pendingTimerDelayMillis == state.delayMillis) {
                    "retry delay mismatch: state says ${state.delayMillis}, timer says $pendingTimerDelayMillis"
                }
            }

            is ConnectionState.Settling -> {
                // The whole point of the state: closed, and NOT armed, until the
                // watch's window is over. A client still open here would be the
                // second-exchange-in-a-spent-window bug under another name.
                expect(!linkOpen) { "settling while still holding a GATT client" }
                expect(pendingTimerToken != null) { "settling with nothing to wake the app up" }
                expect(pendingTimerDelayMillis == state.delayMillis) {
                    "settle delay mismatch: state says ${state.delayMillis}, timer says $pendingTimerDelayMillis"
                }
            }

            is ConnectionState.Blocked -> {
                expect(!linkOpen) { "blocked while still holding a GATT client" }
                expect(pendingTimerToken == null) { "blocked with a timer pending" }
                expect(state.adapterOff || state.permissionMissing) { "blocked for no stated reason" }
            }

            ConnectionState.Discovering,
            ConnectionState.EnablingNotifications,
            ConnectionState.WritingTime,
            ConnectionState.AwaitingStatus,
            -> {
                expect(linkOpen) { "mid-exchange with no GATT client" }
                expect(pendingTimerToken != null) { "GATT operation outstanding with no timeout armed" }
            }

            is ConnectionState.SubscribingFind -> {
                // One GATT operation outstanding — the Find CCCD write — under the
                // ordinary per-operation timeout, on a link held open on purpose.
                expect(linkOpen) { "subscribing to Find with no link to the watch" }
                expect(pendingTimerToken != null && !pendingTimerIsRetry) { "Find subscription outstanding with no timeout armed" }
                expect(pendingTimerDelayMillis == com.workaday.core.protocol.WatchProtocol.OPERATION_TIMEOUT_MS) {
                    "Find subscription timeout is $pendingTimerDelayMillis, not the section 5.2 per-operation value"
                }
            }

            ConnectionState.Ringing -> {
                // The link is held open on purpose, and the §5.2 ring backstop is
                // what bounds it: a ring with no timer is an alarm that only the
                // watch can end, and the watch may be out of range.
                expect(linkOpen) { "ringing with no link to the watch" }
                expect(pendingTimerToken != null && !pendingTimerIsRetry) { "ringing with no backstop armed" }
                expect(pendingTimerDelayMillis == com.workaday.core.protocol.WatchProtocol.FIND_RING_BACKSTOP_MS) {
                    "ring backstop is $pendingTimerDelayMillis, not the section 5.2 value"
                }
            }

            ConnectionState.DismissingFind -> {
                expect(linkOpen) { "dismissing with no link to write on" }
                expect(pendingTimerToken != null && !pendingTimerIsRetry) { "Find write outstanding with no timeout armed" }
            }
        }
    }

    /**
     * Lazy on purpose. These run on the order of a hundred thousand times across
     * the exhaustive and random walks, and eagerly building a failure message
     * each time would turn a fast test into a slow one.
     */
    private fun expect(condition: Boolean, message: () -> String) {
        if (condition) return
        fail("${message()}\n  state: ${machine.state}\n  after: ${history.joinToString(" -> ")}")
    }
}
