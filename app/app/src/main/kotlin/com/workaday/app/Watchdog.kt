package com.workaday.app

import android.content.Context
import android.util.Log
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.PeriodicWorkRequest
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import com.workaday.core.StartupTrigger
import com.workaday.core.planStartup
import java.util.concurrent.TimeUnit

/**
 * Law 1's watchdog: "`START_STICKY`, plus a watchdog that does not trust it."
 *
 * `docs/background-execution.md` §2 is blunt about why — `START_STICKY` is not
 * honoured by every OEM, and the vendors that ignore it are the same ones whose
 * battery managers kill background services outright. A periodic job that checks
 * and restarts is the only thing standing between "the app stopped a month ago"
 * and the user noticing.
 */
internal object Watchdog {

    /**
     * Unique, so re-enqueueing on every boot and every package replacement adds
     * one job rather than one job per boot.
     */
    private const val UNIQUE_NAME = "workaday-service-watchdog"

    /** WorkManager's floor. Asking for less silently gets you this anyway. */
    private const val INTERVAL_MINUTES = 15L

    /**
     * Blocking — WorkManager touches its database. Call off the main thread, and
     * only when `planStartup` says the user is unlocked: this database lives in
     * credential-encrypted storage and is not reachable during direct boot.
     */
    fun enqueue(context: Context) {
        val request = PeriodicWorkRequest.Builder(
            WatchdogWorker::class.java,
            INTERVAL_MINUTES,
            TimeUnit.MINUTES,
        ).build()
        try {
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                UNIQUE_NAME,
                // UPDATE rather than KEEP: KEEP would pin the very first
                // definition of this job forever, so a later change to the
                // interval or the worker would never take effect on a phone that
                // had already run it. UPDATE keeps the existing schedule and
                // replaces the specification.
                ExistingPeriodicWorkPolicy.UPDATE,
                request,
            )
        } catch (e: IllegalStateException) {
            // WorkManager initialises itself from an androidx.startup provider
            // that is not direct-boot aware, so a process that started before the
            // first unlock can reach here with WorkManager never initialised.
            //
            // Handled rather than fatal, and the asymmetry is the point: the
            // foreground service is the primary mechanism and is already running,
            // while this is its backup. Throwing from here would kill a
            // BroadcastReceiver's worker thread — or the service — to punish the
            // absence of the thing that exists to recover from exactly that.
            // Every later trigger (USER_UNLOCKED, BOOT_COMPLETED, a package
            // replacement, a service start) tries again.
            Log.e(LOG_TAG, "WorkManager unavailable; watchdog not scheduled this pass", e)
        }
    }
}

/**
 * Checks whether the service is running and starts it if it is not.
 *
 * Deliberately thin. Whether to start is [planStartup]'s answer, and the
 * checklist's "still actually checks whether the service is running rather than
 * assuming" is [WatchLinkService.isRunning] — a flag the service itself sets,
 * which reads false both when the service was stopped and when the whole process
 * was killed and WorkManager had to start a fresh one to run this.
 *
 * A plain [Worker]: `doWork` already runs on a background thread, so the disk
 * reads here are free and no coroutine machinery is needed.
 */
internal class WatchdogWorker(
    context: Context,
    parameters: WorkerParameters,
) : Worker(context, parameters) {

    override fun doWork(): Result {
        val context = applicationContext
        val plan = planStartup(
            trigger = StartupTrigger.Watchdog,
            watchPaired = WatchStore.open(context).watchAddress != null,
            serviceRunning = WatchLinkService.isRunning,
            userUnlocked = isUserUnlocked(context),
        )
        logInfo("watchdog: $plan")
        // Two reasons to call the same thing. `startService` means it is gone and
        // must come back; `refreshRunningService` means it is up but may be
        // parked in Blocked(permissionMissing), which has no broadcast behind it
        // and so cannot notice on its own that the user granted the permission.
        // startForegroundService on a running service just delivers another
        // onStartCommand, and that path is idempotent.
        if (plan.startService || plan.refreshRunningService) WatchLinkService.start(context)
        // Always success. A retrying watchdog would be a second, unbounded retry
        // schedule sitting next to PROTOCOL.md §5.2's — and there is nothing here
        // that a retry in thirty seconds would do better than the next period.
        return Result.success()
    }
}
