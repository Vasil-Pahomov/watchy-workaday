package com.workaday.core

/**
 * Why something is asking whether the foreground service should be running.
 *
 * One value per entry point. They are kept apart rather than collapsed into a
 * boolean because [planStartup] genuinely answers two of them differently, and
 * because a log line that says which door the app came in through is worth more
 * than one that says "startup".
 */
enum class StartupTrigger {

    /** `ACTION_BOOT_COMPLETED`. The user has unlocked at least once. */
    BootCompleted,

    /**
     * `ACTION_LOCKED_BOOT_COMPLETED` — direct boot, before the first unlock.
     *
     * Only direct-boot-aware components receive it, and only device-protected
     * storage is readable. That is the whole reason this value exists separately.
     */
    LockedBootCompleted,

    /** `ACTION_MY_PACKAGE_REPLACED`. An update must not cost the user a reboot. */
    PackageReplaced,

    /** The periodic WorkManager watchdog woke up, because `START_STICKY` is a hope. */
    Watchdog,

    /** The service itself came up and is checking its own supporting machinery. */
    ServiceStarted,
}

/**
 * Why [planStartup] is not starting the service, or null when it is.
 *
 * Also the answer to a second question, which is why [NoWatchPaired] outranks
 * [ServiceAlreadyRunning] rather than the other way round: the service asks the
 * same function "should I still be running?", and for that caller "there is no
 * watch" is the verdict that matters while "you are already running" is a
 * tautology.
 */
enum class StartupSkipReason {

    /**
     * No CompanionDeviceManager association yet, so there is no device address to
     * hand `connectGatt`. A foreground service in this state would hold a
     * permanent notification saying it was watching for a watch that does not
     * exist.
     */
    NoWatchPaired,

    /** The resting state is already the resting state. */
    ServiceAlreadyRunning,
}

/**
 * What the caller should do, in the order given. It performs these; it decides
 * nothing (Law 3).
 */
data class StartupPlan(
    /** `startForegroundService()`. True exactly when [skipped] is null. */
    val startService: Boolean,

    /**
     * `startForegroundService()` on a service that is **already running**, so
     * that it looks at its environment again.
     *
     * This exists because of one asymmetry, and the asymmetry is what makes the
     * bug it fixes easy to miss. [TransportEvent.AdapterOff] and
     * [TransportEvent.AdapterOn] have a live producer — the adapter's
     * `ACTION_STATE_CHANGED` broadcast — so the adapter half of
     * [ConnectionState.Blocked] heals itself. `PermissionRevoked` and
     * `PermissionGranted` have no broadcast behind them: the app can only *look*,
     * and it looks when the service starts. Without a way to make a running
     * service look again, a machine parked in `Blocked(permissionMissing = true)`
     * has no reachable exit for as long as the process survives — a terminal
     * error state, which Law 2 forbids outright.
     *
     * The scenario is not exotic. Android auto-resets the permissions of an app
     * the user never opens, and never opening this app is its entire design
     * premise. The user then grants the permission again in Settings, which does
     * not restart the process; the app would sit forever with the permission
     * granted and a notification still asking for it. A second route in needs no
     * platform assumption at all: any `SecurityException` the Android layer
     * translates into `PermissionRevoked` latches the machine the same way, and
     * that translation is not authoritative about whether a revocation really
     * happened.
     *
     * Recovery is therefore bounded at one watchdog period. Safe to do at any
     * time: re-observing is idempotent — the machine ignores `AdapterOn` and
     * `PermissionGranted` outside [ConnectionState.Blocked] and ignores a repeat
     * `Started` — so it costs one `startForeground` call and cannot disturb an
     * exchange in flight.
     *
     * Never true at the same time as [startService]: that one needs the service
     * down, this one needs it up.
     */
    val refreshRunningService: Boolean,

    /** Enqueue the unique periodic watchdog. */
    val enqueueWatchdog: Boolean,

    /** Null when [startService]; otherwise why not. */
    val skipped: StartupSkipReason?,
)

/**
 * The one place that decides whether the foreground service should be running and
 * whether the watchdog can be scheduled.
 *
 * Four entry points ask it — the boot receiver for each of its three broadcasts,
 * and the watchdog worker — and the service asks it about itself. Putting the
 * question here rather than in each of them is Law 3's rule about `if`s inside a
 * `BroadcastReceiver`, and it is also the only way the direct-boot case below is
 * testable at all: reproducing "the user has not unlocked since boot" on a JVM is
 * impossible, reproducing `userUnlocked = false` is one argument. The same is
 * true of [StartupPlan.refreshRunningService], whose whole justification is a
 * permission the user re-grants from Settings without restarting the process.
 *
 * @param watchPaired whether an association has supplied a device address.
 * @param serviceRunning whether the foreground service is up **in this process**.
 * @param userUnlocked `UserManager.isUserUnlocked()`. Not derived from [trigger]:
 *   a device with no lock screen is unlocked during `LOCKED_BOOT_COMPLETED`, and
 *   a caller that guessed from the trigger would skip the watchdog forever there.
 */
fun planStartup(
    trigger: StartupTrigger,
    watchPaired: Boolean,
    serviceRunning: Boolean,
    userUnlocked: Boolean,
): StartupPlan {
    val skipped = when {
        !watchPaired -> StartupSkipReason.NoWatchPaired
        serviceRunning -> StartupSkipReason.ServiceAlreadyRunning
        else -> null
    }
    return StartupPlan(
        startService = skipped == null,
        // Only the watchdog. It is the one caller that runs on a schedule while
        // the service is up, so it is the only one that can bound the recovery
        // from a latched Blocked(permissionMissing) at a known interval. The boot
        // broadcasts do not need it — nothing was running to refresh — and the
        // service asking about itself has just observed everything anyway.
        refreshRunningService = trigger == StartupTrigger.Watchdog && serviceRunning && watchPaired,
        // WorkManager keeps its queue in a Room database in credential-encrypted
        // storage, so it is not direct-boot aware: touching it before the first
        // unlock throws, and it would throw inside a BOOT_COMPLETED receiver,
        // which is the worst possible place. LOCKED_BOOT_COMPLETED is always
        // followed by BOOT_COMPLETED once the user unlocks, and that pass is
        // where the watchdog actually gets enqueued.
        //
        // Deliberately not gated on watchPaired: the watchdog is what notices the
        // service is gone, and it must already be in place on the day the user
        // finally pairs a watch.
        enqueueWatchdog = userUnlocked && trigger != StartupTrigger.Watchdog,
        skipped = skipped,
    )
}
