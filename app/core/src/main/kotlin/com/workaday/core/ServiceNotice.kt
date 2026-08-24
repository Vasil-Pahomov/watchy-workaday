package com.workaday.core

/**
 * What the foreground service's notification is telling the user right now.
 *
 * A foreground service must show a notification, and a notification that never
 * changes tells the user nothing about whether the app is working — which is the
 * failure Law 2 exists to refuse, only visible this time. So the text has to
 * follow the state, and "which text" is a decision, which puts it here rather
 * than in a `when` inside the `Service` (Law 3).
 *
 * Deliberately coarse. This is the always-on notification, not the diagnostic
 * screen: it says whether the app is fine, busy, retrying, or needs the user to
 * do something. The `result` codes, the health counters and the last sync time
 * belong to APP-4's diagnostic UI, which can afford detail.
 */
enum class ServiceNotice {

    /** The service is up but has not been told the world's state yet. */
    Starting,

    /**
     * The resting state, and by far the most common: a pending `autoConnect` is
     * with the Bluetooth controller and the app is doing nothing at all.
     */
    Waiting,

    /** The watch turned up and the PROTOCOL.md §4 exchange is in progress. */
    Exchanging,

    /** A §5.2 backoff is running. Nothing is wrong that waiting will not fix. */
    RetryingSoon,

    /** The adapter is off, or airplane mode is on — the two look identical. */
    BluetoothOff,

    /** The runtime Bluetooth permission is missing. The user has to grant it. */
    PermissionMissing,
}

/**
 * The notice for a state. Total, and a pure function of its argument.
 *
 * [ConnectionState.Blocked] can carry both reasons at once, and then
 * [ServiceNotice.PermissionMissing] wins: turning Bluetooth back on while the
 * permission is still revoked changes nothing the user can see, so naming the
 * adapter first would send them to fix the half that is not the problem.
 */
fun serviceNoticeFor(state: ConnectionState): ServiceNotice = when (state) {
    ConnectionState.Idle -> ServiceNotice.Starting
    ConnectionState.Armed -> ServiceNotice.Waiting

    // The pause after a successful sync, while the watch's window finishes. The
    // app is fine, idle, and about to be armed again - which is what Waiting
    // already means. A caption of its own would flicker once an hour and tell the
    // user about an implementation detail they cannot act on.
    is ConnectionState.Settling -> ServiceNotice.Waiting
    is ConnectionState.WaitingForRetry -> ServiceNotice.RetryingSoon
    is ConnectionState.Blocked ->
        if (state.permissionMissing) ServiceNotice.PermissionMissing else ServiceNotice.BluetoothOff

    // The four mid-exchange states are one notice on purpose. They last about a
    // second and a half between them (§5.3), and a notification that flickered
    // through four captions would be noise, not information.
    ConnectionState.Discovering,
    ConnectionState.EnablingNotifications,
    ConnectionState.WritingTime,
    ConnectionState.AwaitingStatus,
    -> ServiceNotice.Exchanging
}
