package com.workaday.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.UserManager
import android.util.Log
import com.workaday.core.StartupTrigger
import com.workaday.core.planStartup

/**
 * Law 1: "It starts on boot. A receiver for `BOOT_COMPLETED`,
 * `LOCKED_BOOT_COMPLETED` and `MY_PACKAGE_REPLACED` starts the service. **All
 * three** — a package update must not silently leave the user unprotected until
 * the next reboot."
 *
 * `docs/background-execution.md` §1 is what makes this legal at all: receiving
 * one of those three broadcasts is on the short list of exemptions that let an
 * app start a foreground service from the background, and `connectedDevice` is
 * **not** on the list of types Android 15 forbids starting from `BOOT_COMPLETED`.
 * `dataSync` is, which is why Law 1 forbids changing the type.
 *
 * `LOCKED_BOOT_COMPLETED` only reaches direct-boot-aware components, so this
 * receiver and the service it starts are both `directBootAware` in the manifest —
 * and everything they read before the first unlock comes out of device-protected
 * storage (see [WatchStore]).
 *
 * There is no decision in here. [planStartup] takes four observations and returns
 * what to do; this class observes, performs, and logs.
 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val trigger = triggerFor(intent.action) ?: return
        val app = context.applicationContext

        // onReceive runs on the main thread and everything below it does disk
        // I/O — SharedPreferences, and WorkManager's database. goAsync() keeps the
        // broadcast alive while a thread does it properly.
        val pending = goAsync()
        Thread({
            try {
                act(app, trigger)
            } finally {
                // On every path, including a throw: a PendingResult that is never
                // finished holds the broadcast open until the system kills it.
                pending.finish()
            }
        }, "workaday-boot").start()
    }

    private fun act(context: Context, trigger: StartupTrigger) {
        val plan = planStartup(
            trigger = trigger,
            watchPaired = WatchStore.open(context).watchAddress != null,
            serviceRunning = WatchLinkService.isRunning,
            userUnlocked = isUserUnlocked(context),
        )
        logInfo("$trigger: $plan")
        if (plan.startService) WatchLinkService.start(context)
        if (plan.enqueueWatchdog) Watchdog.enqueue(context)
    }

    /**
     * Platform vocabulary to the machine's, and nothing else. An action this app
     * did not register for is not a case to handle — it is someone else's
     * broadcast arriving by mistake.
     */
    private fun triggerFor(action: String?): StartupTrigger? = when (action) {
        Intent.ACTION_BOOT_COMPLETED -> StartupTrigger.BootCompleted
        Intent.ACTION_LOCKED_BOOT_COMPLETED -> StartupTrigger.LockedBootCompleted
        Intent.ACTION_MY_PACKAGE_REPLACED -> StartupTrigger.PackageReplaced
        else -> {
            Log.w(LOG_TAG, "BootReceiver woken by an action it does not handle: $action")
            null
        }
    }
}

/**
 * `UserManager.isUserUnlocked()` — whether credential-encrypted storage is
 * readable yet.
 *
 * Observed rather than inferred from the broadcast: a device with no lock screen
 * is unlocked from the moment it boots, and an app that assumed otherwise would
 * never schedule its watchdog there.
 */
internal fun isUserUnlocked(context: Context): Boolean =
    context.getSystemService(UserManager::class.java).isUserUnlocked
