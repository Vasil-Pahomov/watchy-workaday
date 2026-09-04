package com.workaday.core

import com.workaday.core.protocol.StatusDecode
import com.workaday.core.protocol.WatchProtocol
import com.workaday.core.protocol.WatchStatus

/**
 * `BluetoothGatt.GATT_SUCCESS`. Every other status is a failure, and they are
 * all handled the same way — including the notorious 133 that PROTOCOL.md §6.2
 * calls out by name. Treating "not zero" as one case is not laziness: §6.2 gives
 * 133 and every other GATT error the identical remedy (`close()`, one backoff
 * step, re-arm), and a table of Android error codes in `core/` would be a second
 * place for the platform's numbering to be got wrong.
 */
const val GATT_SUCCESS: Int = 0

/**
 * Not a status from the peer: the local stack never got the operation onto the
 * air.
 *
 * Android's GATT calls can fail *synchronously* — `discoverServices()` returns
 * false, the characteristic is absent from the discovered profile, the write API
 * refuses the request. No callback will ever arrive for such an operation, so the
 * Android layer reports the failure with this status rather than leaving a dead
 * operation to burn PROTOCOL.md §5.2's five-second timeout first. The machine
 * treats it exactly like any other non-zero status, which is the point.
 *
 * Negative so it cannot collide with a real GATT status or a `BluetoothStatusCodes`
 * value, both of which are non-negative and both of which the Android layer
 * forwards verbatim when it has one.
 */
const val GATT_LOCAL_FAILURE: Int = -1

/**
 * Which of the two timers is pending. They are separate objects in the Android
 * layer — [Action.ArmOperationTimeout] posts one, [Action.ScheduleRetry] the
 * other — so a firing of the wrong kind is never the one being waited on.
 */
private enum class TimerKind { None, Operation, Retry }

/**
 * Where the app is. Exactly one of these is true at any moment, and — Law 2 —
 * **none of them is a terminal error state.**
 *
 * Every value here is either "connected and working, with a timer armed" or
 * "waiting, with a defined event that resumes". `ConnectionStateMachineTest`
 * walks every reachable sequence of events and asserts precisely that.
 */
sealed interface ConnectionState {

    /** Before [TransportEvent.Started]. The only state that is not yet armed. */
    data object Idle : ConnectionState

    /**
     * The resting state: `connectGatt(autoConnect = true)` is pending and the
     * Bluetooth controller is holding the wait. Costs the app nothing, survives
     * Doze, and fires when the watch opens its window.
     */
    data object Armed : ConnectionState

    /** A §5.2 backoff delay is running; [TransportEvent.RetryTimerFired] re-arms. */
    data class WaitingForRetry(val delayMillis: Long) : ConnectionState

    /**
     * The exchange succeeded, the link is closed, and the app is deliberately
     * **not acting on the watch** until the window that produced it is over.
     *
     * The rule it enforces, in one line: **a successful exchange spends its
     * window, and no new exchange may begin inside a window that has already been
     * spent.** PROTOCOL.md §1 is the ground truth for the shape - the watch appears
     * in short windows and one exchange per window is the intent - and §4 says the
     * phone pushes the time once per connection, on every connection.
     *
     * **What went wrong without it, from the phone and the watch logs of the same
     * window.** The exchange completed at 41.499 and the machine went to [Armed],
     * which is correct and is Law 1's resting state. But `autoConnect` does not
     * mean "wait for the next window"; it means "connect as soon as the peer is
     * connectable", and the watch's window ran to 42.257. A fresh connection
     * arrived **16 ms** after the success was reported, the machine correctly
     * began a second exchange - a peripheral had appeared - and the watch shut its
     * window half way through it. The phone then sat in [AwaitingStatus] until the
     * link dropped at 47.422 and recorded `DisconnectedMidExchange`: a 36-second
     * backoff step and a health increment, **after a sync that had just
     * succeeded.** Every successful sync was followed by a spurious failed one.
     *
     * Note where the fault was not. The machine's logic was right at every step,
     * `CloseConnection` did what it promised, and nothing was stale. What was wrong
     * was that "armed" and "ready to start an exchange" had been treated as the
     * same thing, and against a peer that is still advertising they are not.
     *
     * **Why a bounded wait rather than waiting for the link to go away.** After
     * `close()` there is no GATT client, so there are no callbacks - there is no
     * "the peer went away and stayed away" event to wait for, and a state that
     * waited for one would wait forever. So the machine waits out the window by
     * the clock instead.
     *
     * [delayMillis] is what is left of §5.2's 15 s whole-exchange budget, measured
     * from connect. That number is already chosen against exactly this boundary,
     * though it took hardware to notice: §5.2 gives it as "longer than the watch's
     * 12 s cap, so the watch's own teardown is the normal ending". The watch's
     * window opened at or before our connect, so connect plus 15 s is always past
     * its §5.1 absolute cap. No new constant is invented and no contract changes;
     * `WatchProtocolTest` pins the relationship so the two numbers cannot cross
     * silently.
     *
     * The failure path never needed this: it goes through a §5.2 backoff of at
     * least 30 s, which already outlasts the watch's 12 s cap. Success was the only
     * path that re-armed with no delay at all - which is why the defect appeared
     * only after a sync that worked.
     *
     * A waiting state with a defined event that resumes it, so Law 2 holds: the
     * timer fires and the machine arms. The app still ends up armed, always -
     * that is Law 1 and it is not negotiable. The only question this state answers
     * is whether to *act* on a peripheral it has just finished with.
     */
    data class Settling(val delayMillis: Long) : ConnectionState

    /**
     * The radio cannot be used: adapter off (or airplane mode, which looks the
     * same), or the runtime Bluetooth permission was revoked.
     *
     * Still "waiting", not "failed" — §6.2's last two rows. Both flags are
     * tracked separately so that turning the adapter back on while the
     * permission is still missing does not arm into a guaranteed failure.
     */
    data class Blocked(val adapterOff: Boolean, val permissionMissing: Boolean) : ConnectionState

    /** Connected; service discovery outstanding. §4 op 1. */
    data object Discovering : ConnectionState

    /** Connected; the CCCD write on Status is outstanding. §4 op 2. */
    data object EnablingNotifications : ConnectionState

    /** Connected; the Time write is outstanding. §4 op 3. */
    data object WritingTime : ConnectionState

    /** Connected; waiting for the Status notify that ends the exchange. */
    data object AwaitingStatus : ConnectionState

    /**
     * Connected, the §4 exchange finished, its Status carried `FIND_PHONE`
     * (PROTOCOL.md §4.1), the alarm is on, and the CCCD write that subscribes the
     * phone to `Find` — the channel the watch changes the alarm mode on — is
     * outstanding with a §5.2 per-operation timeout.
     *
     * Lasts one GATT round trip. Completes, fails or times out into [Ringing];
     * an older watch with no notify on `Find` simply leaves the phone in the mode
     * the Status asked for. [dismissRequested] is the user's Stop landing inside
     * that round trip: the alarm is already off, and the §3.3 dismiss write waits
     * for the CCCD callback rather than being issued on top of it — Android
     * silently drops a second outstanding operation (Law 2), and the callback is
     * at most five seconds away.
     */
    data class SubscribingFind(val dismissRequested: Boolean = false) : ConnectionState

    /**
     * Connected, the §4 exchange finished, and its Status carried `FIND_PHONE`
     * (PROTOCOL.md §4.1): the link is held **open** and the phone's alarm is
     * on — vibration, and the tone if the watch asked for it. A
     * [Action.ArmOperationTimeout] for the §5.2 ring backstop is armed, and a
     * `FindMode` notify from the watch changes the mode in place.
     *
     * The one state in which a finished exchange does not end in `close()`, and
     * deliberately so: the alarm sounds exactly while the find link is up, and the
     * watch ends it by hanging up. Left by [TransportEvent.Disconnected] (the
     * normal ending), by the backstop, by the user dismissing it on the phone
     * ([DismissingFind]), or by the radio going away ([Blocked]). Every exit stops
     * the alarm; none of them backs off or touches health — the exchange itself
     * was recorded when the Status arrived, as any other would be.
     */
    data object Ringing : ConnectionState

    /**
     * The user silenced the alarm on the phone. The alarm is already off, the §3.3
     * FindDismiss write is outstanding with a §5.2 per-operation timeout, and the
     * link closes — and re-arms at once — when the write completes, fails or times
     * out. §4.1: the watch has processed the write before its response leaves, so
     * closing on the reply never cuts the message short.
     */
    data object DismissingFind : ConnectionState
}

/**
 * What the Android layer observed. These mirror the GATT callbacks and the
 * system broadcasts one for one, deliberately carrying the raw status codes:
 * the Android layer forwards, it does not decide (Law 3).
 */
sealed interface TransportEvent {

    /** The foreground service came up. Idempotent. */
    data object Started : TransportEvent

    /** `onConnectionStateChange` → `STATE_CONNECTED`. */
    data class Connected(val gattStatus: Int) : TransportEvent

    /**
     * `onConnectionStateChange` → `STATE_DISCONNECTED`.
     *
     * [gattStatus] is carried so APP-3 can log it, but nothing in here branches
     * on it. §6.2's three disconnect rows differ by *when* the disconnect
     * happened — after a successful notify, mid-exchange, or while merely armed
     * — which is the state machine's own state, not the peer's error code.
     */
    data class Disconnected(val gattStatus: Int) : TransportEvent

    /** `onServicesDiscovered`. */
    data class ServicesDiscovered(val gattStatus: Int) : TransportEvent

    /** `onDescriptorWrite` for the CCCD on Status. */
    data class DescriptorWritten(val gattStatus: Int) : TransportEvent

    /** `onCharacteristicWrite` for Time. */
    data class CharacteristicWritten(val gattStatus: Int) : TransportEvent

    /**
     * `onCharacteristicChanged` for Status. Nullable payload because Android's
     * value getter is platform-typed; a null payload is a wrong length, not a
     * crash.
     *
     * A plain class, not a `data class`: `ByteArray` has identity equality, so a
     * generated `equals` would be quietly wrong for anyone who tried to use it.
     */
    class NotificationReceived(val payload: ByteArray?) : TransportEvent {
        override fun toString(): String =
            "NotificationReceived(${payload?.size ?: "null"} bytes)"
    }

    /**
     * The timer armed by [Action.ArmOperationTimeout] expired. [token] is
     * checked against the machine's current one, so a firing that raced a
     * cancellation is discarded instead of tearing down a healthy connection.
     */
    data class OperationTimedOut(val token: Long) : TransportEvent

    /** The timer armed by [Action.ScheduleRetry] expired. [token] as above. */
    data class RetryTimerFired(val token: Long) : TransportEvent

    /** Adapter turned off, or airplane mode engaged — they are indistinguishable. */
    data object AdapterOff : TransportEvent

    /** Adapter available again. */
    data object AdapterOn : TransportEvent

    /** `BLUETOOTH_CONNECT` revoked at runtime. */
    data object PermissionRevoked : TransportEvent

    /** `BLUETOOTH_CONNECT` granted again. */
    data object PermissionGranted : TransportEvent

    /**
     * The user silenced the find-phone alarm on the phone — the Stop button on the
     * alarm screen or on its notification. Not a radio event: it comes from the
     * UI, which is why it is the one event the Android layer feeds from an
     * `onStartCommand` rather than from a GATT callback. Ignored outside
     * [ConnectionState.Ringing]; there is nothing to dismiss.
     */
    data object FindDismissedOnPhone : TransportEvent
}

/**
 * What the Android layer must do, in the order given. It performs these; it
 * decides nothing (Law 3).
 */
sealed interface Action {

    /**
     * `device.connectGatt(context, autoConnect = true, callback)`.
     *
     * Never issued while a GATT client is already open — the machine closes
     * first — so this can never leak one of the process's limited client slots.
     */
    data object ArmAutoConnect : Action

    /**
     * `gatt.disconnect()` **then** `gatt.close()`, release the reference, and
     * deliver no further callbacks from that client to the machine.
     *
     * All three parts are load-bearing. `disconnect()` without `close()` leaks a
     * client slot (Law 2). And the machine reads a `Disconnected` arriving while
     * [ConnectionState.Armed] as "the pending autoConnect failed" and backs off
     * — which is right for a 133, and wrong if it is really the echo of a link
     * this action already closed. `close()` unregisters the client, so the
     * platform stops delivering; APP-3's job is not to route a stale `gatt`
     * object's callbacks back in after that.
     */
    data object CloseConnection : Action

    /** `gatt.discoverServices()`. */
    data object DiscoverServices : Action

    /**
     * `setCharacteristicNotification` on Status plus a CCCD write of
     * [WatchProtocol.cccdEnableNotificationValue].
     *
     * §4: this happens **before** the Time write. Writing first races the
     * notification and loses it.
     */
    data object EnableStatusNotifications : Action

    /**
     * Write [payload] to [WatchProtocol.TIME_CHARACTERISTIC_UUID], **with
     * response**. The bytes are already encoded — the Android layer does not
     * touch the wire format.
     */
    class WriteTime(val payload: ByteArray) : Action {
        override fun toString(): String = "WriteTime(${payload.size} bytes)"
    }

    /**
     * Write [payload] to [WatchProtocol.FIND_CHARACTERISTIC_UUID], **with
     * response** — the §3.3 FindDismiss frame, already encoded. Issued only from
     * [ConnectionState.Ringing], once per link, and answered by
     * [TransportEvent.CharacteristicWritten] like any other write.
     */
    class WriteFindDismiss(val payload: ByteArray) : Action {
        override fun toString(): String = "WriteFindDismiss(${payload.size} bytes)"
    }

    /**
     * Make the phone felt: vibration and the full-screen alarm UI, plus the tone
     * on the alarm channel when [sound] is set (PROTOCOL.md §4.1). Idempotent for
     * the Android layer — the machine issues it once per find link and always
     * pairs it with a [StopFindAlarm] before the link is left.
     */
    data class StartFindAlarm(val sound: Boolean) : Action

    /**
     * Change the mode of an alarm that is already on: add the tone or take it
     * away, vibration continuing throughout (§3.3 FindMode). Only ever emitted
     * between a [StartFindAlarm] and its [StopFindAlarm].
     */
    data class SetFindAlarmSound(val sound: Boolean) : Action

    /**
     * Silence it and take the alarm UI down. Emitted on **every** exit from
     * [ConnectionState.Ringing] and [ConnectionState.SubscribingFind] — the watch
     * hanging up, the backstop, the user's dismissal, the radio going away — so
     * there is no path on which the alarm outlives the link.
     */
    data object StopFindAlarm : Action

    /**
     * `setCharacteristicNotification` on Find plus a CCCD write of
     * [WatchProtocol.cccdEnableNotificationValue] — the phone's subscription to
     * the watch's `FindMode` notifies (§4.1). Issued once per find link, right
     * after the flagged Status, with a §5.2 per-operation timeout.
     */
    data object EnableFindNotifications : Action

    /**
     * Post a single delayed callback that will deliver
     * [TransportEvent.OperationTimedOut] with this [token], replacing any timer
     * already pending. Exactly one timer is ever outstanding.
     */
    data class ArmOperationTimeout(val timeoutMillis: Long, val token: Long) : Action

    /**
     * As [ArmOperationTimeout], but delivering [TransportEvent.RetryTimerFired].
     * The delay already includes §5.2's jitter.
     *
     * This has to survive Doze — an `AlarmManager` alarm or a `WorkManager`
     * one-shot, not a `Handler.postDelayed` that a dozing phone will stretch
     * past the watch's next window.
     */
    data class ScheduleRetry(val delayMillis: Long, val token: Long) : Action

    /** Cancel whatever timer is pending. Idempotent. */
    data object CancelTimers : Action

    /** Write [snapshot] somewhere that survives process death. Law 2. */
    data class PersistHealth(val snapshot: HealthSnapshot) : Action

    /**
     * Surface a finished exchange in the diagnostic UI. [status] is present only
     * when the watch actually answered — §6.2 requires the `result` code to be
     * visible, `BadVersion` most of all.
     *
     * [atUtcEpochSeconds] rides along rather than being read by the Android layer
     * when it performs this, so that the timestamp on the persisted
     * [ExchangeReport] is the same clock read that went into [HealthSnapshot] —
     * one read, one instant, no chance of the screen and the counters disagreeing
     * about when the same exchange happened.
     */
    data class ReportExchange(
        val outcome: ExchangeOutcome,
        val status: WatchStatus?,
        val atUtcEpochSeconds: Long,
    ) : Action
}

/**
 * Every decision the app makes about the link, in one place a JVM test can drive.
 *
 * It consumes [TransportEvent]s and returns the [Action]s the Android layer must
 * perform, in order. There is no `Context`, no `BluetoothGatt`, no thread and no
 * wall-clock read in here — time arrives through [Clock] (Law 3).
 *
 * The two rules it exists to enforce:
 *
 * - **§4's ordering.** Discover, then enable notifications, then write Time,
 *   then wait for the notify. One outstanding GATT operation at a time, each
 *   with its own timeout, because Android silently drops a write issued before
 *   the previous callback returns.
 * - **Law 2's invariant.** Every path ends armed and waiting. After any event
 *   the machine is connected with a timer armed, resting on a pending
 *   autoConnect, counting down a backoff, or blocked on a condition with a
 *   defined event that clears it. There is no fifth option and no terminal
 *   error state.
 *
 * Not thread-safe, deliberately: the Android layer owns it from a single
 * serialising context, which is also what makes "one outstanding operation"
 * true rather than hoped for.
 */
class ConnectionStateMachine(
    private val clock: Clock,
    private val backoff: Backoff,
    initialHealth: HealthSnapshot = HealthSnapshot(),
) {
    private val healthCounter = Health(initialHealth)

    private var started = false
    private var adapterOn = true
    private var permissionGranted = true

    /** True between an emitted [Action.ArmAutoConnect] and its [Action.CloseConnection]. */
    private var linkOpen = false

    /**
     * Which timer is pending, if any — never more than one.
     *
     * The kind is checked alongside the token, and it is **defence in depth, not
     * the load-bearing guard**: in the shape this file settled into, each
     * handler already refuses to act outside the states its timer can exist in
     * ([onRetryTimerFired] returns unless [ConnectionState.WaitingForRetry],
     * [onOperationTimedOut] routes every other state to a no-op), and those
     * guards subsume it. Deleting the kind check today changes no observable
     * behaviour, and the tests say so.
     *
     * It is kept because the guards it duplicates are easy to relax by accident.
     * The two timers really are different objects in the Android layer — a
     * posted callback and a Doze-proof alarm — so a firing of the wrong kind is
     * never the one being waited on, and saying that once here is cheaper than
     * re-deriving it at each handler the next time one is edited.
     */
    private var armedTimer = TimerKind.None
    private var timerToken = 0L

    /** [Clock.monotonicMillis] at the moment the link came up. §5.2's 15 s runs from here. */
    private var connectedAtMillis = 0L

    var state: ConnectionState = ConnectionState.Idle
        private set

    /** The last well-formed Status frame the watch sent, for the diagnostic screen. */
    var lastStatus: WatchStatus? = null
        private set

    val health: HealthSnapshot get() = healthCounter.snapshot

    /**
     * [health] read as a traffic light. Failing means the backoff is at its cap.
     *
     * The snapshot overload, not the two-number one: a machine that has never
     * completed an exchange has zero consecutive failures, and answering
     * [HealthLevel.Healthy] to that would be the same "the last exchange
     * succeeded" claim that a fresh install has no business making.
     */
    val healthLevel: HealthLevel
        get() = healthLevelFor(health, backoff.attemptsToReachCap + 1)

    /**
     * Feed one observation, get back the work to do.
     *
     * Returns an empty list for anything that does not apply in the current
     * state — a callback from a link that has already been closed, a timer
     * firing that lost a race with its own cancellation, a duplicate `Started`.
     * Ignoring those is not sloppiness: the alternative is acting on a stale
     * link, which is how a GATT client gets leaked.
     */
    fun onEvent(event: TransportEvent): List<Action> {
        val actions = mutableListOf<Action>()
        when (event) {
            TransportEvent.Started -> onStarted(actions)

            TransportEvent.AdapterOff -> {
                adapterOn = false
                enterBlockedIfNeeded(actions)
            }

            TransportEvent.AdapterOn -> {
                adapterOn = true
                leaveBlockedIfPossible(actions)
            }

            TransportEvent.PermissionRevoked -> {
                permissionGranted = false
                enterBlockedIfNeeded(actions)
            }

            TransportEvent.PermissionGranted -> {
                permissionGranted = true
                leaveBlockedIfPossible(actions)
            }

            is TransportEvent.Connected -> onConnected(event.gattStatus, actions)
            is TransportEvent.Disconnected -> onDisconnected(actions)
            is TransportEvent.ServicesDiscovered -> onServicesDiscovered(event.gattStatus, actions)
            is TransportEvent.DescriptorWritten -> onDescriptorWritten(event.gattStatus, actions)
            is TransportEvent.CharacteristicWritten -> onCharacteristicWritten(event.gattStatus, actions)
            is TransportEvent.NotificationReceived -> onNotification(event.payload, actions)
            is TransportEvent.OperationTimedOut -> onOperationTimedOut(event.token, actions)
            is TransportEvent.RetryTimerFired -> onRetryTimerFired(event.token, actions)
            TransportEvent.FindDismissedOnPhone -> onFindDismissedOnPhone(actions)
        }
        return actions
    }

    // ── Lifecycle and gating ─────────────────────────────────────────────────

    private fun onStarted(actions: MutableList<Action>) {
        if (started) return
        started = true
        if (isBlocked()) {
            state = blockedState()
        } else {
            arm(actions)
        }
    }

    private fun isBlocked(): Boolean = !adapterOn || !permissionGranted

    private fun blockedState(): ConnectionState.Blocked =
        ConnectionState.Blocked(adapterOff = !adapterOn, permissionMissing = !permissionGranted)

    /**
     * The radio became unusable. Tear down whatever was in flight and wait.
     *
     * Deliberately **no** backoff step and no health increment: §6.2 lists these
     * as handled transitions, separate from the failure rows. The adapter being
     * off is not the watch failing to answer, and penalising it would delay the
     * first attempt after the user leaves airplane mode — exactly when the app
     * should be quickest.
     */
    private fun enterBlockedIfNeeded(actions: MutableList<Action>) {
        if (!started || !isBlocked()) return
        cancelTimer(actions)
        // The alarm sounds exactly while the find link is up (§4.1), and the link
        // is about to be closed under it. Every exit from a ringing state silences it.
        if (alarmOn()) actions += Action.StopFindAlarm
        closeLink(actions)
        state = blockedState()
    }

    /** Whether the find-phone alarm is on: Ringing, or subscribing with no Stop tapped yet. */
    private fun alarmOn(): Boolean = when (val current = state) {
        ConnectionState.Ringing -> true
        is ConnectionState.SubscribingFind -> !current.dismissRequested
        else -> false
    }

    /** Whether the user's Stop has already landed on the current Find subscription. */
    private fun findDismissRequested(): Boolean =
        (state as? ConnectionState.SubscribingFind)?.dismissRequested == true

    private fun leaveBlockedIfPossible(actions: MutableList<Action>) {
        if (!started) return
        if (state !is ConnectionState.Blocked) return
        if (isBlocked()) {
            // One condition cleared, the other has not. Refresh the reasons so
            // the diagnostic screen stops naming the one that is fixed.
            state = blockedState()
            return
        }
        // Straight back to armed, skipping any backoff that was running when the
        // radio went away: the outage already supplied the delay, and the watch's
        // next window is what we are trying not to miss. The failure count is
        // untouched, so a link that fails again resumes where it left off.
        arm(actions)
    }

    // ── The exchange, in §4's order ──────────────────────────────────────────

    private fun onConnected(gattStatus: Int, actions: MutableList<Action>) {
        if (state != ConnectionState.Armed) return
        if (gattStatus != GATT_SUCCESS) {
            failExchange(ExchangeOutcome.ConnectionAttemptFailed, actions)
            return
        }
        connectedAtMillis = clock.monotonicMillis()
        state = ConnectionState.Discovering
        // Timer first, then the operation: there is then no instant in which a
        // GATT call is outstanding without something watching it.
        armOperationTimeout(actions)
        actions += Action.DiscoverServices
    }

    private fun onServicesDiscovered(gattStatus: Int, actions: MutableList<Action>) {
        if (state != ConnectionState.Discovering) return
        if (gattStatus != GATT_SUCCESS) {
            failExchange(ExchangeOutcome.GattOperationFailed, actions)
            return
        }
        state = ConnectionState.EnablingNotifications
        armOperationTimeout(actions)
        actions += Action.EnableStatusNotifications
    }

    private fun onDescriptorWritten(gattStatus: Int, actions: MutableList<Action>) {
        val current = state
        if (current is ConnectionState.SubscribingFind) {
            // The Find subscription is done, one way or another (§4.1). A failure —
            // an older watch with no notify on Find, a stack that refused — costs
            // only the mid-link mode change; the alarm is already on in the mode
            // the Status asked for, so it rings on. Not a failed exchange.
            afterFindSubscription(current, actions)
            return
        }
        if (current != ConnectionState.EnablingNotifications) return
        if (gattStatus != GATT_SUCCESS) {
            failExchange(ExchangeOutcome.GattOperationFailed, actions)
            return
        }
        // The clock is read here, as late as possible: this is the last moment
        // before the bytes go on the wire, so the watch gets the freshest time
        // the phone has.
        val epochSeconds = clock.utcEpochSeconds()
        val offsetMinutes = clock.utcOffsetMinutes()
        if (!WatchProtocol.isEncodableTime(epochSeconds, offsetMinutes)) {
            // The phone's own clock is outside what §3.1 can carry. Sending a
            // truncated epoch would set the watch to a wrong time and report
            // success, which is worse than not syncing.
            failExchange(ExchangeOutcome.LocalClockUnusable, actions)
            return
        }
        state = ConnectionState.WritingTime
        armOperationTimeout(actions)
        actions += Action.WriteTime(WatchProtocol.encodeTime(epochSeconds, offsetMinutes))
    }

    private fun onCharacteristicWritten(gattStatus: Int, actions: MutableList<Action>) {
        if (state == ConnectionState.DismissingFind) {
            // The §3.3 write is done — delivered, refused, or never issued at all
            // (an older watch with no Find characteristic reports GATT_LOCAL_FAILURE
            // here). §4.1: the alarm was already stopped by the user's tap, the
            // watch has processed a delivered frame before its response left, and
            // there is nothing else to say on this link. Close and re-arm; the
            // status is not a failed exchange, because the exchange finished when
            // the Status arrived.
            finishDismiss(actions)
            return
        }
        if (state != ConnectionState.WritingTime) return
        if (gattStatus != GATT_SUCCESS) {
            failExchange(ExchangeOutcome.GattOperationFailed, actions)
            return
        }
        state = ConnectionState.AwaitingStatus
        // Waiting for the notify is not one of §5.2's per-operation timeouts, so
        // it gets whatever is left of the 15 s whole-exchange budget.
        armTimer(remainingExchangeBudgetMillis(), actions)
    }

    private fun onNotification(payload: ByteArray?, actions: MutableList<Action>) {
        // On a find link the only notify the watch sends is a FindMode on Find
        // (§3.3): §4 forbids an unprompted Status, and the phone wrote Time once.
        // Anything that does not decode as a mode — a stray 12-byte frame, a
        // truncated one — is ignored, and the mode stays what it was. Accepted in
        // SubscribingFind as well as Ringing, because the watch answers the
        // subscription with the current mode and that notify can beat the CCCD
        // callback, exactly as a Status can beat its write callback below.
        if (alarmOn() || state is ConnectionState.SubscribingFind) {
            val mode = WatchProtocol.decodeFindMode(payload) ?: return
            // With the user's Stop already tapped the alarm is off, and a mode for
            // an alarm that is off is nothing to perform.
            if (alarmOn()) actions += Action.SetFindAlarmSound(mode.sound)
            return
        }

        // Accepted in WritingTime as well as AwaitingStatus. Android's write
        // callback and an incoming notification are separate deliveries and can
        // arrive in either order; if the notify wins the race, consuming it here
        // is the difference between a successful sync and a 15 s timeout
        // followed by a backoff step.
        //
        // Deliberately *not* accepted in Discovering or EnablingNotifications.
        // §3.2 says a Status read before this connection's write reports the
        // *previous* sync — so a notify that early could carry a stale `Ok` from
        // an hour ago, and treating it as this exchange's result would reset the
        // backoff for a sync that never happened.
        if (state != ConnectionState.WritingTime && state != ConnectionState.AwaitingStatus) return

        when (val decoded = WatchProtocol.decodeStatus(payload)) {
            is StatusDecode.Rejected -> failExchange(ExchangeOutcome.MalformedStatus, actions)

            is StatusDecode.Valid -> {
                val status = decoded.status
                lastStatus = status
                if (status.findPhoneRequested) {
                    // §4.1: the watch is looking for this phone. The exchange is
                    // recorded exactly as it would be without the flag — success
                    // or failure, health, backoff counter, diagnostic record — but
                    // the link is kept and the alarm starts, whatever `result`
                    // said. A clock that could not be set is no reason to leave the
                    // phone lost.
                    ring(status, actions)
                } else if (status.isSuccess) {
                    succeed(status, actions)
                } else {
                    // §6.2: a non-zero result is a **failed** exchange. Backoff
                    // is not reset, and there is no second push in this
                    // connection — §4 permits one only if the first failed, and
                    // §6.2 says a drifted `BadVersion` must not hot-loop. The
                    // watch's next window is the right place to try again.
                    failExchange(
                        ExchangeOutcome.WatchReportedFailure,
                        actions,
                        status = status,
                        statusResultCode = status.resultCode,
                    )
                }
            }
        }
    }

    // ── Failure paths ────────────────────────────────────────────────────────

    private fun onDisconnected(actions: MutableList<Action>) {
        when (state) {
            // §6.2 row "status 133": the pending autoConnect never came up, or
            // dropped before the exchange began. Close (the client may be stale
            // or leaked, which is what 133 usually means), back off one step,
            // re-arm with a fresh client.
            ConnectionState.Armed -> failExchange(ExchangeOutcome.ConnectionAttemptFailed, actions)

            // §6.2 row "disconnect mid-exchange".
            ConnectionState.Discovering,
            ConnectionState.EnablingNotifications,
            ConnectionState.WritingTime,
            ConnectionState.AwaitingStatus,
            -> failExchange(ExchangeOutcome.DisconnectedMidExchange, actions)

            // §4.1's normal ending: the watch hung up because its search is over —
            // Back pressed, or its cap — or the link was lost and it is still
            // looking. The phone cannot tell and does not need to: the alarm stops
            // with the link, and re-arming at once serves both. Not a fault, no
            // backoff, no health increment — the exchange was recorded when the
            // Status arrived.
            ConnectionState.Ringing -> endRinging(actions)

            // The same, one round trip earlier. With the user's Stop already
            // tapped the alarm is off and there is nothing left to silence.
            is ConnectionState.SubscribingFind ->
                if (findDismissRequested()) finishDismiss(actions) else endRinging(actions)

            // The watch hung up before the dismiss write completed — it processed
            // the frame and ended its search, or the link dropped. Either way the
            // alarm is already off and the link is gone; re-arm.
            ConnectionState.DismissingFind -> finishDismiss(actions)

            // §6.2 row "disconnect after a successful notify" lands here: the
            // exchange already ended, the link is already closed and re-armed,
            // and this is at most an echo. Normal, not a fault — no backoff, no
            // health increment, nothing logged as an error.
            ConnectionState.Idle,
            is ConnectionState.WaitingForRetry,
            is ConnectionState.Blocked,
            // Settling lands here too, and it is the same row: the exchange ended,
            // the link is already closed, and this is at most the echo of our own
            // hang-up. No backoff, no health increment, nothing logged as a fault.
            is ConnectionState.Settling,
            -> Unit
        }
    }

    private fun onOperationTimedOut(token: Long, actions: MutableList<Action>) {
        if (!isCurrentTimer(TimerKind.Operation, token)) return
        when (state) {
            ConnectionState.Discovering,
            ConnectionState.EnablingNotifications,
            ConnectionState.WritingTime,
            ConnectionState.AwaitingStatus,
            -> {
                // It fired, so there is nothing left to cancel.
                armedTimer = TimerKind.None
                // §6.2: close() — never just disconnect() — one backoff step, re-arm.
                failExchange(ExchangeOutcome.OperationTimedOut, actions)
            }

            // The settle after a successful exchange is over, so the watch's
            // window has closed and arming can no longer land back inside it.
            is ConnectionState.Settling -> {
                armedTimer = TimerKind.None
                arm(actions)
            }

            // §5.2's ring backstop: the watch's cap has passed and no disconnect
            // reached us. Stop the alarm and close — the net behind a hang-up that
            // never arrived, not the normal ending.
            ConnectionState.Ringing -> {
                armedTimer = TimerKind.None
                endRinging(actions)
            }

            // The Find subscription never completed inside its §5.2 timeout. The
            // alarm rings on in the mode it started in, under the backstop; a
            // dismiss that was waiting on the callback closes instead — the user
            // already silenced the phone, and a link whose last operation timed out
            // is not one to issue another write on.
            is ConnectionState.SubscribingFind -> {
                armedTimer = TimerKind.None
                if (findDismissRequested()) finishDismiss(actions) else settleIntoRinging(actions)
            }

            // The dismiss write never completed inside its §5.2 timeout. The alarm
            // is already off; close and re-arm.
            ConnectionState.DismissingFind -> {
                armedTimer = TimerKind.None
                finishDismiss(actions)
            }

            ConnectionState.Idle,
            ConnectionState.Armed,
            is ConnectionState.WaitingForRetry,
            is ConnectionState.Blocked,
            // An operation timer only exists in the states above, so this is
            // unreachable. Deliberately leaving the timer armed rather than
            // clearing it: disarming a timer we are not going to act on is
            // precisely how a state ends up waiting for nothing.
            -> Unit
        }
    }

    // ── Find phone, §4.1 ─────────────────────────────────────────────────────

    /**
     * The Status carried `FIND_PHONE`: record the exchange as any other, then
     * keep the link, make the phone felt, and subscribe to the mode channel.
     *
     * Recording comes first and is unconditional on the flag, so the counters and
     * the diagnostic record see exactly what a plain sync would have produced —
     * a find session that also set the clock is a success, one whose Time write
     * the watch refused is a failure, and neither is changed by the ringing that
     * follows. What the flag changes is only what happens to the link.
     *
     * The alarm starts before the subscription is issued, not after it completes:
     * the phone is lost, and a round trip is a round trip of silence. Its mode is
     * the Status's `FIND_SOUND` — the wearer may have pressed Menu before the
     * phone connected — and a `FindMode` notify changes it from then on.
     */
    private fun ring(status: WatchStatus, actions: MutableList<Action>) {
        cancelTimer(actions)
        val now = clock.utcEpochSeconds()
        if (status.isSuccess) {
            healthCounter.recordSuccess(now, status.resultCode)
            actions += Action.PersistHealth(health)
            actions += Action.ReportExchange(ExchangeOutcome.Succeeded, status, now)
        } else {
            healthCounter.recordFailure(now, ExchangeOutcome.WatchReportedFailure, status.resultCode)
            actions += Action.PersistHealth(health)
            actions += Action.ReportExchange(ExchangeOutcome.WatchReportedFailure, status, now)
        }
        actions += Action.StartFindAlarm(sound = status.findSoundRequested)
        // One GATT operation, one per-operation timeout — the plain one, because
        // the §5.2 exchange budget is spent and this is not an exchange.
        armTimer(WatchProtocol.OPERATION_TIMEOUT_MS, actions)
        actions += Action.EnableFindNotifications
        state = ConnectionState.SubscribingFind()
    }

    /**
     * The Find subscription's callback arrived. Either into the ring proper, or —
     * if the user tapped Stop while it was outstanding — straight into the dismiss
     * write that was waiting its turn on the link.
     */
    private fun afterFindSubscription(
        subscribing: ConnectionState.SubscribingFind,
        actions: MutableList<Action>,
    ) {
        if (subscribing.dismissRequested) {
            cancelTimer(actions)
            writeDismiss(actions)
        } else {
            settleIntoRinging(actions)
        }
    }

    /** Ringing, with the §5.2 backstop as the only timer on the link. */
    private fun settleIntoRinging(actions: MutableList<Action>) {
        cancelTimer(actions)
        // The backstop, not a per-operation timeout: nothing is outstanding on the
        // link, and this is the whole of how long the phone will ring unasked.
        armTimer(WatchProtocol.FIND_RING_BACKSTOP_MS, actions)
        state = ConnectionState.Ringing
    }

    /** The §3.3 FindDismiss write, with its own timeout, and the state that waits on it. */
    private fun writeDismiss(actions: MutableList<Action>) {
        state = ConnectionState.DismissingFind
        // The plain per-operation timeout, not armOperationTimeout(): that one is
        // clamped to what is left of the §5.2 exchange budget, and the exchange
        // budget ran out long ago — a ring lasts minutes. Nothing here is an
        // exchange; it is one write on a link that has outlived its exchange.
        armTimer(WatchProtocol.OPERATION_TIMEOUT_MS, actions)
        actions += Action.WriteFindDismiss(WatchProtocol.encodeFindDismiss())
    }

    /**
     * Every way out of [ConnectionState.Ringing] that is not the user's own tap:
     * silence, close, and — §4.1 — re-arm **immediately**. No settle: the watch
     * may still be searching and wants the phone back; if it is not, the pending
     * autoConnect simply waits. No backoff: nothing failed.
     */
    private fun endRinging(actions: MutableList<Action>) {
        cancelTimer(actions)
        actions += Action.StopFindAlarm
        closeLink(actions)
        if (isBlocked()) {
            // The radio went away in the same breath. Blocked is a waiting state
            // too, and AdapterOn / PermissionGranted arms from it.
            state = blockedState()
            return
        }
        arm(actions)
    }

    /**
     * The user's tap. The alarm stops now — not when the watch answers — because
     * the person holding the phone has already found it, and a write that takes
     * five seconds to fail must not keep them listening to it. Then the §3.3
     * frame goes out with a per-operation timeout, and the link closes on the
     * reply.
     *
     * If the Find subscription is still outstanding the write waits for its
     * callback — one operation at a time on the link (Law 2) — and only the alarm
     * stops now. The tap is remembered in the state, so the callback knows where
     * to go next.
     */
    private fun onFindDismissedOnPhone(actions: MutableList<Action>) {
        when (val current = state) {
            ConnectionState.Ringing -> {
                cancelTimer(actions)
                actions += Action.StopFindAlarm
                writeDismiss(actions)
            }

            is ConnectionState.SubscribingFind -> {
                if (current.dismissRequested) return
                actions += Action.StopFindAlarm
                // The CCCD write and its timeout stay as they are; the tap only
                // decides what its callback leads to.
                state = ConnectionState.SubscribingFind(dismissRequested = true)
            }

            else -> Unit
        }
    }

    /** The dismiss write is over, one way or another. Close and re-arm at once. */
    private fun finishDismiss(actions: MutableList<Action>) {
        cancelTimer(actions)
        closeLink(actions)
        if (isBlocked()) {
            state = blockedState()
            return
        }
        arm(actions)
    }

    private fun onRetryTimerFired(token: Long, actions: MutableList<Action>) {
        if (!isCurrentTimer(TimerKind.Retry, token)) return
        // Same reasoning as above: check first, disarm only once we are certain
        // we are going to replace what we disarmed.
        if (state !is ConnectionState.WaitingForRetry) return
        armedTimer = TimerKind.None
        if (isBlocked()) {
            state = blockedState()
            return
        }
        arm(actions)
    }

    /**
     * Close, count it, and schedule the retry. The single exit for every failure
     * in the file — which is what makes "always `close()`, always end waiting"
     * a property of the code rather than a promise about it.
     */
    private fun failExchange(
        outcome: ExchangeOutcome,
        actions: MutableList<Action>,
        status: WatchStatus? = null,
        statusResultCode: Int = HealthSnapshot.NO_RESULT_CODE,
    ) {
        cancelTimer(actions)
        closeLink(actions)
        // One read, used by both the counters and the report: two reads could
        // straddle an NTP correction and date the same exchange twice.
        val now = clock.utcEpochSeconds()
        healthCounter.recordFailure(now, outcome, statusResultCode)
        actions += Action.PersistHealth(health)
        actions += Action.ReportExchange(outcome, status, now)
        scheduleRetry(actions)
    }

    private fun succeed(status: WatchStatus, actions: MutableList<Action>) {
        cancelTimer(actions)
        // §4: the phone hangs up after the notify. The watch tears its radio down
        // and sleeps either way.
        closeLink(actions)
        val now = clock.utcEpochSeconds()
        healthCounter.recordSuccess(now, status.resultCode)
        actions += Action.PersistHealth(health)
        actions += Action.ReportExchange(ExchangeOutcome.Succeeded, status, now)
        settle(actions)
    }

    /**
     * Wait out the rest of the watch's window, then re-arm.
     *
     * Not a backoff, and it must not be mistaken for one: nothing is counted,
     * health is untouched, and §6.2's "a disconnect after a successful notify is
     * normal" still holds. It is a bounded pause with exactly one purpose -
     * making sure the next `autoConnect` cannot land back inside the window the
     * app has just finished using. See [ConnectionState.Settling] for what
     * happened on hardware without it.
     */
    private fun settle(actions: MutableList<Action>) {
        if (isBlocked()) {
            // The radio went away in the same breath as the success. Blocked is
            // also a waiting state, and AdapterOn / PermissionGranted arms from it.
            state = blockedState()
            return
        }
        val delayMillis = remainingExchangeBudgetMillis()
        if (delayMillis <= 0L) {
            // The exchange used its whole budget, so the watch is already past its
            // §5.1 cap and its window is shut. Nothing left to wait out.
            arm(actions)
            return
        }
        armTimer(delayMillis, actions)
        state = ConnectionState.Settling(delayMillis)
    }

    private fun scheduleRetry(actions: MutableList<Action>) {
        if (isBlocked()) {
            // The radio went away while we were failing. Blocked is also a
            // waiting state, and AdapterOn / PermissionGranted arms from there.
            state = blockedState()
            return
        }
        val delayMillis = backoff.delayMillisFor(health.retryAttemptIndex)
        actions += Action.ScheduleRetry(delayMillis, nextTimerToken(TimerKind.Retry))
        state = ConnectionState.WaitingForRetry(delayMillis)
    }

    // ── Link and timer bookkeeping ───────────────────────────────────────────

    private fun arm(actions: MutableList<Action>) {
        cancelTimer(actions)
        // Belt and braces: arming while a client is still open would leak one of
        // the process's limited GATT client slots. Unreachable by construction —
        // every caller has already closed — and cheap enough to keep that way.
        closeLink(actions)
        actions += Action.ArmAutoConnect
        linkOpen = true
        state = ConnectionState.Armed
    }

    private fun closeLink(actions: MutableList<Action>) {
        if (!linkOpen) return
        actions += Action.CloseConnection
        linkOpen = false
    }

    private fun cancelTimer(actions: MutableList<Action>) {
        if (armedTimer == TimerKind.None) return
        actions += Action.CancelTimers
        armedTimer = TimerKind.None
        // Bump the token so a firing already in flight is recognised as stale.
        timerToken++
    }

    private fun armOperationTimeout(actions: MutableList<Action>) {
        armTimer(minOf(WatchProtocol.OPERATION_TIMEOUT_MS, remainingExchangeBudgetMillis()), actions)
    }

    private fun armTimer(timeoutMillis: Long, actions: MutableList<Action>) {
        actions += Action.ArmOperationTimeout(timeoutMillis, nextTimerToken(TimerKind.Operation))
    }

    /** Tokens come from one counter across both kinds, so no two are ever equal. */
    private fun nextTimerToken(kind: TimerKind): Long {
        timerToken++
        armedTimer = kind
        return timerToken
    }

    private fun isCurrentTimer(kind: TimerKind, token: Long): Boolean =
        armedTimer == kind && token == timerToken

    /**
     * What is left of §5.2's 15 s whole-exchange budget.
     *
     * Every per-operation timeout is clamped to this, so exactly one timer is
     * ever outstanding and the exchange still cannot outlive its budget. The
     * elapsed time is clamped at zero first: [Clock.monotonicMillis] promises not
     * to go backwards, but a process that runs for months does not stake "the
     * budget never expires" on a platform promise.
     */
    private fun remainingExchangeBudgetMillis(): Long {
        val elapsed = (clock.monotonicMillis() - connectedAtMillis).coerceAtLeast(0L)
        return (WatchProtocol.EXCHANGE_TIMEOUT_MS - elapsed).coerceAtLeast(0L)
    }
}
