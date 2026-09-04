package com.workaday.app

import android.Manifest
import android.app.AlarmManager
import android.app.ForegroundServiceStartNotAllowedException
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import com.workaday.core.Action
import com.workaday.core.Backoff
import com.workaday.core.ConnectionStateMachine
import com.workaday.core.ExchangeReport
import com.workaday.core.JitterSource
import com.workaday.core.RestingSummary
import com.workaday.core.ServiceNotice
import com.workaday.core.StartupPlan
import com.workaday.core.StartupSkipReason
import com.workaday.core.StartupTrigger
import com.workaday.core.TransportEvent
import com.workaday.core.planStartup
import com.workaday.core.restingSummaryFor
import com.workaday.core.serviceNoticeFor

/**
 * The app's resting state, and the only place its work happens.
 *
 * Law 1, item by item: a **foreground service of type `connectedDevice`**
 * (declared in the manifest, and `dataSync` would break start-on-boot outright —
 * `docs/background-execution.md` §1); **`START_STICKY`**, backed by a
 * [Watchdog] that does not trust it; started from [BootReceiver] on all three
 * boot-ish broadcasts; and owning everything that matters, so that killing the
 * Activity, swiping the app from Recents, or never opening it again changes
 * nothing.
 *
 * Law 3, item by item: this class decides nothing. It observes the platform,
 * feeds [ConnectionStateMachine], and performs the `Action`s it hands back. Every
 * `when` below is either an exhaustive execution of those actions or a
 * translation of a platform constant into the machine's vocabulary.
 *
 * **Threading.** One `HandlerThread` owns everything: the machine, the GATT
 * client, the timers and the store. GATT callbacks arrive on binder threads and
 * are *posted* to it; the broadcast receiver runs on the main thread and posts to
 * it; the timers fire on it. That single serialising context is what makes "one
 * outstanding GATT operation at a time" a property of the code rather than a
 * hope, and it is why neither the machine nor [com.workaday.core.ClientEpoch]
 * needs to be thread-safe. Nothing here blocks the main thread: the only work
 * done there is `startForeground`, which must be prompt, and which needs no I/O.
 */
class WatchLinkService : Service() {

    /** Everything that exists only while there is a watch to talk to. */
    private class LinkRuntime(
        val store: WatchStore,
        val machine: ConnectionStateMachine,
        val link: WatchLink,
        val operationTimer: OperationTimer,
        val retryAlarm: RetryAlarm,
        val alarm: FindPhoneAlarm,
    )

    private lateinit var handlerThread: HandlerThread
    private lateinit var handler: Handler
    private lateinit var notificationManager: NotificationManager

    /** Built by [initialise] on the serial thread, and only ever touched there. */
    private var runtime: LinkRuntime? = null

    /**
     * What the notification currently says. Written on the serial thread, read on
     * the main thread by [onStartCommand], hence volatile — and it is a caption,
     * so a stale read costs one redundant `notify`, not a decision.
     */
    @Volatile
    private var notice: ServiceNotice = ServiceNotice.Starting

    /**
     * The two facts the resting caption carries, alongside [notice] and for the
     * same reasons: written on the serial thread, read on the main thread by
     * [onStartCommand], and a caption rather than a decision.
     *
     * It starts empty because filling it is a [WatchStore] read and the main thread
     * does not do disk I/O. The cost is that the very first notification of a
     * process says "nothing synced yet" until [dispatch] runs a few milliseconds
     * later on the serial thread and replaces it.
     */
    @Volatile
    private var summary: RestingSummary = RestingSummary.NothingYet

    /**
     * The adapter's state changes and the backoff alarm.
     *
     * Registered at runtime rather than in the manifest because
     * `BluetoothAdapter.ACTION_STATE_CHANGED` is an implicit broadcast, and
     * implicit broadcasts have not been deliverable to manifest receivers since
     * API 26. It is unregistered in [onDestroy]; nothing outside this service's
     * lifetime listens for either.
     */
    private val systemEvents = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)
                    adapterEventFor(state)?.let(::post)
                }

                RetryAlarm.ACTION_RETRY ->
                    post(TransportEvent.RetryTimerFired(RetryAlarm.tokenFrom(intent)))

                // Credential-encrypted storage just became readable, so
                // WorkManager is reachable for the first time. See
                // ensureWatchdogScheduled.
                Intent.ACTION_USER_UNLOCKED -> handler.post {
                    ensureWatchdogScheduled(watchPaired = WatchStore.open(this@WatchLinkService).watchAddress != null)
                }

                else -> Unit
            }
        }
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        isRunning = true
        notificationManager = getSystemService(NotificationManager::class.java)
        ServiceNotification.ensureChannel(this, notificationManager)
        FindPhoneAlarm.ensureChannel(this, notificationManager)

        handlerThread = HandlerThread(THREAD_NAME).apply { start() }
        handler = Handler(handlerThread.looper)
        // Posted first, so FIFO ordering guarantees it runs before any event.
        handler.post(::initialise)

        registerSystemEvents()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Promptly, on every path into the service, before anything else can
        // throw: the system gives us seconds, not milliseconds.
        if (!enterForeground()) {
            // The start was refused. Stop rather than sit half-alive, and let the
            // watchdog try again on its next period — a bounded retry, which is
            // what Law 2 asks for and what an immediate sticky restart would not
            // be.
            stopSelf()
            return START_NOT_STICKY
        }
        // Law 1. The watchdog exists because not every OEM honours it.
        handler.post(::onStarted)
        // The one event that reaches the machine from the UI rather than from the
        // radio: the Stop button on the find-phone screen or its notification
        // (PROTOCOL.md §4.1). Posted after onStarted for FIFO's sake; the machine
        // ignores it outside Ringing, so a stale tap costs nothing.
        if (intent?.action == ACTION_DISMISS_FIND) post(TransportEvent.FindDismissedOnPhone)
        return START_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        unregisterReceiver(systemEvents)
        handler.post(::shutDown)
        // quitSafely, not quit: the shutdown message above is already queued and
        // this lets it run. It is the one that closes the GATT client.
        handlerThread.quitSafely()
        super.onDestroy()
    }

    /** Not a bound service (Law 1). */
    override fun onBind(intent: Intent?): IBinder? = null

    // ── The serial thread ────────────────────────────────────────────────────

    private fun initialise() {
        val store = WatchStore.open(this)
        // One answer, used for both the plan and the link. Asking the store again
        // below would let an association landing between the two reads produce a
        // plan that disagrees with the address it was computed from.
        val address = reArmFromAssociation(store)
        val plan = ensureWatchdogScheduled(watchPaired = address != null)

        // The second half of the condition is redundant — `address == null` is
        // exactly what produced `NoWatchPaired` — and is here only because it is
        // the form Kotlin can smart-cast on. The verdict is the first half.
        if (plan.skipped == StartupSkipReason.NoWatchPaired || address == null) {
            // No CompanionDeviceManager association to arm an autoConnect against.
            // The association flow in DiagnosticsActivity starts the service again
            // once there is one.
            Log.w(LOG_TAG, "no associated watch; stopping until one is paired")
            stopSelf()
            return
        }

        val operationTimer = OperationTimer(handler, getSystemService(PowerManager::class.java)) { token ->
            dispatch(TransportEvent.OperationTimedOut(token))
        }
        runtime = LinkRuntime(
            store = store,
            machine = ConnectionStateMachine(
                clock = AndroidClock,
                // The production jitter source. PROTOCOL.md §5.2's ±20 %, so a
                // fleet of one still does not retry on a metronome.
                backoff = Backoff(JitterSource.uniform()),
                // Law 2: health survives process death, and the backoff it drives
                // resumes where it left off rather than at 30 s.
                initialHealth = store.readHealth(),
            ),
            link = WatchLink.forDevice(
                context = applicationContext,
                adapter = getSystemService(BluetoothManager::class.java)?.adapter,
                deviceAddress = address,
                handler = handler,
                dispatch = ::dispatch,
            ),
            operationTimer = operationTimer,
            retryAlarm = RetryAlarm(applicationContext, getSystemService(AlarmManager::class.java)),
            alarm = FindPhoneAlarm(applicationContext, handler),
        )
    }

    /**
     * "Boot: receiver → start the service → **re-arm from the surviving CDM
     * association. No re-pairing, no scan**" — `docs/background-execution.md` §3.
     *
     * Associations outlive reboots and are revoked only by uninstall, by
     * `disassociate()`, or by us — so the address written down at pairing time is
     * normally still good, and there is nothing to re-pair and nothing to scan for.
     * The job here is the abnormal case: noticing that the record behind the
     * address has gone, before "paired" becomes a permanent lie.
     *
     * **Called once per service *creation*,** from [initialise] — so on every boot,
     * every package replacement, every restart after an OEM kill, and every fresh
     * process, but *not* on a watchdog poke of a service that is already up. That
     * is deliberate: the address is what [LinkRuntime] was built around, so a
     * different answer mid-life would mean rebuilding the runtime rather than
     * merely noting it. The window it leaves is an association revoked in Settings
     * while this process happens to survive; the next restart closes it, and the
     * app spends the interval in its bounded backoff rather than anywhere new.
     *
     * The decision is `reconcileAssociation` in `core/` and performing it is
     * [reArmedWatchAddress], which is a free function rather than a method here so
     * that a JVM test can drive the branch that writes and the branch that clears.
     * All this one does is name the three observations.
     */
    private fun reArmFromAssociation(store: WatchStore): String? = reArmedWatchAddress(
        storedAddress = store.watchAddress,
        associatedAddresses = Associations.observed(
            userUnlocked = isUserUnlocked(this),
            read = Associations.forContext(this),
        ),
        writeAddress = { store.watchAddress = it },
    )

    /**
     * Put Law 1's watchdog in place, if it can be reached yet.
     *
     * Two callers, and the second one is the whole reason this is a function.
     * On a phone with a lock screen the service starts during direct boot, where
     * WorkManager's credential-encrypted database does not exist yet — so
     * `initialise` cannot schedule the watchdog and the running process may never
     * be told to try again. `BOOT_COMPLETED` arrives at [BootReceiver] on unlock,
     * but a receiver in a process that started before unlock can find WorkManager's
     * initialisation provider was never installed. `ACTION_USER_UNLOCKED` reaches
     * this service directly, which is the one path that is certainly ours.
     *
     * Returns the plan so [initialise] can also read the "is there a watch at
     * all" half of it.
     */
    private fun ensureWatchdogScheduled(watchPaired: Boolean): StartupPlan {
        val plan = planStartup(
            trigger = StartupTrigger.ServiceStarted,
            watchPaired = watchPaired,
            serviceRunning = true,
            userUnlocked = isUserUnlocked(this),
        )
        logInfo("service supervision: $plan")
        if (plan.enqueueWatchdog) Watchdog.enqueue(applicationContext)
        return plan
    }

    /**
     * Tell the machine what the world looks like, then that we are up.
     *
     * The order matters and the machine is built for it: `AdapterOff` before
     * `Started` parks it in `Blocked` instead of calling `connectGatt` into a
     * switched-off radio. All three are idempotent, so running this on every
     * `onStartCommand` — boot, watchdog, a sticky restart — costs nothing.
     */
    private fun onStarted() {
        dispatch(if (isAdapterOn()) TransportEvent.AdapterOn else TransportEvent.AdapterOff)
        dispatch(if (hasConnectPermission()) TransportEvent.PermissionGranted else TransportEvent.PermissionRevoked)
        dispatch(TransportEvent.Started)
    }

    /**
     * The whole of the app's behaviour, in six lines: feed one observation,
     * perform what comes back.
     */
    private fun dispatch(event: TransportEvent) {
        val runtime = runtime ?: return
        val actions = runtime.machine.onEvent(event)
        if (actions.isNotEmpty()) logInfo("$event -> ${runtime.machine.state}")
        actions.forEach { perform(runtime, it) }
        updateNotification(runtime)
    }

    /**
     * Exhaustive, and every branch is an effect. There is no `if` here choosing
     * whether to do something — that is the machine's job (Law 3).
     */
    private fun perform(runtime: LinkRuntime, action: Action) {
        when (action) {
            Action.ArmAutoConnect -> runtime.link.arm()
            Action.CloseConnection -> runtime.link.close()
            Action.DiscoverServices -> runtime.link.discoverServices()
            Action.EnableStatusNotifications -> runtime.link.enableStatusNotifications()
            Action.EnableFindNotifications -> runtime.link.enableFindNotifications()
            is Action.WriteTime -> runtime.link.writeTime(action.payload)
            is Action.WriteFindDismiss -> runtime.link.writeFindDismiss(action.payload)

            // PROTOCOL.md §4.1. Vibration and the Stop screen, the tone when asked
            // for, started and stopped together; the machine pairs every start
            // with a stop, and changes the mode only in between.
            is Action.StartFindAlarm -> runtime.alarm.start(sound = action.sound)
            is Action.SetFindAlarmSound -> runtime.alarm.setSound(action.sound)
            Action.StopFindAlarm -> runtime.alarm.stop()

            is Action.ArmOperationTimeout ->
                runtime.operationTimer.arm(action.timeoutMillis, action.token)

            is Action.ScheduleRetry ->
                runtime.retryAlarm.schedule(action.delayMillis, action.token)

            // Both, unconditionally. The machine knows which one is pending; this
            // side does not need to, and both cancels are idempotent.
            Action.CancelTimers -> {
                runtime.operationTimer.cancel()
                runtime.retryAlarm.cancel()
            }

            is Action.PersistHealth -> runtime.store.writeHealth(action.snapshot)

            // Written down, not merely logged. PROTOCOL.md §6.2 requires the
            // `result` code to be visible — `BadVersion` above all, since it means
            // the two sides have drifted and no amount of retrying will help — and
            // a log line is visible only to whoever has `adb` plugged in. The
            // record outlives this process, which matters because the screen that
            // reads it is usually opened *because* the service is not running.
            is Action.ReportExchange -> {
                logInfo("exchange ${action.outcome}${action.status?.let { " $it" } ?: ""}")
                runtime.store.writeLastExchange(ExchangeReport.from(action))
            }
        }
    }

    private fun shutDown() {
        val runtime = runtime ?: return
        this.runtime = null
        // Order: stop the clocks, then the radio. Cancelling the operation timer
        // is also what releases the wakelock.
        runtime.operationTimer.cancel()
        runtime.retryAlarm.cancel()
        // Law 2's "on every path including cancellation". This is that path.
        runtime.link.close()
        // And the alarm, for the same reason: a service being destroyed mid-ring
        // must not leave the phone sounding with nothing left to stop it.
        runtime.alarm.stop()
        logInfo("service stopped; health ${runtime.machine.health}")
    }

    // ── Foreground and notification ──────────────────────────────────────────

    /**
     * @return false when the system refused the start, in which case the caller
     *   must stop rather than continue as a background service that will be killed
     *   without warning.
     */
    private fun enterForeground(): Boolean = try {
        startForeground(
            ServiceNotification.ID,
            ServiceNotification.build(this, notice, summary),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
        )
        true
    } catch (e: ForegroundServiceStartNotAllowedException) {
        // Also the parent of Android 14's Missing/InvalidForegroundServiceType
        // exceptions. Genuinely handleable: the remedy is to stop and let the
        // watchdog retry, and crashing here would take out whatever started us —
        // including the boot receiver.
        Log.e(LOG_TAG, "foreground start refused", e)
        false
    } catch (e: SecurityException) {
        // Android 14+ requires one of the Bluetooth permissions to run a
        // connectedDevice foreground service. Revoking it kills this process
        // anyway; this is the restart afterwards.
        Log.e(LOG_TAG, "not permitted to run a connectedDevice foreground service", e)
        false
    }

    /**
     * Both halves of the caption, re-posted only when one of them has changed.
     *
     * The store read is the last exchange the app wrote down, which
     * [com.workaday.core.Action.ReportExchange] has already committed by the time
     * this runs — `dispatch` performs the actions first. It is a
     * `SharedPreferences` lookup out of an in-memory map, on the serial thread,
     * and reading it rather than remembering it is what lets the first
     * notification of a fresh process carry the previous process's numbers.
     *
     * Neither value depends on the current time, so this comparison is stable at
     * rest: a summary that has not changed does not re-post, and the "how long
     * ago" that does change every minute is SystemUI's chronometer, not ours.
     */
    private fun updateNotification(runtime: LinkRuntime) {
        val nextNotice = serviceNoticeFor(runtime.machine.state)
        val nextSummary = restingSummaryFor(runtime.machine.health, runtime.store.readLastExchange())
        if (nextNotice == notice && nextSummary == summary) return
        notice = nextNotice
        summary = nextSummary
        // Silently dropped if POST_NOTIFICATIONS was denied. The service keeps
        // running either way (docs/background-execution.md §2).
        notificationManager.notify(
            ServiceNotification.ID,
            ServiceNotification.build(this, nextNotice, nextSummary),
        )
    }

    // ── Observing the platform ───────────────────────────────────────────────

    private fun registerSystemEvents() {
        val filter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(RetryAlarm.ACTION_RETRY)
            // Registered-receiver only; it cannot be declared in the manifest.
            addAction(Intent.ACTION_USER_UNLOCKED)
        }
        // NOT_EXPORTED: two of the three are protected system broadcasts and the
        // third is our own alarm, package-scoped. The flag keeps other apps out
        // without keeping the system out — a broadcast whose sender is the system
        // uid, or this app's own uid, is delivered regardless.
        //
        // No version branch. The flags overload has existed since API 26 and this
        // bit is simply unset there; the constant is a compile-time `int`, so it
        // inlines and needs no API-33 guard.
        registerReceiver(systemEvents, filter, Context.RECEIVER_NOT_EXPORTED)
    }

    private fun isAdapterOn(): Boolean =
        getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

    private fun hasConnectPermission(): Boolean =
        checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    /**
     * The adapter's state constants in the machine's vocabulary.
     *
     * `TURNING_OFF` counts as off: the stack is going away and closing the client
     * now is what keeps it from being torn out from under us. `TURNING_ON` counts
     * as nothing — arming against a half-started adapter buys a `status 133` and
     * a backoff step, and `STATE_ON` follows within moments anyway.
     */
    private fun adapterEventFor(state: Int): TransportEvent? = when (state) {
        BluetoothAdapter.STATE_ON -> TransportEvent.AdapterOn
        BluetoothAdapter.STATE_OFF, BluetoothAdapter.STATE_TURNING_OFF -> TransportEvent.AdapterOff
        else -> null
    }

    private fun post(event: TransportEvent) {
        handler.post { dispatch(event) }
    }

    companion object {
        private const val THREAD_NAME = "workaday-link"

        /** The Stop button's intent action (PROTOCOL.md §4.1). */
        const val ACTION_DISMISS_FIND = "com.workaday.app.action.DISMISS_FIND"

        /**
         * The intent the find-phone alarm's Stop delivers, from its notification
         * action and from [FindPhoneActivity] alike. One definition, so the two
         * routes cannot drift apart.
         */
        fun dismissFindIntent(context: Context): Intent =
            Intent(context, WatchLinkService::class.java).setAction(ACTION_DISMISS_FIND)

        /**
         * The user silenced the find-phone alarm on the phone.
         *
         * `startForegroundService` on a service that is already in the foreground
         * — it is, while it rings — simply delivers another `onStartCommand`, and
         * from a foreground Activity or a notification action the start is
         * permitted regardless. The exception is caught for the reason [start]
         * catches it: this runs from a tap and a crash here would take the screen
         * offering to stop the alarm down with it.
         */
        fun dismissFind(context: Context) {
            try {
                context.startForegroundService(dismissFindIntent(context))
            } catch (e: ForegroundServiceStartNotAllowedException) {
                Log.e(LOG_TAG, "not allowed to reach the service to stop the alarm", e)
            }
        }

        /**
         * Whether the service is up **in this process**.
         *
         * The watchdog's "actually checks rather than assuming". A killed process
         * loses the flag along with the service, which is exactly right: the
         * watchdog runs in a fresh process, reads false, and starts one.
         */
        @Volatile
        var isRunning: Boolean = false
            private set

        /**
         * The one way anything starts this service.
         *
         * Legal from the background because of the two exemptions
         * `docs/background-execution.md` §1 records: the three boot-ish broadcasts,
         * and the CompanionDeviceManager association's
         * `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND`.
         */
        fun start(context: Context) {
            try {
                context.startForegroundService(Intent(context, WatchLinkService::class.java))
            } catch (e: ForegroundServiceStartNotAllowedException) {
                // Never allowed to propagate: this runs inside a BroadcastReceiver
                // and inside a Worker, and throwing in either loses the whole
                // startup pass instead of the one attempt.
                Log.e(LOG_TAG, "not allowed to start the service from the background", e)
            }
        }
    }
}
