package com.workaday.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import com.workaday.core.ServiceNotice

/**
 * The foreground service's notification.
 *
 * Pure presentation: [ServiceNotice] is decided in `core/` from the connection
 * state, and everything here is the mapping from that decision to a string and a
 * `Notification` object. There is no `if` in this file that changes what the app
 * does.
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

    fun build(context: Context, notice: ServiceNotice): Notification =
        Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(context.getString(textFor(notice)))
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(diagnosticsIntent(context))
            .build()

    private fun textFor(notice: ServiceNotice): Int = when (notice) {
        ServiceNotice.Starting -> R.string.notice_starting
        ServiceNotice.Waiting -> R.string.notice_waiting
        ServiceNotice.Exchanging -> R.string.notice_exchanging
        ServiceNotice.RetryingSoon -> R.string.notice_retrying
        ServiceNotice.BluetoothOff -> R.string.notice_bluetooth_off
        ServiceNotice.PermissionMissing -> R.string.notice_permission_missing
    }

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
