package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The decision behind "start the service" and "schedule the watchdog".
 *
 * None of this is observable on a JVM in its natural habitat — a real boot, a
 * real direct-boot window, a real OEM task killer — which is exactly why it is a
 * pure function of four observations rather than four `if`s spread across a
 * receiver, a worker and a service.
 */
class SupervisionTest {

    private fun plan(
        trigger: StartupTrigger,
        watchPaired: Boolean = true,
        serviceRunning: Boolean = false,
        userUnlocked: Boolean = true,
    ) = planStartup(trigger, watchPaired, serviceRunning, userUnlocked)

    // ── Starting the service ─────────────────────────────────────────────────

    @Test
    fun `every boot-time trigger starts the service when a watch is paired and nothing is running`() {
        // All three broadcasts, not two. A package update that did not restart the
        // service would leave the user unprotected until their next reboot, which
        // could be months.
        val triggers = listOf(
            StartupTrigger.BootCompleted,
            StartupTrigger.LockedBootCompleted,
            StartupTrigger.PackageReplaced,
            StartupTrigger.Watchdog,
        )
        for (trigger in triggers) {
            val plan = plan(trigger)
            assertTrue(plan.startService, "$trigger")
            assertNull(plan.skipped, "$trigger")
        }
    }

    @Test
    fun `the watchdog does not start a service that is already running`() {
        val plan = plan(StartupTrigger.Watchdog, serviceRunning = true)
        assertFalse(plan.startService)
        assertEquals(StartupSkipReason.ServiceAlreadyRunning, plan.skipped)
    }

    @Test
    fun `nothing starts the service while no watch is paired`() {
        for (trigger in StartupTrigger.entries) {
            val plan = plan(trigger, watchPaired = false)
            assertFalse(plan.startService, "$trigger")
            assertEquals(StartupSkipReason.NoWatchPaired, plan.skipped, "$trigger")
        }
    }

    @Test
    fun `an unpaired app is told so even while the service is still running`() {
        // This is the service asking about itself after an association was
        // dropped. "You are already running" would be a tautology; "there is no
        // watch" is the verdict it acts on by stopping.
        val plan = plan(StartupTrigger.ServiceStarted, watchPaired = false, serviceRunning = true)
        assertEquals(StartupSkipReason.NoWatchPaired, plan.skipped)
    }

    @Test
    fun `the service asking about itself is never told to start itself`() {
        val plan = plan(StartupTrigger.ServiceStarted, serviceRunning = true)
        assertFalse(plan.startService)
        assertEquals(StartupSkipReason.ServiceAlreadyRunning, plan.skipped)
    }

    // ── Refreshing a service that is already up ──────────────────────────────

    @Test
    fun `the watchdog pokes a running service so a latched permission block can clear`() {
        // The failure this exists to prevent: PermissionRevoked has no broadcast
        // behind it, so a machine parked in Blocked(permissionMissing) has no
        // reachable exit while the process survives. The user grants the
        // permission in Settings — which does not restart the process — and the
        // app sits there with the permission granted and a notification still
        // asking for it. That is a terminal error state, and Law 2 forbids one.
        val plan = plan(StartupTrigger.Watchdog, serviceRunning = true)
        assertTrue(plan.refreshRunningService)
        assertFalse(plan.startService, "the service is already up; this is a poke, not a start")
        assertEquals(StartupSkipReason.ServiceAlreadyRunning, plan.skipped)
    }

    @Test
    fun `nothing but the watchdog pokes a running service`() {
        // The boot broadcasts have nothing to refresh — a boot has no running
        // service — and the service asking about itself has just observed
        // everything there is to observe.
        for (trigger in StartupTrigger.entries - StartupTrigger.Watchdog) {
            assertFalse(plan(trigger, serviceRunning = true).refreshRunningService, "$trigger")
        }
    }

    @Test
    fun `a service that is not running is started, not poked`() {
        val plan = plan(StartupTrigger.Watchdog, serviceRunning = false)
        assertTrue(plan.startService)
        assertFalse(plan.refreshRunningService)
    }

    @Test
    fun `an unpaired app is never poked`() {
        // Refreshing would only make it re-observe its way to the same verdict,
        // and it stops itself for that verdict anyway.
        assertFalse(plan(StartupTrigger.Watchdog, watchPaired = false, serviceRunning = true).refreshRunningService)
    }

    // ── Scheduling the watchdog ──────────────────────────────────────────────

    @Test
    fun `the watchdog is never enqueued before the first unlock`() {
        // WorkManager's queue lives in credential-encrypted storage. Touching it
        // from a LOCKED_BOOT_COMPLETED receiver throws, and it throws inside a
        // system broadcast — the worst place in the app to learn that.
        for (trigger in StartupTrigger.entries) {
            assertFalse(plan(trigger, userUnlocked = false).enqueueWatchdog, "$trigger")
        }
    }

    @Test
    fun `a device with no lock screen enqueues the watchdog during direct boot`() {
        // isUserUnlocked() is true from the start when there is no credential, so
        // the answer must come from the observation and not from the trigger.
        val plan = plan(StartupTrigger.LockedBootCompleted, userUnlocked = true)
        assertTrue(plan.enqueueWatchdog)
    }

    @Test
    fun `the watchdog does not re-enqueue itself`() {
        assertFalse(plan(StartupTrigger.Watchdog, userUnlocked = true).enqueueWatchdog)
    }

    @Test
    fun `boot, package replacement and the service coming up all enqueue the watchdog`() {
        // A fresh install never sees BOOT_COMPLETED or MY_PACKAGE_REPLACED, so
        // ServiceStarted is the only trigger that puts the watchdog in place on
        // day one. Losing it would mean no watchdog until the first reboot.
        val triggers = listOf(
            StartupTrigger.BootCompleted,
            StartupTrigger.PackageReplaced,
            StartupTrigger.ServiceStarted,
        )
        for (trigger in triggers) {
            assertTrue(plan(trigger).enqueueWatchdog, "$trigger")
        }
    }

    @Test
    fun `the watchdog is scheduled even with no watch paired`() {
        // It has to be already running on the day the user finally associates one,
        // otherwise the first service death after pairing is permanent.
        assertTrue(plan(StartupTrigger.BootCompleted, watchPaired = false).enqueueWatchdog)
    }

    // ── The whole input space ────────────────────────────────────────────────

    @Test
    fun `startService and skipped are exactly each other's inverse, over every input`() {
        var cases = 0
        for (trigger in StartupTrigger.entries) {
            for (watchPaired in listOf(false, true)) {
                for (serviceRunning in listOf(false, true)) {
                    for (userUnlocked in listOf(false, true)) {
                        val plan = planStartup(trigger, watchPaired, serviceRunning, userUnlocked)
                        val label = "$trigger paired=$watchPaired running=$serviceRunning unlocked=$userUnlocked"
                        assertEquals(plan.skipped == null, plan.startService, label)
                        // The two things a start must never be: pointless, or a
                        // second copy of a service that is already up.
                        if (plan.startService) {
                            assertTrue(watchPaired, label)
                            assertFalse(serviceRunning, label)
                        }
                        // Starting and poking are the same effect for two
                        // opposite reasons, so they can never both be asked for.
                        assertFalse(plan.startService && plan.refreshRunningService, label)
                        if (plan.refreshRunningService) {
                            assertTrue(serviceRunning, label)
                            assertTrue(watchPaired, label)
                        }
                        cases++
                    }
                }
            }
        }
        assertEquals(StartupTrigger.entries.size * 8, cases)
    }
}
