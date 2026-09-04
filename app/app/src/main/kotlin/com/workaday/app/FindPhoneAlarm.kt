package com.workaday.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.SystemClock
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.Settings
import android.util.Log
import com.workaday.core.FindAlarm
import java.io.IOException

/**
 * The phone making itself heard — `Action.StartFindAlarm` and `Action.StopFindAlarm`
 * performed (PROTOCOL.md §4.1).
 *
 * Three effects, all stopped together on every path, two of them always on:
 *
 * - **Vibration**, a repeating pulse on the ALARM usage, for a phone face-down on
 *   a sofa cushion. On from the first moment: `PROTOCOL.md` §4.1 makes the
 *   vibration-only alarm the default a search starts in.
 * - **The alarm notification**, high importance with a full-screen intent, which
 *   is Android's mechanism for putting a screen in front of a locked or sleeping
 *   phone: on a phone that is off it launches [FindPhoneActivity] over the lock
 *   screen with the display turned on; on a phone in use it is a heads-up with a
 *   Stop action. Both routes end in the same `Action` back to the machine.
 * - **Sound**, only when the watch asks for it — the Status's `FIND_SOUND` at the
 *   start, a `FindMode` notify mid-link, either way the wearer's Menu press — from
 *   a `MediaPlayer` looping the device's alarm tone on the ALARM audio usage, with
 *   the alarm stream turned up to its maximum while the tone plays and the
 *   player's own volume ramped from quiet to full along [FindAlarm]'s curve, the
 *   ramp starting afresh each time the tone is switched on. ALARM rather than
 *   NOTIFICATION or RING because that is the usage Do Not Disturb's default
 *   "priority only" mode lets through; a mode that blocks alarms — "total
 *   silence" — mutes this too, and no app can override that without
 *   notification-policy access the user would have to grant separately. Stated in
 *   `PROTOCOL.md` §6.2 as a platform limit rather than papered over.
 *
 * **No decision here.** Whether to ring, in which mode, for how long, and when to
 * stop are the state machine's; the volume curve is `core/`'s. This class turns a
 * start into platform calls and a stop into their undo calls, and the `if`s it
 * has — `isRinging`, and whether the tone is already what was asked for — are
 * idempotence, so a second start, stop or identical mode is a no-op rather than a
 * second player.
 *
 * Every method runs on the link's serial thread. `MediaPlayer.prepare()` blocks
 * for the length of a small file read, which is fine there and would not be on
 * the main thread.
 */
internal class FindPhoneAlarm(
    private val context: Context,
    private val handler: Handler,
) {
    private val audio: AudioManager? = context.getSystemService(AudioManager::class.java)
    private val vibrator: Vibrator? =
        context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    private val notifications: NotificationManager? =
        context.getSystemService(NotificationManager::class.java)

    private var player: MediaPlayer? = null

    /** Whether the tone has been asked for — what [setSound] compares against. */
    private var soundOn = false

    /** The alarm stream's volume before we raised it, restored when the tone stops. */
    private var previousAlarmVolume: Int? = null

    /** When the tone was last switched on: the ramp's origin. */
    private var toneStartedAtMillis = 0L

    /**
     * Re-applies [FindAlarm.volumeAt] every [FindAlarm.STEP_MS] until the curve
     * reaches full. Driven by the clock rather than by counting steps, so a step
     * that fires late lands on the right volume rather than one step behind.
     */
    private val ramp = object : Runnable {
        override fun run() {
            val current = player ?: return
            val volume = FindAlarm.volumeAt(SystemClock.elapsedRealtime() - toneStartedAtMillis)
            try {
                current.setVolume(volume, volume)
            } catch (e: IllegalStateException) {
                // Released under us — stopTone() ran. Nothing more to do.
                return
            }
            if (volume < 1f) handler.postDelayed(this, FindAlarm.STEP_MS)
        }
    }

    /**
     * @param sound whether the tone plays from the start, alongside the vibration
     *   that always does. `PROTOCOL.md` §4.1: false unless the wearer pressed
     *   Menu before the phone connected.
     */
    fun start(sound: Boolean) {
        if (isRinging) return
        isRinging = true
        vibrate()
        notifications?.notify(NOTIFICATION_ID, notification())
        if (sound) startTone()
        logInfo("find phone: alarm started (${if (sound) "tone and vibration" else "vibration only"})")
    }

    /**
     * The mid-link mode change (§3.3 FindMode): add the tone or take it away,
     * vibration untouched either way. A mode that matches what is already playing
     * is a no-op, so a repeated notify cannot restart the ramp.
     */
    fun setSound(sound: Boolean) {
        if (!isRinging || sound == soundOn) return
        if (sound) startTone() else stopTone()
        logInfo("find phone: tone ${if (sound) "on" else "off"}")
    }

    /**
     * Idempotent, and it runs on every exit from the ringing states as well as on
     * service shutdown — Law 2's "on every path including cancellation".
     */
    fun stop() {
        if (!isRinging) return
        isRinging = false
        stopTone()
        vibrator?.cancel()
        notifications?.cancel(NOTIFICATION_ID)
        // Tells FindPhoneActivity, if it is up, to go away. Package-scoped: nothing
        // outside this app has any business hearing it.
        context.sendBroadcast(Intent(ACTION_FIND_ENDED).setPackage(context.packageName))
        logInfo("find phone: alarm stopped")
    }

    // ── sound ────────────────────────────────────────────────────────────────

    private fun startTone() {
        soundOn = true
        toneStartedAtMillis = SystemClock.elapsedRealtime()
        raiseAlarmStream()
        player = openPlayer()
        handler.post(ramp)
        if (player == null) Log.w(LOG_TAG, "find phone: tone asked for but no alarm tone could be opened")
    }

    private fun stopTone() {
        soundOn = false
        handler.removeCallbacks(ramp)
        player?.let { current ->
            try {
                current.stop()
            } catch (e: IllegalStateException) {
                // Never started — the tone failed to open. release() is still due.
            }
            current.release()
        }
        player = null
        restoreAlarmStream()
    }

    private fun raiseAlarmStream() {
        val manager = audio ?: return
        try {
            previousAlarmVolume = manager.getStreamVolume(AudioManager.STREAM_ALARM)
            manager.setStreamVolume(
                AudioManager.STREAM_ALARM,
                manager.getStreamMaxVolume(AudioManager.STREAM_ALARM),
                0,
            )
        } catch (e: SecurityException) {
            // A Do Not Disturb mode that refuses volume changes. The tone plays at
            // whatever the stream is set to — and may be muted, which §6.2 records
            // as the platform limit it is.
            Log.w(LOG_TAG, "could not raise the alarm volume", e)
            previousAlarmVolume = null
        }
    }

    private fun restoreAlarmStream() {
        val manager = audio ?: return
        val previous = previousAlarmVolume ?: return
        previousAlarmVolume = null
        try {
            manager.setStreamVolume(AudioManager.STREAM_ALARM, previous, 0)
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "could not restore the alarm volume", e)
        }
    }

    /**
     * The device's alarm tone, or the ringtone if there is none, looping on the
     * ALARM usage. Null when nothing would open — a phone with no sounds at all —
     * in which case vibration and the screen carry the alarm on their own.
     */
    private fun openPlayer(): MediaPlayer? {
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_ALARM)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        for (uri in toneCandidates()) {
            val candidate = MediaPlayer()
            try {
                candidate.setAudioAttributes(attributes)
                candidate.setDataSource(context, uri)
                candidate.isLooping = true
                val start = FindAlarm.volumeAt(0)
                candidate.setVolume(start, start)
                candidate.prepare()
                candidate.start()
                return candidate
            } catch (e: IOException) {
                Log.w(LOG_TAG, "alarm tone $uri would not open", e)
            } catch (e: IllegalArgumentException) {
                Log.w(LOG_TAG, "alarm tone $uri is not a usable source", e)
            } catch (e: IllegalStateException) {
                Log.w(LOG_TAG, "alarm tone $uri would not play", e)
            } catch (e: SecurityException) {
                Log.w(LOG_TAG, "alarm tone $uri is not readable", e)
            }
            candidate.release()
        }
        Log.w(LOG_TAG, "no alarm tone could be opened; ringing by vibration and screen only")
        return null
    }

    private fun toneCandidates(): List<Uri> = listOfNotNull(
        RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_ALARM),
        Settings.System.DEFAULT_ALARM_ALERT_URI,
        RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE),
        Settings.System.DEFAULT_RINGTONE_URI,
    ).distinct()

    // ── vibration ────────────────────────────────────────────────────────────

    private fun vibrate() {
        val motor = vibrator ?: return
        if (!motor.hasVibrator()) return
        // On for most of a second, off for half, forever (repeat from index 0).
        val effect = VibrationEffect.createWaveform(
            longArrayOf(0, 700, 500),
            intArrayOf(0, VibrationEffect.DEFAULT_AMPLITUDE, 0),
            0,
        )
        try {
            // The version branch is *how*: the attributes type changed in 33, and
            // minSdk is 31. Both name the ALARM usage for the same reason the sound
            // does.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                motor.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_ALARM))
            } else {
                @Suppress("DEPRECATION")
                motor.vibrate(effect, AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).build())
            }
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "vibration denied", e)
        }
    }

    // ── the alarm notification ───────────────────────────────────────────────

    private fun notification(): Notification {
        val screen = PendingIntent.getActivity(
            context,
            REQUEST_SCREEN,
            Intent(context, FindPhoneActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getForegroundService(
            context,
            REQUEST_STOP,
            WatchLinkService.dismissFindIntent(context),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(Icon.createWithResource(context, android.R.drawable.ic_lock_idle_alarm))
            .setContentTitle(context.getString(R.string.find_title))
            .setContentText(context.getString(R.string.find_text))
            .setCategory(Notification.CATEGORY_ALARM)
            // On the lock screen in full: the whole point is to be found.
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(screen)
            // Android's route to a screen on a locked, sleeping phone. If the
            // USE_FULL_SCREEN_INTENT permission has been revoked in Settings the
            // system downgrades this to a heads-up with the Stop action, and the
            // sound and vibration are unaffected.
            .setFullScreenIntent(screen, true)
            .addAction(
                Notification.Action.Builder(
                    Icon.createWithResource(context, android.R.drawable.ic_media_pause),
                    context.getString(R.string.find_stop),
                    stop,
                ).build(),
            )
            .build()
    }

    companion object {
        /** Distinct from ServiceNotification.ID: the two are on screen together. */
        const val NOTIFICATION_ID = 2

        private const val CHANNEL_ID = "workaday-find"
        private const val REQUEST_SCREEN = 2
        private const val REQUEST_STOP = 3

        /** Sent, package-scoped, when the alarm stops; FindPhoneActivity finishes on it. */
        const val ACTION_FIND_ENDED = "com.workaday.app.action.FIND_ENDED"

        /**
         * Whether the alarm is sounding **in this process** — the service's own
         * state, read by [FindPhoneActivity] the way `WatchLinkService.isRunning`
         * is read by the watchdog. Law 1: the Activity owns nothing; it looks.
         */
        @Volatile
        var isRinging: Boolean = false
            private set

        /**
         * High importance, silent: the sound is this class's own player on the
         * ALARM usage, not the notification's, so the channel makes no noise of
         * its own and the ramp is the only volume anyone hears.
         *
         * Cheap and idempotent, like the service channel; run on every service
         * creation. A channel's importance cannot be changed by the app once it
         * exists, which is why it is created right the first time.
         */
        fun ensureChannel(context: Context, manager: NotificationManager) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                context.getString(R.string.find_channel_name),
                NotificationManager.IMPORTANCE_HIGH,
            ).apply {
                description = context.getString(R.string.find_channel_description)
                setSound(null, null)
                enableVibration(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            }
            manager.createNotificationChannel(channel)
        }
    }
}
