package com.workaday.app

import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.Context
import android.content.Intent
import android.content.IntentSender
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Parcelable
import android.os.ParcelUuid
import android.util.Log
import com.workaday.core.AssociationOutcome
import com.workaday.core.normaliseBluetoothAddress
import com.workaday.core.protocol.WatchProtocol
import com.workaday.core.reconcileAssociation
import com.workaday.core.supersededAssociations

/**
 * Everything in the app that touches CompanionDeviceManager.
 *
 * Law 1 calls the association mandatory rather than a nicety, and
 * `docs/background-execution.md` §1 says why: it is what grants
 * `REQUEST_COMPANION_RUN_IN_BACKGROUND` and
 * `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND` — the two
 * exemptions that let the service be started from the background at all — and it
 * lets the system find the watch on our behalf **without `ACCESS_FINE_LOCATION`**.
 *
 * What it is *not* is a connection. §1 again: "The association by itself creates no
 * connection and no scanning. We still do the GATT work ourselves." Nothing in this
 * file opens a socket; the address it produces is handed to [WatchStore] and the
 * radio work stays in [WatchLink].
 *
 * **Presence monitoring is deliberately not used** —
 * `startObservingDevicePresence` in either of its forms. See the note in
 * `docs/background-execution.md` §1; the short version is that a pending
 * `connectGatt(autoConnect = true)` already is the resting state, and a
 * `CompanionDeviceService` callback would be a second route into the state machine
 * that does not pass through [WatchLink.deliver] — where the epoch gate that stops
 * a closed client's events reaching the machine lives.
 *
 * There is no decision in this file. The two that belong to it —
 * `com.workaday.core.reconcileAssociation` and
 * `com.workaday.core.supersededAssociations` — are in `core/` where a JVM test can
 * drive them, because none of what happens here is reproducible on this machine.
 */
internal object Associations {

    /**
     * The addresses CompanionDeviceManager still holds associations for, or
     * **null meaning "not consulted"**.
     *
     * The null is the whole reason this returns a nullable list.
     * `reconcileAssociation` clears a stored address that no association backs, and
     * an empty list would therefore un-pair the phone. So every way of failing to
     * get an answer — no such system service, a `SecurityException`, or being
     * called before the first unlock — comes back as "we do not know" and changes
     * nothing.
     *
     * **[read] is injected, and the reason is the same one that made
     * [WatchLink.openClient] a constructor parameter.** The gate below exists to
     * protect against a platform behaviour nobody can look up — whether CDM's
     * records are readable during direct boot — so the only claim worth making
     * about it is *"we never ask"*, and that claim needs no Android at all.
     * Deleting the gate leaves every test in `core/` green, including the one
     * named `an unreadable association list confirms what is stored and clears
     * nothing`, because that test can only reach the decision and the bug is in
     * the wiring. `AssociationsTest` is what closes it.
     *
     * @param userUnlocked `UserManager.isUserUnlocked()`. The service starts during
     *   direct boot by design (Law 1's `LOCKED_BOOT_COMPLETED`). Rather than find
     *   out on a user's phone — where the cost is a locked boot silently un-pairing
     *   the watch — we do not ask until the storage the platform keeps them in is
     *   certainly up.
     * @param read the platform lookup. [forContext] is the production one.
     */
    fun observed(userUnlocked: Boolean, read: () -> List<String>?): List<String>? =
        if (userUnlocked) read() else null

    /**
     * The production reader for [observed], and the only place this app asks the
     * platform what it is associated with.
     *
     * Every failure is an absent answer rather than an empty one: no companion
     * service on this device, or a `SecurityException`. Both mean "we do not know",
     * and "we do not know" must never look like "the association is gone".
     */
    fun forContext(context: Context): () -> List<String>? = {
        val manager = context.getSystemService(CompanionDeviceManager::class.java)
        if (manager == null) {
            null
        } else {
            try {
                associatedAddresses(manager)
            } catch (e: SecurityException) {
                // Handleable, and the handling is to know nothing rather than to
                // conclude something wrong.
                Log.w(LOG_TAG, "companion associations could not be read", e)
                null
            }
        }
    }

    /**
     * Give up the associations that are not the watch (Law 5 — one watch).
     *
     * Only ever called from the foreground association flow, with the list
     * `supersededAssociations` produced, immediately after the user has said which
     * watch is theirs. Revoking is not reversible from here, so the moment when the
     * answer is certain is the only moment it happens.
     */
    fun revoke(context: Context, addresses: List<String>) {
        if (addresses.isEmpty()) return
        val manager = context.getSystemService(CompanionDeviceManager::class.java) ?: return
        for (address in addresses) {
            try {
                disassociate(manager, address)
                logInfo("revoked superseded association $address")
            } catch (e: IllegalArgumentException) {
                // Already gone. Nothing to do and nothing wrong.
                Log.w(LOG_TAG, "no association left to revoke for $address", e)
            } catch (e: SecurityException) {
                Log.w(LOG_TAG, "not permitted to revoke the association for $address", e)
            }
        }
    }

    /**
     * The API-33 split. *How*, not *whether*: `minSdk` is 31, so the older call is
     * still reachable and is the only one that exists there.
     */
    @Suppress("DEPRECATION")
    private fun associatedAddresses(manager: CompanionDeviceManager): List<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // MacAddress.toString() is lower case and getRemoteDevice refuses that,
            // which is what normaliseBluetoothAddress exists for. Left raw here so
            // there is exactly one place that normalises: the core decision.
            manager.myAssociations.mapNotNull { it.deviceMacAddress?.toString() }
        } else {
            manager.associations
        }

    @Suppress("DEPRECATION")
    private fun disassociate(manager: CompanionDeviceManager, address: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val id = manager.myAssociations
                .firstOrNull { normaliseBluetoothAddress(it.deviceMacAddress?.toString()) == address }
                ?.id
                ?: return
            manager.disassociate(id)
        } else {
            manager.disassociate(address)
        }
    }
}

/**
 * `reconcileAssociation`'s answer, performed.
 *
 * **Extracted from `WatchLinkService` so that a JVM test can reach it**, for the
 * reason `WatchLinkDeliveryTest`'s header already gives about a different seam:
 * the decision is covered in `core/` and the *performance* of it is not, so
 * deleting the `writeAddress(null)` below turns the whole of `Association.kt` into
 * a no-op while `a stored address with no association left is abandoned, not kept`
 * stays green. Three of the five branches have a durable consequence and each one
 * is a different failure:
 *
 * - not clearing on [AssociationOutcome.Abandoned] leaves a phone that looks
 *   paired, arms at an address nothing backs, and backs off for months;
 * - not persisting on [AssociationOutcome.Adopted] arms *this* process and no
 *   other, because [BootReceiver] and [WatchdogWorker] both gate on the stored
 *   address — so the service never comes back after a reboot;
 * - writing anything at all on [AssociationOutcome.Confirmed] would be a
 *   redundant `commit()` on the service's serial thread on every start.
 *
 * @param writeAddress `WatchStore.watchAddress`'s setter. Called at most once.
 * @return the address to arm against, or null when there is no watch.
 */
internal fun reArmedWatchAddress(
    storedAddress: String?,
    associatedAddresses: List<String>?,
    writeAddress: (String?) -> Unit,
): String? = when (val outcome = reconcileAssociation(storedAddress, associatedAddresses)) {
    is AssociationOutcome.Confirmed -> {
        logInfo("association confirmed for ${outcome.address}")
        outcome.address
    }

    is AssociationOutcome.Adopted -> {
        // The association outlived the address — the process died between the
        // system creating it and our onActivityResult writing it down. Persisting
        // is what makes the recovery survive this process too.
        logInfo("adopting the surviving association ${outcome.address}")
        writeAddress(outcome.address)
        outcome.address
    }

    is AssociationOutcome.Abandoned -> {
        // The user revoked the association in Settings, or something else removed
        // it. Keeping the address would leave a phone that looks paired and can
        // never legitimately start its service from the background. Clearing it
        // makes the state honest: the service stops, the notification goes away,
        // and the screen says "no watch paired" with a button that fixes it.
        Log.w(LOG_TAG, "association for ${outcome.staleAddress} is gone; forgetting the watch")
        writeAddress(null)
        null
    }

    AssociationOutcome.Unpaired -> {
        logInfo("no watch associated")
        null
    }

    is AssociationOutcome.Ambiguous -> {
        // Deliberately no write. Guessing which of several is the watch is the
        // device registry Law 5 forbids, arrived at by accident.
        Log.w(LOG_TAG, "several associations and no stored watch: ${outcome.addresses}")
        null
    }
}

/**
 * The seam this whole change exists to close, performed: remember the watch, give
 * up the ones that are not it, and start the service.
 *
 * Also extracted for testability, and the ordering is the point of the test.
 * `writeAddress` must precede `startService`, because the service reads the stored
 * address in `initialise()` and stops itself if it finds none — reversing the two
 * gives a pairing that silently does not take until the watchdog's next period.
 * And `revoke` must be handed [supersededAssociations]' answer rather than the raw
 * record list, or pairing revokes the watch it has just paired.
 */
internal fun completePairing(
    address: String,
    associatedAddresses: List<String>?,
    writeAddress: (String) -> Unit,
    revoke: (List<String>) -> Unit,
    startService: () -> Unit,
) {
    writeAddress(address)
    // Law 5. Records we could not read revoke nothing: orEmpty() collapses "we do
    // not know" into "nothing to give up", which is the safe direction here — the
    // opposite one revokes on no evidence.
    revoke(supersededAssociations(address, associatedAddresses.orEmpty()))
    startService()
}

/** What the association flow has to say for itself, for the screen to render. */
internal sealed interface AssociationEvent {

    /** The system is scanning on our behalf. Foreground, and bounded by a timeout. */
    data object Scanning : AssociationEvent

    /** [AssociationFlow.SCAN_TIMEOUT_MS] elapsed with no watch found. */
    data object TimedOut : AssociationEvent

    /** The user backed out of the system's chooser. */
    data object Cancelled : AssociationEvent

    /** The platform refused, or handed back something that is not an address. */
    data class Failed(val reason: String?) : AssociationEvent

    /** Done. [address] is normalised and ready for `BluetoothAdapter.getRemoteDevice`. */
    data class Associated(val address: String) : AssociationEvent
}

/**
 * The one moment in this app's life that the user has to open it for, and the one
 * place an active scan happens.
 *
 * CLAUDE.md: "Active scanning is for one case only: first-time pairing,
 * user-initiated, foreground, hard timeout." All four hold here — the flow starts
 * from a button, the system runs the scan (so it is not even our radio budget), the
 * flow abandons itself in `onStop`, and [SCAN_TIMEOUT_MS] bounds the wait.
 *
 * The filter is the service UUID from [WatchProtocol] and nothing else. PROTOCOL.md
 * §2.2 puts that UUID in the **advertisement** rather than the scan response
 * precisely so this works; the local name `Workaday` is in the scan response and is
 * documented there as diagnostic, so filtering on it as well would mean a watch
 * whose scan response was not collected is invisible for no gain.
 *
 * Everything runs on the main thread: `associate` is handed the main looper, the
 * timeout is posted to it, and the result arrives on it. Nothing here does I/O —
 * the caller writes the address down on a background thread.
 */
internal class AssociationFlow(
    private val activity: Activity,
    private val onEvent: (AssociationEvent) -> Unit,
) {
    private val handler = Handler(Looper.getMainLooper())

    /**
     * True from [begin] until the flow ends or the Activity stops.
     *
     * A one-shot latch, and it carries three jobs: the timeout cannot fire after a
     * device was found, a late `onDeviceFound` cannot launch a chooser at an
     * Activity the user has left, and a double tap on the button cannot start two
     * scans (which is also how an app earns Android's silent scan throttling —
     * `docs/background-execution.md` §2).
     */
    private var scanning = false

    private val timeout = Runnable {
        if (!scanning) return@Runnable
        scanning = false
        onEvent(AssociationEvent.TimedOut)
    }

    fun begin() {
        if (scanning) return
        val manager = activity.getSystemService(CompanionDeviceManager::class.java)
        if (manager == null) {
            onEvent(AssociationEvent.Failed(null))
            return
        }
        scanning = true
        handler.postDelayed(timeout, SCAN_TIMEOUT_MS)
        onEvent(AssociationEvent.Scanning)
        manager.associate(request(), callback, handler)
    }

    /**
     * Stop listening. Called from `onStop`, which is what makes "foreground only"
     * true rather than intended: the system's scan may still be running, but a
     * result arriving after the user has left this screen does nothing.
     *
     * Deliberately does not disturb [onActivityResult] — by the time the chooser is
     * up the scan is over, and the chooser covering us is itself an `onStop`.
     */
    fun abandon() {
        scanning = false
        handler.removeCallbacks(timeout)
    }

    /**
     * @return true if this was the association result, whatever it said.
     */
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode != REQUEST_ASSOCIATE) return false
        abandon()
        if (resultCode != Activity.RESULT_OK) {
            onEvent(AssociationEvent.Cancelled)
            return true
        }
        val address = normaliseBluetoothAddress(addressFrom(data))
        if (address == null) {
            // Storing an unusable address is the failure this whole path exists to
            // avoid: it would leave a phone that looks paired and can never connect.
            Log.e(LOG_TAG, "association returned no usable device address")
            onEvent(AssociationEvent.Failed(null))
            return true
        }
        onEvent(AssociationEvent.Associated(address))
        return true
    }

    private fun request(): AssociationRequest {
        val filter = BluetoothLeDeviceFilter.Builder()
            .setScanFilter(
                ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(WatchProtocol.SYNC_SERVICE_UUID))
                    .build(),
            )
            .build()
        return AssociationRequest.Builder()
            .addDeviceFilter(filter)
            // Law 5, stated to the platform as well as to ourselves. It also means
            // the system stops as soon as it has one match rather than collecting a
            // list for the user to browse.
            .setSingleDevice(true)
            // No setDeviceProfile: DEVICE_PROFILE_WATCH would need
            // REQUEST_COMPANION_PROFILE_WATCH and a broader consent dialog, and the
            // two exemptions this app actually relies on come from having any
            // association at all (docs/background-execution.md §1).
            .build()
    }

    private val callback = object : CompanionDeviceManager.Callback() {

        /**
         * The pre-33 delivery, and still the one the platform routes through on
         * 33+ unless [onAssociationPending] is overridden — which it is, below.
         * `minSdk` is 31, so this one has to exist.
         */
        @Deprecated("Superseded by onAssociationPending in API 33")
        @Suppress("OVERRIDE_DEPRECATION")
        override fun onDeviceFound(intentSender: IntentSender) {
            present(intentSender)
        }

        /**
         * API 33+. Overriding both is safe in either direction: on 31 and 32 this
         * method is not in the base class and is simply never called, and on 33+
         * this one replaces the default that would have delegated to
         * [onDeviceFound]. [scanning] makes a double delivery a no-op regardless.
         */
        override fun onAssociationPending(intentSender: IntentSender) {
            present(intentSender)
        }

        override fun onFailure(error: CharSequence?) {
            if (!scanning) return
            abandon()
            Log.w(LOG_TAG, "companion association failed: $error")
            onEvent(AssociationEvent.Failed(error?.toString()))
        }
    }

    /**
     * Hand the system's chooser to the user.
     *
     * The scan is over the moment this runs — a device was found — so the timeout
     * is dropped here rather than left to fire underneath the dialog.
     */
    private fun present(intentSender: IntentSender) {
        if (!scanning) return
        handler.removeCallbacks(timeout)
        try {
            activity.startIntentSenderForResult(intentSender, REQUEST_ASSOCIATE, null, 0, 0, 0)
        } catch (e: IntentSender.SendIntentException) {
            scanning = false
            Log.e(LOG_TAG, "could not show the companion device chooser", e)
            onEvent(AssociationEvent.Failed(null))
        }
    }

    /**
     * The device the user picked, as an address string.
     *
     * `EXTRA_DEVICE` is read untyped on purpose: its class depends on which filter
     * the request used — a `ScanResult` for `BluetoothLeDeviceFilter`, a
     * `BluetoothDevice` for `BluetoothDeviceFilter` — so there is no single class to
     * hand the API-33 typed getter, and both shapes are accepted here rather than
     * assumed. API 33+ also supplies an `AssociationInfo`, used as the fallback.
     *
     * Whatever comes back is normalised by the caller before it is believed.
     */
    @Suppress("DEPRECATION")
    private fun addressFrom(data: Intent?): String? {
        val intent = data ?: return null
        val fromDevice = when (val device = intent.getParcelableExtra<Parcelable>(CompanionDeviceManager.EXTRA_DEVICE)) {
            is ScanResult -> device.device?.address
            is BluetoothDevice -> device.address
            else -> null
        }
        if (fromDevice != null) return fromDevice
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return null
        return intent
            .getParcelableExtra(CompanionDeviceManager.EXTRA_ASSOCIATION, AssociationInfo::class.java)
            ?.deviceMacAddress
            ?.toString()
    }

    companion object {
        private const val REQUEST_ASSOCIATE = 4001

        /**
         * The hard timeout on the one scan this app ever performs.
         *
         * Sixty seconds because of what the user has to do inside it: PROTOCOL.md
         * §5.1 gives the watch a **6 s** advertising window, opened hourly or on
         * demand from its own Sync menu — so pairing means walking to the watch and
         * choosing Sync while this screen is open. A ten-second timeout would fail
         * everybody; an unbounded one would leave the app claiming to be pairing
         * forever, which is the "installed, looks fine, doing nothing" state Law 2
         * refuses, only in the UI.
         *
         * Not a PROTOCOL.md §5 number and not derived from one, so it lives here
         * beside the effect rather than in [WatchProtocol] (§8).
         */
        const val SCAN_TIMEOUT_MS = 60_000L
    }
}
