package com.workaday.core

import com.workaday.core.protocol.WatchProtocol
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * "Does the app look correctly set up", and "does the background survival look
 * like it worked".
 *
 * **These tests exist here rather than in `app/` for one specific reason.** `app/`
 * builds its unit tests against the stubbed `android.jar` with
 * `isReturnDefaultValues = true`, and `PackageManager.PERMISSION_GRANTED` is `0` —
 * so a test over there that called `checkSelfPermission` would be handed `0`, read
 * it as *granted*, and pass no matter what the code did. An entire screen about
 * permissions is precisely the change that trap is set for. Every input below is a
 * plain boolean that no framework stub can answer on the code's behalf.
 */
class SetupTest {

    private fun observations(
        watchAssociated: Boolean = true,
        bluetoothPermissionGranted: Boolean = true,
        notificationPermissionGranted: Boolean = true,
        bluetoothAdapterOn: Boolean = true,
        ignoringBatteryOptimisations: Boolean = true,
    ) = SetupObservations(
        watchAssociated = watchAssociated,
        bluetoothPermissionGranted = bluetoothPermissionGranted,
        notificationPermissionGranted = notificationPermissionGranted,
        bluetoothAdapterOn = bluetoothAdapterOn,
        ignoringBatteryOptimisations = ignoringBatteryOptimisations,
    )

    // ── assessSetup ──────────────────────────────────────────────────────────

    @Test
    fun `everything observable in order is Ready with nothing to do`() {
        val assessment = assessSetup(observations())
        assertEquals(SetupVerdict.Ready, assessment.verdict)
        assertEquals(emptyList<SetupTask>(), assessment.tasks)
    }

    @Test
    fun `no association outranks every other complaint`() {
        // Law 1: the association is what buys the background exemptions and what
        // supplies the address, so without it nothing else on the list can matter.
        // Breaks if the verdict is computed from the first failing task in list
        // order rather than from the association first.
        val assessment = assessSetup(
            observations(
                watchAssociated = false,
                bluetoothPermissionGranted = false,
                bluetoothAdapterOn = false,
                ignoringBatteryOptimisations = false,
                notificationPermissionGranted = false,
            ),
        )
        assertEquals(SetupVerdict.NotPaired, assessment.verdict)
        assertEquals(SetupTask.AssociateWatch, assessment.tasks.first())
        assertEquals(SetupTask.entries.size, assessment.tasks.size, "everything wrong should list everything")
    }

    @Test
    fun `a missing Bluetooth permission blocks, it does not merely put at risk`() {
        val assessment = assessSetup(observations(bluetoothPermissionGranted = false))
        assertEquals(SetupVerdict.Blocked, assessment.verdict)
        assertEquals(listOf(SetupTask.GrantBluetoothPermission), assessment.tasks)
    }

    @Test
    fun `Bluetooth being off blocks`() {
        val assessment = assessSetup(observations(bluetoothAdapterOn = false))
        assertEquals(SetupVerdict.Blocked, assessment.verdict)
        assertEquals(listOf(SetupTask.TurnBluetoothOn), assessment.tasks)
    }

    @Test
    fun `no battery exemption is AtRisk rather than Blocked`() {
        // It works right now. What it will not survive is a week in a pocket, which
        // is a different claim and deserves a different word.
        val assessment = assessSetup(observations(ignoringBatteryOptimisations = false))
        assertEquals(SetupVerdict.AtRisk, assessment.verdict)
        assertEquals(listOf(SetupTask.ExemptFromBatteryOptimisation), assessment.tasks)
    }

    @Test
    fun `denied notifications are AtRisk, because an invisible service is the quiet failure`() {
        // Breaks if POST_NOTIFICATIONS is treated as cosmetic and left out of the
        // verdict. The service does keep running — but Law 2's refused failure mode
        // is an app that is installed, looks fine, and silently stopped, and an
        // invisible notification is exactly how the user fails to notice.
        val assessment = assessSetup(observations(notificationPermissionGranted = false))
        assertEquals(SetupVerdict.AtRisk, assessment.verdict)
        assertEquals(listOf(SetupTask.GrantNotificationPermission), assessment.tasks)
    }

    @Test
    fun `a blocking problem outranks an at-risk one`() {
        val assessment = assessSetup(
            observations(bluetoothAdapterOn = false, ignoringBatteryOptimisations = false),
        )
        assertEquals(SetupVerdict.Blocked, assessment.verdict)
        assertEquals(
            listOf(SetupTask.TurnBluetoothOn, SetupTask.ExemptFromBatteryOptimisation),
            assessment.tasks,
        )
    }

    @Test
    fun `tasks always come back in the same priority order`() {
        val assessment = assessSetup(
            observations(
                watchAssociated = false,
                bluetoothPermissionGranted = false,
                bluetoothAdapterOn = false,
                ignoringBatteryOptimisations = false,
                notificationPermissionGranted = false,
            ),
        )
        assertEquals(SetupTask.entries.toList(), assessment.tasks)
    }

    @Test
    fun `over every combination, Ready means nothing outstanding and vice versa`() {
        var cases = 0
        for (associated in listOf(false, true)) {
            for (permission in listOf(false, true)) {
                for (notifications in listOf(false, true)) {
                    for (adapter in listOf(false, true)) {
                        for (exempt in listOf(false, true)) {
                            val obs = observations(associated, permission, notifications, adapter, exempt)
                            val assessment = assessSetup(obs)
                            val label = "$obs"
                            assertEquals(
                                assessment.tasks.isEmpty(),
                                assessment.verdict == SetupVerdict.Ready,
                                label,
                            )
                            // A verdict never claims more than the observations do.
                            if (assessment.verdict == SetupVerdict.NotPaired) {
                                assertFalse(associated, label)
                            }
                            if (assessment.verdict == SetupVerdict.Blocked) {
                                assertTrue(associated, label)
                                assertTrue(!permission || !adapter, label)
                            }
                            if (assessment.verdict == SetupVerdict.AtRisk) {
                                assertTrue(associated && permission && adapter, label)
                                assertTrue(!exempt || !notifications, label)
                            }
                            cases++
                        }
                    }
                }
            }
        }
        assertEquals(32, cases)
    }

    // ── syncRecencyFor ───────────────────────────────────────────────────────

    private val window = WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS

    @Test
    fun `never having synced is its own answer, not a very long gap`() {
        // Breaks if null is folded into "a large number": a fresh install has not
        // failed at anything, and telling the user their phone is killing the app
        // before the watch has ever had a window is a lie the screen would tell on
        // day one, every time.
        assertEquals(SyncRecency.NeverSynced, syncRecencyFor(null))
    }

    @Test
    fun `a sync just now is Recent`() {
        assertEquals(SyncRecency.Recent, syncRecencyFor(0L))
    }

    @Test
    fun `one missed window is still Recent`() {
        // The watch may simply have been out of range for an hour.
        assertEquals(SyncRecency.Recent, syncRecencyFor(window + 1))
    }

    @Test
    fun `the Recent boundary is inclusive at two windows`() {
        assertEquals(SyncRecency.Recent, syncRecencyFor(2 * window))
        assertEquals(SyncRecency.Overdue, syncRecencyFor(2 * window + 1))
    }

    @Test
    fun `the Overdue boundary is inclusive at six windows`() {
        assertEquals(SyncRecency.Overdue, syncRecencyFor(6 * window))
        assertEquals(SyncRecency.Stalled, syncRecencyFor(6 * window + 1))
    }

    @Test
    fun `a day without a sync is Stalled`() {
        assertEquals(SyncRecency.Stalled, syncRecencyFor(24 * window))
    }

    @Test
    fun `an absurd gap does not overflow into Recent`() {
        // The thresholds are multiples, so a naive implementation that multiplied
        // the elapsed time instead of the window could wrap. Long.MAX_VALUE is the
        // cheapest way to notice.
        assertEquals(SyncRecency.Stalled, syncRecencyFor(Long.MAX_VALUE))
    }

    @Test
    fun `a negative gap reads as Recent rather than as a fault`() {
        // HealthSnapshot clamps, so this should be unreachable — but the wall clock
        // does jump backwards, and "your phone is killing the app" is the wrong
        // thing to say when what actually happened is an NTP correction.
        assertEquals(SyncRecency.Recent, syncRecencyFor(-1L))
        assertEquals(SyncRecency.Recent, syncRecencyFor(Long.MIN_VALUE))
    }

    @Test
    fun `the thresholds are multiples of the protocol's window, not a second copy of an hour`() {
        // Breaks if someone writes 7200 and 21600 in here. The scale that makes
        // "overdue" mean anything is the rate at which the watch offers a chance,
        // and PROTOCOL.md section 8 keeps that number in exactly one file.
        assertEquals(SyncRecency.Recent, syncRecencyFor(WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS * 2))
        assertEquals(SyncRecency.Stalled, syncRecencyFor(WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS * 7))
    }

    @Test
    fun `recency composes with HealthSnapshot's own clamp`() {
        // The two halves as the screen actually uses them: the snapshot answers
        // "how long since", this answers "is that bad". A clock that ran backwards
        // must not come out the far end as Stalled.
        val synced = HealthSnapshot(lastSuccessUtcEpochSeconds = 1_000_000)
        assertEquals(SyncRecency.Recent, syncRecencyFor(synced.secondsSinceLastSuccess(1_000_000)))
        assertEquals(SyncRecency.Recent, syncRecencyFor(synced.secondsSinceLastSuccess(1)))
        assertEquals(SyncRecency.Stalled, syncRecencyFor(synced.secondsSinceLastSuccess(1_000_000 + 7 * window)))
        assertEquals(SyncRecency.NeverSynced, syncRecencyFor(HealthSnapshot().secondsSinceLastSuccess(1_000_000)))
    }

    // ── oemBatteryAdviceFor ──────────────────────────────────────────────────

    @Test
    fun `the vendors docs background-execution names are each recognised, in their own casing`() {
        // Build.MANUFACTURER is whatever the vendor shipped, and they do not agree
        // on case. Breaks on a plain equality check against a capitalised list.
        assertEquals(OemBatteryAdvice.Xiaomi, oemBatteryAdviceFor("Xiaomi"))
        assertEquals(OemBatteryAdvice.Huawei, oemBatteryAdviceFor("HUAWEI"))
        assertEquals(OemBatteryAdvice.Samsung, oemBatteryAdviceFor("samsung"))
        assertEquals(OemBatteryAdvice.BbkGroup, oemBatteryAdviceFor("OnePlus"))
        assertEquals(OemBatteryAdvice.BbkGroup, oemBatteryAdviceFor("OPPO"))
        assertEquals(OemBatteryAdvice.BbkGroup, oemBatteryAdviceFor("vivo"))
    }

    @Test
    fun `case is folded in both directions`() {
        assertEquals(OemBatteryAdvice.Xiaomi, oemBatteryAdviceFor("XIAOMI"))
        assertEquals(OemBatteryAdvice.Huawei, oemBatteryAdviceFor("Huawei"))
        assertEquals(OemBatteryAdvice.Samsung, oemBatteryAdviceFor("SAMSUNG"))
    }

    @Test
    fun `an unknown or absent manufacturer gets the generic advice, not silence`() {
        // Generic advice is the honest answer for a phone nobody here has tried,
        // and it is still advice — the failure mode this file is about is a user
        // who is never told there is anything to do.
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor(null))
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor(""))
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor("Google"))
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor("Fairphone"))
    }

    @Test
    fun `a near miss is not matched`() {
        // Substring matching would put "Samsung Electronics" and "not-samsung" in
        // the same bucket; exact identity is what Build.MANUFACTURER actually gives.
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor("Samsung Electronics"))
        assertEquals(OemBatteryAdvice.Generic, oemBatteryAdviceFor("xiaomi-clone"))
    }
}
