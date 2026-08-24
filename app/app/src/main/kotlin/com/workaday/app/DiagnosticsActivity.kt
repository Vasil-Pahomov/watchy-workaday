package com.workaday.app

import android.Manifest
import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.text.format.DateUtils
import android.util.Log
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.workaday.core.Backoff
import com.workaday.core.ExchangeReport
import com.workaday.core.ExchangeHistory
import com.workaday.core.HealthLevel
import com.workaday.core.HealthSnapshot
import com.workaday.core.JitterSource
import com.workaday.core.SetupAssessment
import com.workaday.core.SetupTask
import com.workaday.core.SetupVerdict
import com.workaday.core.assessSetup
import com.workaday.core.exchangeHistoryFor
import com.workaday.core.oemBatteryAdviceFor
import com.workaday.core.protocol.SyncResult

/**
 * The diagnostic window, and the one thing the user has to open the app for — once,
 * to associate the watch.
 *
 * **Law 1 is the constraint this class is shaped by:** "The Activity is a
 * diagnostic window, nothing more. Killing it, swiping the app from Recents, or
 * never launching it again must change nothing." So:
 *
 * - It owns no state. Everything shown is read, at the moment it is shown, out of
 *   [WatchStore] — which the *service* writes — plus a few live platform
 *   observations. There is no ViewModel, no cache, no singleton it initialises, and
 *   nothing anywhere else in the app reads a field of this class.
 * - It starts the service after an association, and offers to start it when it is
 *   down. It is never what keeps it up: [BootReceiver] and [WatchdogWorker] do
 *   that whether this screen has ever been opened or not.
 * - Deleting this file would cost the user the ability to pair a watch and to see
 *   what is going on. It would not stop a paired app syncing for months.
 *
 * The reverse arrow — service to Activity — does not exist either. Nothing is
 * pushed here; this screen pulls when it is resumed and when the user asks. An
 * Activity that had to be alive to receive something would be exactly the
 * dependency Law 1 forbids.
 *
 * **No UI framework.** Plain [android.app.Activity] with views built in code. The
 * app has no AndroidX UI dependency, this screen is a column of text and buttons,
 * and Law 4 and Law 5 both say a dependency in an app that must keep working
 * untouched for months is a liability rather than a shortcut.
 */
class DiagnosticsActivity : Activity() {

    /**
     * Everything the screen shows, assembled off the main thread.
     *
     * Passed to [render] and dropped — deliberately not a field. The whole point is
     * that nothing here outlives the moment it is drawn.
     */
    private class Snapshot(
        val assessment: SetupAssessment,
        val health: HealthSnapshot,
        val lastExchange: ExchangeReport?,
        val watchAddress: String?,
        val serviceRunning: Boolean,
        val nowUtcEpochSeconds: Long,
    )

    private lateinit var verdictView: TextView
    private lateinit var todoView: TextView
    private lateinit var pairingView: TextView
    private lateinit var statusView: TextView
    private lateinit var oemView: TextView

    private lateinit var pairButton: Button
    private lateinit var permissionButton: Button
    private lateinit var bluetoothButton: Button
    private lateinit var batteryButton: Button
    private lateinit var notificationButton: Button
    private lateinit var serviceButton: Button

    /**
     * Owned by this Activity and dying with it, which is what "user-initiated,
     * foreground" means in practice — see [AssociationFlow.abandon].
     */
    private val association by lazy { AssociationFlow(this, ::onAssociationEvent) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildLayout())
    }

    override fun onResume() {
        super.onResume()
        // Permissions, the adapter and the battery exemption can all have changed
        // while the user was in Settings, and the service may have died since.
        // Nothing is remembered between resumes, so this is the only read there is.
        refresh()
    }

    override fun onStop() {
        // The scan is bounded by the foreground, not only by its timeout: a result
        // arriving after the user has left this screen does nothing.
        association.abandon()
        super.onStop()
    }

    /**
     * The framework's own result callback. Not the AndroidX Activity Result API,
     * which would mean an `androidx.activity` dependency for one `IntentSender`.
     */
    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        association.onActivityResult(requestCode, resultCode, data)
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        // Deliberately no branch on the result. `refresh()` re-observes and
        // `assessSetup` decides what is still outstanding; a "did they say yes"
        // check here would be a second copy of that decision, living in the one
        // layer where a JVM test cannot reach it.
        refresh()
    }

    // ── Reading the world ────────────────────────────────────────────────────

    /**
     * Blocking work on a background thread, rendering on the main one.
     *
     * [WatchStore] is device-protected `SharedPreferences` and every read of it is
     * disk I/O, banned on the main thread for the same reason it is banned in the
     * service. A plain [Thread] rather than a coroutine: one screen, one read, and
     * no dependency worth adding for it.
     */
    private fun refresh() {
        val context = applicationContext
        Thread({
            val snapshot = snapshot(context)
            runOnUiThread { if (!isFinishing && !isDestroyed) render(snapshot) }
        }, "workaday-diagnostics").start()
    }

    private fun snapshot(context: Context): Snapshot {
        val store = WatchStore.open(context)
        val address = store.watchAddress
        return Snapshot(
            assessment = assessSetup(observeSetup(context, watchAssociated = address != null)),
            health = store.readHealth(),
            lastExchange = store.readLastExchange(),
            watchAddress = address,
            serviceRunning = WatchLinkService.isRunning,
            nowUtcEpochSeconds = AndroidClock.utcEpochSeconds(),
        )
    }

    // ── Drawing it ───────────────────────────────────────────────────────────

    private fun render(snapshot: Snapshot) {
        val assessment = snapshot.assessment
        verdictView.setText(textFor(assessment.verdict))
        verdictView.setTextColor(colourFor(assessment.verdict))

        todoView.text = if (assessment.tasks.isEmpty()) {
            getString(R.string.todo_none)
        } else {
            assessment.tasks.joinToString("\n") { "• " + getString(textFor(it)) }
        }

        statusView.text = statusLines(snapshot).joinToString("\n")

        pairButton.setText(
            if (snapshot.watchAddress == null) R.string.button_pair else R.string.button_pair_again,
        )
        permissionButton.showIf(SetupTask.GrantBluetoothPermission in assessment.tasks)
        bluetoothButton.showIf(SetupTask.TurnBluetoothOn in assessment.tasks)
        batteryButton.showIf(SetupTask.ExemptFromBatteryOptimisation in assessment.tasks)
        notificationButton.showIf(SetupTask.GrantNotificationPermission in assessment.tasks)
        // Not part of the setup assessment, because a service that is momentarily
        // down is not something the user configured wrongly. Worth a button anyway:
        // "paired, permitted, and not running" is what an OEM battery manager looks
        // like from in here.
        serviceButton.showIf(snapshot.watchAddress != null && !snapshot.serviceRunning)

        oemView.text = listOf(
            getString(R.string.oem_intro),
            getString(textFor(oemBatteryAdviceFor(Build.MANUFACTURER))),
            getString(R.string.oem_footer),
        ).joinToString("\n\n")
    }

    /** The facts, in the order someone diagnosing a silent app would want them. */
    private fun statusLines(snapshot: Snapshot): List<String> = buildList {
        add(
            snapshot.watchAddress
                ?.let { getString(R.string.status_watch_paired, it) }
                ?: getString(R.string.status_watch_none),
        )
        add(
            getString(
                if (snapshot.serviceRunning) R.string.status_service_running else R.string.status_service_stopped,
            ),
        )

        // One value, four lines, so the relationship between them is decided in
        // core/ and asserted there. Rendering them from four separate reads is
        // what let "the last exchange succeeded" appear under "Last sync: never".
        val history = exchangeHistoryFor(
            health = snapshot.health,
            lastAttempt = snapshot.lastExchange,
            nowUtcEpochSeconds = snapshot.nowUtcEpochSeconds,
            failingThreshold = FAILING_THRESHOLD,
        )
        add(
            history.lastSuccessUtcEpochSeconds
                ?.let { getString(R.string.status_last_sync, relativeTime(it, snapshot.nowUtcEpochSeconds)) }
                ?: getString(R.string.status_last_sync_never),
        )
        add(getString(textFor(history.recency)))
        add(healthLine(history, snapshot.health.consecutiveFailures))
        addAll(exchangeLines(history, snapshot.nowUtcEpochSeconds))
    }

    /**
     * @param failures only ever reaches a string as a number of consecutive
     *   failures, so a lost counter cannot make this claim a count it does not
     *   have — the two branches that print it are unreachable while the level is
     *   [HealthLevel.NothingRecorded].
     */
    private fun healthLine(history: ExchangeHistory, failures: Int): String =
        when (history.health) {
            // The line this whole round is about. "Zero failures" is true on a
            // fresh install and after a success; only one of them has an exchange
            // to report.
            HealthLevel.NothingRecorded -> getString(R.string.status_health_nothing_recorded)
            HealthLevel.Healthy -> getString(R.string.status_health_healthy)
            HealthLevel.Degraded ->
                resources.getQuantityString(R.plurals.status_health_degraded, failures, failures)

            HealthLevel.Failing ->
                resources.getQuantityString(R.plurals.status_health_failing, failures, failures)
        }

    /**
     * `Action.ReportExchange`, surfaced. PROTOCOL.md §6.2 requires the `result` code
     * to be visible; this is where it becomes visible to somebody without `adb`.
     */
    private fun exchangeLines(history: ExchangeHistory, nowUtcEpochSeconds: Long): List<String> {
        val report = history.lastAttempt ?: return listOf(getString(R.string.status_last_attempt_none))
        return buildList {
            add(
                getString(
                    R.string.status_last_attempt,
                    relativeTime(report.atUtcEpochSeconds, nowUtcEpochSeconds),
                    getString(textFor(report.outcome)),
                ),
            )
            if (report.statusResultCode == HealthSnapshot.NO_RESULT_CODE) {
                add(getString(R.string.status_watch_silent))
            } else {
                val result = SyncResult.fromCode(report.statusResultCode)
                val said = result
                    ?.let { getString(textFor(it)) }
                    ?: getString(R.string.result_unknown_code, report.statusResultCode)
                add(getString(R.string.status_watch_said, said))
                // §6.2: BadVersion means the two sides have drifted and retrying
                // will not help. Saying so is the whole reason the code is shown.
                if (result != null && !result.retryingWillHelp) add(getString(R.string.result_retry_wont_help))
            }
            report.batteryPercent?.let { add(getString(R.string.status_watch_battery, it)) }
            report.fwBuild?.let { add(getString(R.string.status_firmware, it)) }
        }
    }

    private fun relativeTime(epochSeconds: Long, nowEpochSeconds: Long): CharSequence =
        DateUtils.getRelativeTimeSpanString(
            epochSeconds * 1_000L,
            nowEpochSeconds * 1_000L,
            DateUtils.MINUTE_IN_MILLIS,
        )

    private fun colourFor(verdict: SetupVerdict): Int = when (verdict) {
        SetupVerdict.Ready -> COLOUR_GOOD
        SetupVerdict.AtRisk -> COLOUR_WARN
        SetupVerdict.Blocked, SetupVerdict.NotPaired -> COLOUR_BAD
    }

    // ── The things the buttons do ────────────────────────────────────────────

    /**
     * The seam this whole change exists to close: an association writes the address
     * and starts the service, and from then on nothing needs this screen again.
     */
    private fun onAssociationEvent(event: AssociationEvent) {
        when (event) {
            AssociationEvent.Scanning -> pairingView.setText(R.string.pairing_scanning)
            AssociationEvent.TimedOut -> pairingView.setText(R.string.pairing_timed_out)
            AssociationEvent.Cancelled -> pairingView.setText(R.string.pairing_cancelled)

            is AssociationEvent.Failed ->
                pairingView.text = event.reason
                    ?.let { getString(R.string.pairing_failed_because, it) }
                    ?: getString(R.string.pairing_failed)

            is AssociationEvent.Associated -> {
                pairingView.text = getString(R.string.pairing_done, event.address)
                adopt(event.address)
            }
        }
    }

    /**
     * Write it down, drop the associations that are not this watch, start the
     * service — [completePairing], off the main thread because the first of those
     * is disk I/O.
     *
     * The ordering and the `supersededAssociations` filter both live in that
     * function rather than here, so that a JVM test can hold them: reversing the
     * first two steps loses the pairing until the watchdog's next period, and
     * handing `revoke` the raw record list revokes the watch just paired. Revoking
     * happens here and only here — foreground, immediately after the user has said
     * which watch is theirs, which is the one moment the answer is certain.
     *
     * Nothing is cancelled if the Activity dies mid-way: this is a plain [Thread],
     * so the address still lands and the service still starts. Law 1 — the window
     * is not allowed to be load-bearing even for the one thing it exists to do.
     */
    private fun adopt(address: String) {
        val context = applicationContext
        Thread({
            val store = WatchStore.open(context)
            completePairing(
                address = address,
                associatedAddresses = Associations.observed(
                    userUnlocked = isUserUnlocked(context),
                    read = Associations.forContext(context),
                ),
                writeAddress = { store.watchAddress = it },
                revoke = { Associations.revoke(context, it) },
                startService = { WatchLinkService.start(context) },
            )
            runOnUiThread { if (!isFinishing && !isDestroyed) refresh() }
        }, "workaday-pairing").start()
    }

    private fun requestBluetoothPermissions() {
        // Both, in one dialog: they are one user-visible permission group ("Nearby
        // devices"), both are declared in the manifest, and a declared runtime
        // permission that is never requested stays denied forever.
        requestPermissions(
            arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN),
            REQUEST_BLUETOOTH_PERMISSIONS,
        )
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATION_PERMISSION)
    }

    /**
     * Never lets a missing Settings screen take the app down with it. Not every OEM
     * build ships every `Settings.ACTION_*` activity, and a crash here would be a
     * crash in the one window that exists to explain problems.
     */
    private fun open(intent: Intent, fallback: Intent? = null) {
        try {
            startActivity(intent)
        } catch (e: ActivityNotFoundException) {
            Log.w(LOG_TAG, "no activity for ${intent.action}", e)
            if (fallback != null) open(fallback) else pairingView.setText(R.string.settings_unavailable)
        }
    }

    // ── Layout ───────────────────────────────────────────────────────────────

    private fun buildLayout(): View {
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = dp(16)
            setPadding(pad, pad, pad, pad)
        }

        verdictView = column.addHeading()
        todoView = column.addBody()

        pairButton = column.addButton(R.string.button_pair) { association.begin() }
        // Static, and load-bearing: PROTOCOL.md §5.1 gives the watch a 6 s
        // advertising window opened hourly or from its own Sync menu, so a user who
        // does not know to reach for that menu will simply watch this time out.
        column.addBody().setText(R.string.pairing_instructions)
        pairingView = column.addBody()

        permissionButton = column.addButton(R.string.button_permission) { requestBluetoothPermissions() }
        bluetoothButton = column.addButton(R.string.button_bluetooth) { open(bluetoothSettingsIntent()) }
        batteryButton = column.addButton(R.string.button_battery) {
            open(batteryOptimisationRequestIntent(this), fallback = batteryOptimisationSettingsIntent())
        }
        notificationButton = column.addButton(R.string.button_notifications) { requestNotificationPermission() }
        serviceButton = column.addButton(R.string.button_start_service) {
            WatchLinkService.start(applicationContext)
            refresh()
        }

        column.addHeading().setText(R.string.section_status)
        statusView = column.addBody()

        column.addHeading().setText(R.string.section_battery_managers)
        oemView = column.addBody()
        column.addButton(R.string.button_app_settings) { open(appSettingsIntent(this)) }
        column.addButton(R.string.button_refresh) { refresh() }

        // Hidden until the first render says otherwise, which happens one thread
        // hop later. Starting them visible would show a fully set-up user a column
        // of things to fix for a frame.
        for (button in listOf(permissionButton, bluetoothButton, batteryButton, notificationButton, serviceButton)) {
            button.showIf(false)
        }

        return ScrollView(this).apply {
            addView(
                column,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
    }

    private fun LinearLayout.addHeading(): TextView = TextView(context).also {
        it.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        it.setPadding(0, dp(16), 0, dp(4))
        addView(it, wrapContent())
    }

    private fun LinearLayout.addBody(): TextView = TextView(context).also {
        it.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        it.setPadding(0, dp(4), 0, dp(4))
        it.setLineSpacing(dp(4).toFloat(), 1f)
        addView(it, wrapContent())
    }

    private fun LinearLayout.addButton(text: Int, onClick: () -> Unit): Button =
        Button(context).also {
            it.setText(text)
            it.setOnClickListener { _ -> onClick() }
            addView(it, wrapContent())
        }

    private fun wrapContent() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    private fun View.showIf(visible: Boolean) {
        visibility = if (visible) View.VISIBLE else View.GONE
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private companion object {
        const val REQUEST_BLUETOOTH_PERMISSIONS = 4101
        const val REQUEST_NOTIFICATION_PERMISSION = 4102

        /**
         * The number of consecutive failures at which PROTOCOL.md §5.2's backoff is
         * pinned at its cap. Asked of the same [Backoff] the service runs, rather
         * than written down here, so the screen's idea of "as bad as it gets"
         * cannot drift from the machine's.
         */
        val FAILING_THRESHOLD = Backoff(JitterSource.NONE).attemptsToReachCap + 1

        // Literal colours rather than theme attributes: this app has no theme of
        // its own and no AndroidX to read one from, and three constants are a much
        // smaller liability than a dependency (Law 4).
        const val COLOUR_GOOD = 0xFF1B7F3B.toInt()
        const val COLOUR_WARN = 0xFF9A6700.toInt()
        const val COLOUR_BAD = 0xFFB3261E.toInt()
    }
}
