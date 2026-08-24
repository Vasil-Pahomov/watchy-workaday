package com.workaday.app

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.PowerManager
import android.os.SystemClock

/**
 * The machine's two timers are two different mechanisms, because they have two
 * different enemies.
 *
 * `Action.ArmOperationTimeout` guards a GATT operation that is outstanding right
 * now, for at most PROTOCOL.md §5.2's 15 s. `Action.ScheduleRetry` waits out a
 * backoff of up to 18 minutes on a phone that is probably asleep. A single
 * mechanism would be wrong for one of them: an alarm per GATT operation is four
 * wakeups an hour for nothing, and a posted callback for the backoff is the bug
 * `Action.ScheduleRetry`'s KDoc names by hand.
 */

/**
 * The per-operation timeout, plus the wakelock that makes it mean anything.
 *
 * `Handler.postDelayed` counts in `SystemClock.uptimeMillis()`, which **stops
 * while the phone is suspended**. Left at that, a watch that vanished mid-write
 * at 3 a.m. would leave the app sitting in `Discovering` with a GATT client open
 * and a timeout that never fires — not a crash, not a backoff, just an app that
 * is installed, looks fine, and has silently stopped talking to the watch. That
 * is the exact failure Law 2 refuses.
 *
 * So a partial wakelock is held for as long as an operation timeout is armed. It
 * buys a second thing that matters more: PROTOCOL.md §5.1 gives the watch a **12 s
 * absolute session cap**, measured in the watch's own real time. A phone that
 * suspends between two GATT callbacks spends that budget asleep and the window
 * closes with nothing synced. The wakelock is what makes the §4 exchange run at
 * wall-clock speed.
 *
 * Bounded three ways, because an unbounded wakelock on a phone is a battery fire:
 * it is only ever held while an operation timeout is armed (≤ 15 s by §5.2), it
 * is released on every arm, cancel, fire and shutdown, and it is acquired with a
 * kernel-side timeout so the OS drops it even if this class is the thing that
 * broke.
 *
 * Single-threaded by construction: every method runs on the link's serial thread,
 * which is also the thread [Handler] posts to.
 */
internal class OperationTimer(
    private val handler: Handler,
    powerManager: PowerManager,
    /** Called on the serial thread with the token of the timer that fired. */
    private val onTimeout: (Long) -> Unit,
) {
    private val wakeLock: PowerManager.WakeLock =
        powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKELOCK_TAG).apply {
            // Not reference counted: this class holds at most one, and a counted
            // lock that gets acquired twice needs releasing twice, which is how
            // wakelocks get leaked.
            setReferenceCounted(false)
        }

    /** The token of the armed timer, or null when nothing is armed. */
    private var armedToken: Long? = null

    private val fire = Runnable {
        val token = armedToken
        // Clear before dispatching: onTimeout re-enters the machine, which will
        // arm the next timer, and it must not be undone by this one's bookkeeping.
        armedToken = null
        releaseWakeLock()
        if (token != null) onTimeout(token)
    }

    /**
     * Replace whatever is armed with a timer for [token].
     *
     * The machine guarantees at most one is ever outstanding; this enforces it
     * anyway, because a second live timeout would tear down a healthy connection.
     */
    fun arm(timeoutMillis: Long, token: Long) {
        cancel()
        armedToken = token
        // The lock outlives the timeout by a margin so the event it delivers is
        // still processed while the CPU is up. The kernel drops it regardless
        // once that expires.
        wakeLock.acquire(timeoutMillis + WAKELOCK_MARGIN_MS)
        handler.postDelayed(fire, timeoutMillis)
    }

    /**
     * Idempotent. A firing that had already been dequeued when this ran is
     * dropped by [fire]'s null check, and the machine's token check catches it a
     * second time.
     */
    fun cancel() {
        handler.removeCallbacks(fire)
        armedToken = null
        releaseWakeLock()
    }

    private fun releaseWakeLock() {
        // isHeld, because release() on an unheld non-counted lock throws.
        if (wakeLock.isHeld) wakeLock.release()
    }

    private companion object {
        const val WAKELOCK_TAG = "workaday:gatt-exchange"
        const val WAKELOCK_MARGIN_MS = 2_000L
    }
}

/**
 * `Action.ScheduleRetry`, honoured by `AlarmManager` exactly as its KDoc requires.
 *
 * `setAndAllowWhileIdle` with an `ELAPSED_REALTIME_WAKEUP` trigger: it fires in
 * Doze, it counts in a clock that keeps running while the phone is suspended, and
 * it wakes the device to deliver. `Handler.postDelayed` would be stretched by a
 * dozing phone past the watch's next hourly window, and — the reason that is
 * worth an alarm — no unit test on any machine would catch it.
 *
 * Not `setExact*`: that needs `SCHEDULE_EXACT_ALARM`, which
 * `docs/background-execution.md` §1 lists among the exemptions that are "not
 * appropriate here". §5.2's delays are 30 s to 18 min against a window that opens
 * hourly, so a few minutes of alarm slop costs nothing.
 *
 * The alarm is a **broadcast** to a receiver the service registers at runtime, not
 * a service start. If the service is alive the token reaches it with no start
 * restriction to negotiate at all; if the process is gone there is nothing to
 * deliver to, and the watchdog is what brings it back — where the machine arms
 * immediately rather than resuming a backoff that has already been served.
 */
internal class RetryAlarm(
    private val context: Context,
    private val alarmManager: AlarmManager,
) {

    fun schedule(delayMillis: Long, token: Long) {
        val pending = pendingIntent(token, PendingIntent.FLAG_UPDATE_CURRENT) ?: return
        alarmManager.setAndAllowWhileIdle(
            AlarmManager.ELAPSED_REALTIME_WAKEUP,
            SystemClock.elapsedRealtime() + delayMillis,
            pending,
        )
    }

    /** Idempotent — nothing pending means nothing to cancel. */
    fun cancel() {
        val pending = pendingIntent(token = 0L, PendingIntent.FLAG_NO_CREATE) ?: return
        alarmManager.cancel(pending)
        pending.cancel()
    }

    /**
     * One PendingIntent identity for the whole app, so a new retry replaces the
     * old one and [cancel] matches it.
     *
     * `Intent.filterEquals` ignores extras, so the token rides along without
     * making each alarm a different alarm — which is what we want, since only one
     * retry is ever pending. Immutable because the system is the only thing that
     * ever sends it, and API 31+ requires the flag to be stated either way.
     */
    private fun pendingIntent(token: Long, flags: Int): PendingIntent? {
        val intent = Intent(ACTION_RETRY)
            .setPackage(context.packageName)
            .putExtra(EXTRA_TOKEN, token)
        return PendingIntent.getBroadcast(
            context,
            REQUEST_CODE,
            intent,
            flags or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    companion object {
        const val ACTION_RETRY = "com.workaday.app.action.RETRY_TIMER"
        const val EXTRA_TOKEN = "com.workaday.app.extra.TIMER_TOKEN"

        /**
         * Not a valid token: the machine's counter starts at 1 and only ever
         * increases, so an alarm that somehow arrived without its extra is
         * rejected rather than mistaken for the live one.
         */
        const val NO_TOKEN = -1L

        private const val REQUEST_CODE = 1

        fun tokenFrom(intent: Intent): Long = intent.getLongExtra(EXTRA_TOKEN, NO_TOKEN)
    }
}
