package com.workaday.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import com.workaday.core.RestingSummary
import com.workaday.core.ServiceNotice

/**
 * The foreground service's notification.
 *
 * Pure presentation: [ServiceNotice] and [RestingSummary] are both decided in
 * `core/` — the first from the connection state, the second from the two records
 * the app persists — and everything here is the mapping from those decisions to
 * strings, to pixels, and to a `Notification` object. Every `when` below is
 * exhaustive over a type `core/` owns; there is no `if` in this file that changes
 * what the app does.
 *
 * **How the "how long ago" stays true.** The notification is re-posted only when
 * something changes, and at rest nothing changes for an hour at a time — so a
 * pre-formatted "4 minutes ago" would still be sitting there, still saying four, an
 * hour after it became false. That is Law 2's failure with the volume turned down:
 * an app that looks fine while saying something untrue. So no elapsed time is
 * formatted here at all. [RestingSummary] carries the *instant* of the last sync,
 * this hands it to `setWhen` + `setUsesChronometer`, and SystemUI counts it up
 * itself. It costs the app nothing — the counting is a view in SystemUI's process,
 * ticking only while the shade is actually on screen — so Law 1's power budget pays
 * for a periodic re-post it does not have to schedule, and cannot be stretched by
 * Doze into telling a lie.
 *
 * Built with the framework's own `Notification.Builder` rather than
 * `NotificationCompat`: `minSdk` is 31, so the compat layer would buy nothing and
 * cost a dependency.
 */
internal object ServiceNotification {

    /** Any non-zero id. There is exactly one notification in this app. */
    const val ID = 1

    private const val CHANNEL_ID = "workaday-link"

    /**
     * Cheap and idempotent, so it runs on every service creation rather than
     * being remembered somewhere that a process death would forget.
     */
    fun ensureChannel(context: Context, manager: NotificationManager) {
        val channel = NotificationChannel(
            CHANNEL_ID,
            context.getString(R.string.channel_name),
            // LOW: no sound, no heads-up. This notification is on for months; it
            // is a status light, not an alert.
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = context.getString(R.string.channel_description)
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    /**
     * @param summary shown only under [ServiceNotice.Waiting], which is the state
     *   the app spends nearly all of its life in. The other five notices are about
     *   the app's own situation and say one thing each; the watch's battery is
     *   still drawn into the icon under all of them, because it is the last
     *   measurement either way and does not stop being true because Bluetooth was
     *   switched off.
     */
    fun build(context: Context, notice: ServiceNotice, summary: RestingSummary): Notification {
        val builder = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(WatchGaugeIcon.of(context, summary.gauge))
            .setContentTitle(context.getString(R.string.app_name))
            .setOngoing(true)
            .setContentIntent(diagnosticsIntent(context))
        // No `else`: a seventh notice has to be given words here or the build
        // fails, which is the same guarantee the old `textFor` gave.
        return when (notice) {
            ServiceNotice.Waiting -> builder.resting(context, summary)
            ServiceNotice.Starting -> builder.plain(context, R.string.notice_starting)
            ServiceNotice.Exchanging -> builder.plain(context, R.string.notice_exchanging)
            ServiceNotice.RetryingSoon -> builder.plain(context, R.string.notice_retrying)
            ServiceNotice.BluetoothOff -> builder.plain(context, R.string.notice_bluetooth_off)
            ServiceNotice.PermissionMissing -> builder.plain(context, R.string.notice_permission_missing)
        }.build()
    }

    /** One of the five notices about the app's own situation: a line, and no clock. */
    private fun Notification.Builder.plain(context: Context, text: Int): Notification.Builder =
        noSyncToDate().setContentText(context.getString(text))

    /**
     * The resting caption: what the watch said its battery was, and — as a live
     * counter rather than as words — how long ago it last synced.
     *
     * The counter goes in the header, labelled by `setSubText`, so the line reads
     * "Workaday · Last sync · 12:04". The content text below it is left to the one
     * fact that is a measurement rather than an elapsed time.
     */
    private fun Notification.Builder.resting(
        context: Context,
        summary: RestingSummary,
    ): Notification.Builder = when (summary) {
        RestingSummary.NothingYet -> noSyncToDate()
            .setContentText(context.getString(R.string.notice_waiting_nothing_yet))

        is RestingSummary.BatteryOnly -> noSyncToDate()
            .setContentText(
                context.getString(R.string.notice_waiting_battery_unsynced, summary.batteryPercent),
            )

        is RestingSummary.SyncedOnly -> countingFrom(context, summary.syncedAtUtcEpochSeconds)
            .setContentText(context.getString(R.string.notice_waiting_no_battery))

        is RestingSummary.BatteryAndSync -> countingFrom(context, summary.syncedAtUtcEpochSeconds)
            .setContentText(
                context.getString(R.string.notice_waiting_battery, summary.batteryPercent),
            )
    }

    /**
     * The platform's own elapsed-time counter, started at the last sync.
     *
     * `setWhen` takes a wall-clock instant in milliseconds and SystemUI converts it
     * to its own monotonic base once, at post time — so a clock that jumps
     * afterwards moves nothing on screen.
     */
    private fun Notification.Builder.countingFrom(
        context: Context,
        syncedAtUtcEpochSeconds: Long,
    ): Notification.Builder = setShowWhen(true)
        .setWhen(syncedAtUtcEpochSeconds * 1_000L)
        .setUsesChronometer(true)
        .setSubText(context.getString(R.string.notice_last_sync_label))

    /** No instant to count from, so nothing in the header pretends there is one. */
    private fun Notification.Builder.noSyncToDate(): Notification.Builder =
        setShowWhen(false).setUsesChronometer(false)

    /**
     * Tapping it opens the diagnostic window. Law 1: that window is a view onto
     * the service and nothing the service depends on — this is the only direction
     * the arrow ever points.
     */
    private fun diagnosticsIntent(context: Context): PendingIntent =
        PendingIntent.getActivity(
            context,
            0,
            Intent(context, DiagnosticsActivity::class.java)
                .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
}
