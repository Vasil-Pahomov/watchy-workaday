package com.workaday.app

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Build
import android.os.Handler
import android.util.Log
import com.workaday.core.ClientEpoch
import com.workaday.core.GATT_LOCAL_FAILURE
import com.workaday.core.TransportEvent
import com.workaday.core.protocol.WatchProtocol
import java.util.UUID

/**
 * The one thing in this app that touches a radio.
 *
 * It performs the GATT half of the machine's `Action`s and forwards the callbacks
 * back as `TransportEvent`s. It decides nothing: there is no `if` in here that
 * chooses *whether* to do something, only branches that choose *how* (which
 * platform API this Android version wants) and translations of a platform failure
 * into the machine's vocabulary.
 *
 * Three properties it is responsible for, all of them Law 2:
 *
 * - **One owner, one `close()`.** [gatt] is the only reference to a
 *   `BluetoothGatt` in the app, [close] is the only place `close()` is called,
 *   and every failure path in this file routes through the machine, which always
 *   emits `Action.CloseConnection` before it emits another `Action.ArmAutoConnect`.
 * - **No callbacks from a closed client.** Each client is stamped with a
 *   [ClientEpoch]; a callback whose epoch is no longer live is dropped before it
 *   reaches the machine. Without that, `disconnect()`'s own disconnect echo would
 *   land while the machine is `Armed`, be read as PROTOCOL.md §6.2's `status 133`,
 *   and cost a backoff step after **every successful sync**.
 * - **One operation at a time.** Every callback is *posted* to the link's serial
 *   thread rather than dispatched on the binder thread it arrived on, and nothing
 *   in here calls back into the machine synchronously while its actions are being
 *   performed. The machine emits at most one GATT operation per event, always
 *   preceded by its timeout — so "one outstanding operation, each with a timeout"
 *   holds by construction rather than by a queue that would provably never hold
 *   more than one item.
 *
 * Every method must run on the link's serial thread, and [post] is what puts work
 * on it.
 *
 * **Why the two platform seams are constructor parameters.** The second property
 * above is wiring, not logic: it holds only if each callback closes over its own
 * generation, if [close] retires the epoch before it touches the client, and if
 * [admits][ClientEpoch.admits] is asked *inside* the posted body rather than when
 * the body is posted. `ClientEpochTest` cannot see any of that — it tests the
 * gate, not the hole — and deleting the check outright leaves the whole suite
 * green. So [openClient] and [post] are injected, and `WatchLinkDeliveryTest`
 * drives the real thing on a JVM. [forDevice] is the production wiring.
 */
internal class WatchLink(
    /**
     * `connectGatt(context, autoConnect = true, callback, TRANSPORT_LE)`, or null
     * when there is no device or the stack refused. May throw `SecurityException`
     * if `BLUETOOTH_CONNECT` has gone; [arm] handles that.
     */
    private val openClient: (BluetoothGattCallback) -> BluetoothGatt?,
    /** Hands a body to the link's serial thread. `Handler::post` in production. */
    private val post: (Runnable) -> Unit,
    /** Feeds the machine. Called on the serial thread, never re-entrantly. */
    private val dispatch: (TransportEvent) -> Unit,
) {

    private val epoch = ClientEpoch()

    /** The only `BluetoothGatt` reference in the app. */
    private var gatt: BluetoothGatt? = null

    // ── Action.ArmAutoConnect ────────────────────────────────────────────────

    /**
     * `connectGatt(context, autoConnect = true, …)` — the resting state.
     *
     * Not a scan: `autoConnect` hands the wait to the Bluetooth controller, which
     * costs the app nothing while idle, survives Doze, and fires when the watch
     * opens its window (CLAUDE.md, PROTOCOL.md §1).
     *
     * A fresh `BluetoothDevice` every time, because §6.2 names a stale one as a
     * common cause of `status 133`. `TRANSPORT_LE` explicitly, because the
     * default lets the stack try BR/EDR against a peripheral that has none — the
     * other common cause of the same 133.
     */
    fun arm() {
        // Unreachable: the machine closes before it arms. Kept because arming
        // over a live client would leak one of the process's limited GATT client
        // slots, and that failure is invisible until every later connection fails.
        close()
        val generation = epoch.open()

        val client = try {
            openClient(callbackFor(generation))
        } catch (e: SecurityException) {
            // BLUETOOTH_CONNECT was revoked. A handled transition, not a crash:
            // the machine parks in Blocked and arms again when it comes back.
            Log.w(LOG_TAG, "connectGatt denied", e)
            dispatchLater(TransportEvent.PermissionRevoked)
            return
        }
        if (client == null) {
            deliver(generation, TransportEvent.Connected(GATT_LOCAL_FAILURE))
            return
        }
        gatt = client
    }

    // ── Action.CloseConnection ───────────────────────────────────────────────

    /**
     * `disconnect()`, then `close()`, then no further callbacks. All three parts,
     * on every path.
     *
     * Idempotent, so the machine's belt-and-braces close before an arm is free,
     * and so shutting the service down after a close is not an error.
     */
    fun close() {
        // First, so that anything already queued on the serial thread from this
        // client is refused when it runs — and unconditionally, so that a link
        // whose connectGatt never returned a client still retires its epoch.
        // Moving this below the guard below is caught by WatchLinkDeliveryTest.
        epoch.close()
        val client = gatt ?: return
        gatt = null
        try {
            client.disconnect()
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "disconnect() denied; closing anyway", e)
        } finally {
            // The close that Law 2 makes non-negotiable: it runs even if
            // disconnect() threw something this method does not handle, because a
            // leaked client burns a GATT slot and enough of them make every later
            // connection fail with no visible cause.
            try {
                client.close()
            } catch (e: SecurityException) {
                Log.w(LOG_TAG, "close() denied", e)
            }
        }
    }

    // ── Action.DiscoverServices ──────────────────────────────────────────────

    fun discoverServices() {
        val client = gatt ?: return failLocally { TransportEvent.ServicesDiscovered(it) }
        val issued = try {
            client.discoverServices()
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "discoverServices() denied", e)
            dispatchLater(TransportEvent.PermissionRevoked)
            return
        }
        if (!issued) failLocally { TransportEvent.ServicesDiscovered(it) }
    }

    // ── Action.EnableStatusNotifications ─────────────────────────────────────

    /**
     * PROTOCOL.md §4 op 2: subscribe to Status **before** writing Time. Writing
     * first races the notification and loses it.
     */
    fun enableStatusNotifications() {
        val client = gatt ?: return failLocally { TransportEvent.DescriptorWritten(it) }
        val status = characteristic(client, WatchProtocol.STATUS_CHARACTERISTIC_UUID)
        val cccd = status?.getDescriptor(WatchProtocol.CCCD_UUID)
        if (status == null || cccd == null) {
            // The peer is not the watch, or the platform handed back a cached
            // profile from before the firmware had this service. Nothing will
            // ever call back, so say so now rather than in five seconds.
            Log.w(LOG_TAG, "Status characteristic or its CCCD missing from the discovered profile")
            return failLocally { TransportEvent.DescriptorWritten(it) }
        }
        try {
            if (!client.setCharacteristicNotification(status, true)) {
                return failLocally { TransportEvent.DescriptorWritten(it) }
            }
            val value = WatchProtocol.cccdEnableNotificationValue()
            // The branch is *how*, not *whether*: minSdk is 31, so the pre-33
            // API is still reachable, and it is the only one that exists there.
            // Kept inside the try so the SecurityException handling above is
            // visibly the handler for these calls.
            @Suppress("DEPRECATION")
            val issued = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                client.writeDescriptor(cccd, value) == BluetoothStatusCodes.SUCCESS
            } else {
                cccd.value = value
                client.writeDescriptor(cccd)
            }
            if (!issued) failLocally { TransportEvent.DescriptorWritten(it) }
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "enabling Status notifications denied", e)
            dispatchLater(TransportEvent.PermissionRevoked)
        }
    }

    // ── Action.WriteTime, Action.WriteFindDismiss ────────────────────────────

    /**
     * PROTOCOL.md §4 op 3, §3.1: 12 bytes, **with response**.
     *
     * The bytes arrive already encoded — the wire format is `core/`'s and the
     * Android layer never touches it (§8).
     */
    fun writeTime(payload: ByteArray) =
        writeWithResponse(WatchProtocol.TIME_CHARACTERISTIC_UUID, "Time", payload)

    /**
     * PROTOCOL.md §4.1, §3.3: the FindDismiss frame, **with response**, on Find.
     *
     * The same write as Time on a different characteristic, and answered by the
     * same `onCharacteristicWrite` — the machine tells the two apart by the state
     * it is in, which is the only place a write can be outstanding from. An older
     * watch has no Find characteristic; that is the `missing from the discovered
     * profile` path below, reported at once as a local failure so the machine
     * closes and re-arms instead of waiting five seconds for a callback that will
     * never come (§6.2).
     */
    fun writeFindDismiss(payload: ByteArray) =
        writeWithResponse(WatchProtocol.FIND_CHARACTERISTIC_UUID, "Find", payload)

    private fun writeWithResponse(uuid: UUID, name: String, payload: ByteArray) {
        val client = gatt ?: return failLocally { TransportEvent.CharacteristicWritten(it) }
        val target = characteristic(client, uuid)
        if (target == null) {
            Log.w(LOG_TAG, "$name characteristic missing from the discovered profile")
            return failLocally { TransportEvent.CharacteristicWritten(it) }
        }
        try {
            // WRITE_TYPE_DEFAULT is "write with response", which is what §3.1 and
            // §3.3 both specify. The version branch is *how*; both halves write the
            // same bytes the same way.
            @Suppress("DEPRECATION")
            val issued = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                client.writeCharacteristic(
                    target,
                    payload,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                target.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                target.value = payload
                client.writeCharacteristic(target)
            }
            if (!issued) failLocally { TransportEvent.CharacteristicWritten(it) }
        } catch (e: SecurityException) {
            Log.w(LOG_TAG, "$name write denied", e)
            dispatchLater(TransportEvent.PermissionRevoked)
        }
    }

    // ── Callbacks ────────────────────────────────────────────────────────────

    /**
     * One callback object per client, closed over that client's epoch.
     *
     * Nothing here filters by UUID, and that is deliberate rather than an
     * oversight: PROTOCOL.md §2.1 gives the watch exactly one service, one
     * writable characteristic and one notifying characteristic, so there is
     * nothing else these callbacks could be about. Law 5 — exploit what we know.
     */
    private fun callbackFor(generation: Long) = object : BluetoothGattCallback() {

        /**
         * [client] is declared nullable, and it is the only signature here that
         * is. None of these callbacks ever reads the client — [WatchLink] owns
         * the one reference — and the platform's own declaration is an
         * unannotated platform type, so widening is free. What it buys is the
         * one thing worth testing on a JVM: the disconnect echo of our own
         * `disconnect()`, the exact delivery that costs a backoff step after
         * every successful sync if the epoch gate is ever broken.
         */
        override fun onConnectionStateChange(client: BluetoothGatt?, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED ->
                    deliver(generation, TransportEvent.Connected(status))

                BluetoothProfile.STATE_DISCONNECTED ->
                    deliver(generation, TransportEvent.Disconnected(status))

                // CONNECTING / DISCONNECTING are transitions the machine has no
                // vocabulary for, because there is nothing it would do about one.
                else -> Unit
            }
        }

        override fun onServicesDiscovered(client: BluetoothGatt, status: Int) {
            deliver(generation, TransportEvent.ServicesDiscovered(status))
        }

        override fun onDescriptorWrite(
            client: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            deliver(generation, TransportEvent.DescriptorWritten(status))
        }

        override fun onCharacteristicWrite(
            client: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            deliver(generation, TransportEvent.CharacteristicWritten(status))
        }

        override fun onCharacteristicChanged(
            client: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            deliver(generation, TransportEvent.NotificationReceived(value.copyOf()))
        }

        /**
         * The pre-API-33 delivery. `minSdk` is 31, so it is still reachable.
         *
         * Overriding both is safe whichever way the platform routes: if the
         * three-argument default delegates here, this override is simply not
         * reached on 33+; if the platform ever called both, the second arrival
         * lands after the first has already closed the client and is refused by
         * its epoch.
         */
        @Deprecated("Superseded by the three-argument overload in API 33")
        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(
            client: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            // Copied on the callback thread: the array belongs to the
            // characteristic and the next notification overwrites it in place.
            deliver(generation, TransportEvent.NotificationReceived(characteristic.value?.copyOf()))
        }
    }

    // ── Plumbing ─────────────────────────────────────────────────────────────

    /**
     * The UUIDs come from [WatchProtocol] and appear nowhere else in this app —
     * PROTOCOL.md §8 allows exactly one file to hold a UUID literal, and it is not
     * this one.
     */
    private fun characteristic(client: BluetoothGatt, uuid: UUID): BluetoothGattCharacteristic? =
        client.getService(WatchProtocol.SYNC_SERVICE_UUID)?.getCharacteristic(uuid)

    /**
     * Hand [event] to the machine on the serial thread, if the client that
     * produced it is still the live one.
     *
     * The epoch is checked when the message *runs*, not when it is posted, which
     * is the only check that means anything: the whole point is a callback that
     * was already in flight when `close()` happened.
     */
    private fun deliver(generation: Long, event: TransportEvent) {
        post {
            if (epoch.admits(generation)) dispatch(event) else logInfo("dropped $event from a closed client")
        }
    }

    /**
     * Report an operation the local stack never got onto the air.
     *
     * Posted rather than dispatched: this is called while the machine's actions
     * are being performed, and re-entering it there would break the one-event-at-
     * a-time discipline that makes "one outstanding GATT operation" true.
     */
    private fun failLocally(event: (Int) -> TransportEvent) {
        deliver(epoch.current, event(GATT_LOCAL_FAILURE))
    }

    /**
     * Environment events are **not** epoch-gated: a revoked permission is true of
     * the app, not of one client, and it has to reach the machine even while the
     * client that noticed it is being torn down.
     */
    private fun dispatchLater(event: TransportEvent) {
        post { dispatch(event) }
    }

    companion object {

        /**
         * The production wiring: a fresh `BluetoothDevice` and a real
         * `connectGatt` on every arm, posted through [handler].
         *
         * A fresh device every time because PROTOCOL.md §6.2 names a stale one as
         * a common cause of `status 133`. `TRANSPORT_LE` explicitly, because the
         * default lets the stack try BR/EDR against a peripheral that has none —
         * the other common cause of the same 133.
         *
         * The `SecurityException` from a revoked `BLUETOOTH_CONNECT` is
         * deliberately **not** caught here and is suppressed for lint on that
         * basis: [arm] catches it and turns it into
         * [TransportEvent.PermissionRevoked], which parks the machine in
         * `Blocked` with a notification naming what the user has to fix.
         * Swallowing it in this lambda — which has no route to the machine —
         * would demote it to an ordinary connection failure and spend the app's
         * life in a backoff loop instead.
         */
        @Suppress("MissingPermission")
        fun forDevice(
            context: Context,
            adapter: BluetoothAdapter?,
            deviceAddress: String,
            handler: Handler,
            dispatch: (TransportEvent) -> Unit,
        ): WatchLink = WatchLink(
            openClient = { callback ->
                remoteDevice(adapter, deviceAddress)
                    ?.connectGatt(context, /* autoConnect = */ true, callback, BluetoothDevice.TRANSPORT_LE)
            },
            post = { body -> handler.post(body) },
            dispatch = dispatch,
        )

        private fun remoteDevice(adapter: BluetoothAdapter?, deviceAddress: String): BluetoothDevice? = try {
            adapter?.getRemoteDevice(deviceAddress)
        } catch (e: IllegalArgumentException) {
            // A malformed address in the store. Reporting it as a failed
            // connection attempt keeps the app in its bounded backoff instead of
            // crash-looping on every retry.
            Log.e(LOG_TAG, "stored watch address is not a Bluetooth address", e)
            null
        }
    }
}
