package com.workaday.core

import com.workaday.core.protocol.WatchProtocol

/**
 * What the app can see about its own permission to keep working.
 *
 * **Every field is a plain boolean, and that is a deliberate defence rather than a
 * style choice.** `app/` builds tests against the stubbed `android.jar` with
 * `isReturnDefaultValues = true`, and `PackageManager.PERMISSION_GRANTED` is `0` —
 * so a JVM test that reaches `checkSelfPermission` gets `0` back, reads it as
 * *granted*, and passes without touching a single real decision. A whole screen
 * about permissions is exactly the change that trap is waiting for. Observing in
 * `app/` and deciding here means the decision is reachable by a test that has no
 * framework to fool it.
 */
data class SetupObservations(
    /** A CompanionDeviceManager association backs a stored watch address. */
    val watchAssociated: Boolean,

    /** `BLUETOOTH_CONNECT`. Without it there is no GATT at all. */
    val bluetoothPermissionGranted: Boolean,

    /** `POST_NOTIFICATIONS`, or true below API 33 where it is not a runtime grant. */
    val notificationPermissionGranted: Boolean,

    /** The adapter is on. Airplane mode is indistinguishable and counts as off. */
    val bluetoothAdapterOn: Boolean,

    /** `PowerManager.isIgnoringBatteryOptimizations`. */
    val ignoringBatteryOptimisations: Boolean,
)

/**
 * Something the user can go and do, in the order it is worth doing.
 *
 * Deliberately only things the app can *observe* the state of. The one thing that
 * matters most — the OEM battery manager — is not here, because
 * `docs/background-execution.md` §2 says no API detects it reliably, and a
 * checklist item that can never be ticked is worse than advice that never claims
 * to be one. That is [OemBatteryAdvice]'s job.
 */
enum class SetupTask {

    /**
     * First, always: without an association there is no address, the service stops
     * itself, and none of the rest of the list matters (Law 1 — "ship the
     * association flow before anything else that touches the radio").
     */
    AssociateWatch,

    /** `BLUETOOTH_CONNECT`. The app cannot open a socket without it. */
    GrantBluetoothPermission,

    /** The adapter, or airplane mode. Self-healing once the user fixes it. */
    TurnBluetoothOn,

    /**
     * `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`. Legitimate here only because this
     * app is sideloaded rather than distributed through Play (Law 5,
     * `docs/toolchain.md`).
     */
    ExemptFromBatteryOptimisation,

    /**
     * `POST_NOTIFICATIONS`. Last because the service runs without it — but the
     * user then has no way to see that it is running, which is half of what
     * `docs/background-execution.md` §2 asks the app to show.
     */
    GrantNotificationPermission,
}

/** How the app answers "am I set up properly?" in one word. */
enum class SetupVerdict {

    /** No watch associated. Nothing else is worth reading yet. */
    NotPaired,

    /** Paired, but something is stopping it working *right now*. */
    Blocked,

    /**
     * Working, but with a foot out of the door: the system may stop it in the
     * background, or the user would not be able to tell if it did.
     */
    AtRisk,

    /** Everything the app can observe is as it should be. */
    Ready,
}

/** [SetupVerdict] plus the outstanding [SetupTask]s, most important first. */
data class SetupAssessment(val verdict: SetupVerdict, val tasks: List<SetupTask>)

/**
 * The whole of "does this app look correctly set up", as a pure function.
 *
 * The ordering of [SetupVerdict] is not the ordering of [SetupTask]: a missing
 * notification permission is the *last* thing to fix but still enough to make the
 * verdict [SetupVerdict.AtRisk], because an app the user cannot see stop is the
 * failure Law 2 refuses, only quieter.
 */
fun assessSetup(observations: SetupObservations): SetupAssessment {
    val tasks = buildList {
        if (!observations.watchAssociated) add(SetupTask.AssociateWatch)
        if (!observations.bluetoothPermissionGranted) add(SetupTask.GrantBluetoothPermission)
        if (!observations.bluetoothAdapterOn) add(SetupTask.TurnBluetoothOn)
        if (!observations.ignoringBatteryOptimisations) add(SetupTask.ExemptFromBatteryOptimisation)
        if (!observations.notificationPermissionGranted) add(SetupTask.GrantNotificationPermission)
    }
    val verdict = when {
        !observations.watchAssociated -> SetupVerdict.NotPaired
        !observations.bluetoothPermissionGranted || !observations.bluetoothAdapterOn -> SetupVerdict.Blocked
        !observations.ignoringBatteryOptimisations ||
            !observations.notificationPermissionGranted -> SetupVerdict.AtRisk

        else -> SetupVerdict.Ready
    }
    return SetupAssessment(verdict, tasks)
}

/**
 * How long ago the last successful exchange was, on the only scale that means
 * anything: the rate at which the watch offers one.
 *
 * `docs/background-execution.md` §2 makes this a product requirement — the OEM
 * battery managers "kill background services regardless of the AOSP rules", "no
 * API detects this reliably", and the app "must tell the user what to do on their
 * phone **and show whether it looks like it worked**". This is the second half.
 * There is nothing to query, so the evidence is behavioural: an app that is still
 * being allowed to run keeps catching the watch's windows, and one that is being
 * killed stops.
 *
 * Note what this is *not*. It describes elapsed time that has already happened; it
 * never promises when the next attempt will be. It cannot: the recovery bound is
 * one watchdog period, and Doze stretches a watchdog period by an amount nobody
 * can name. A screen that said "retrying in 12 minutes" would be lying on any
 * phone that was asleep.
 */
enum class SyncRecency {

    /** No successful exchange has ever been recorded on this install. */
    NeverSynced,

    /** Within the last couple of windows. Nothing to see. */
    Recent,

    /**
     * Several windows missed. Ordinary if the watch has been off the wrist, out of
     * range, or flat — worth showing, not worth alarming about.
     */
    Overdue,

    /**
     * Long enough that "the watch happened to be away" stops being the likely
     * explanation and "something on this phone stopped the app" starts being it.
     */
    Stalled,
}

/**
 * Two windows of grace, six before it counts as stalled.
 *
 * Both are judgements rather than contract, so they live here and not in
 * [WatchProtocol]: the interval they are multiples of is the contract, and it is
 * mirrored in exactly one place (§8). Two, because missing a single window is
 * unremarkable — the watch may have been out of range for one hour of the day.
 * Six, because a watch that is worn and a phone that is carried give six chances
 * in six hours, and missing all of them is the shape of a killed process rather
 * than of bad luck.
 */
private const val OVERDUE_WINDOWS = 2L
private const val STALLED_WINDOWS = 6L

/**
 * @param secondsSinceLastSuccess from [HealthSnapshot.secondsSinceLastSuccess],
 *   which is null when there has never been a success and is already clamped at
 *   zero against a wall clock that jumped backwards.
 */
fun syncRecencyFor(secondsSinceLastSuccess: Long?): SyncRecency {
    val elapsed = secondsSinceLastSuccess ?: return SyncRecency.NeverSynced
    val window = WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS
    return when {
        // Negative would mean a caller that skipped the clamp. Treated as "just
        // now" rather than rejected: a clock that jumped is not a reason to tell
        // the user their phone is killing the app.
        elapsed <= OVERDUE_WINDOWS * window -> SyncRecency.Recent
        elapsed <= STALLED_WINDOWS * window -> SyncRecency.Overdue
        else -> SyncRecency.Stalled
    }
}

/**
 * Which vendor's background-killing settings the user has to go and change.
 *
 * The list is `docs/background-execution.md` §2's, and no more: Xiaomi/MIUI,
 * Huawei/EMUI, Samsung, and the BBK group (OnePlus/Oppo/Vivo), which share one
 * skin lineage and one "auto-launch" screen. Everything else gets
 * [Generic] — advice phrased in terms of what to look for rather than where,
 * which is the honest answer for a vendor nobody here has tested.
 *
 * Matching on `Build.MANUFACTURER` is a heuristic and is treated as one: the worst
 * outcome of a wrong guess is a paragraph of advice that does not match the
 * phone's menus, which is why nothing in the app *behaves* differently based on
 * this value.
 */
enum class OemBatteryAdvice {
    Xiaomi,
    Huawei,
    Samsung,
    BbkGroup,
    Generic,
}

/** @param manufacturer `Build.MANUFACTURER`, in whatever case the vendor shipped. */
fun oemBatteryAdviceFor(manufacturer: String?): OemBatteryAdvice {
    // lowercase() with no locale uses the default one, and in a Turkish locale
    // "XIAOMI".lowercase() is "xıaomi" — which matches nothing. The vendor strings
    // are ASCII, so the ASCII-only fold below is both correct and locale-proof.
    val name = manufacturer?.map { if (it in 'A'..'Z') it + ('a' - 'A') else it }?.joinToString("")
        ?: return OemBatteryAdvice.Generic
    return when (name) {
        "xiaomi" -> OemBatteryAdvice.Xiaomi
        "huawei" -> OemBatteryAdvice.Huawei
        "samsung" -> OemBatteryAdvice.Samsung
        "oneplus", "oppo", "vivo" -> OemBatteryAdvice.BbkGroup
        else -> OemBatteryAdvice.Generic
    }
}
