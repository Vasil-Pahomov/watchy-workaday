package com.workaday.app

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothProfile
import com.workaday.core.GATT_SUCCESS
import com.workaday.core.TransportEvent
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * The gate that stops a **closed** GATT client's callbacks reaching the state
 * machine — tested where it actually lives.
 *
 * `ClientEpochTest` in `core/` covers the gate itself: open, close, admit,
 * refuse. It cannot cover the hole, because the hole is wiring and the wiring is
 * here — each callback closing over its own generation, [WatchLink.close]
 * retiring the epoch before it touches the client, and `admits` being asked
 * *inside* the posted body rather than when the body is posted. Deleting the
 * check from that body leaves all 130 `core/` tests green; that is the gap these
 * tests exist to close.
 *
 * No Robolectric and no emulator. `openClient` and `post` are constructor seams,
 * the returned client is null throughout, and the only platform values used are
 * inlined `int` constants — so nothing here depends on any Android behaviour. It
 * is a wiring test, and wiring is all it claims.
 */
class WatchLinkDeliveryTest {

    /** Runs [WatchLink] with the serial thread replaced by a list. */
    private class Harness {
        val posted = mutableListOf<Runnable>()
        val dispatched = mutableListOf<TransportEvent>()
        val callbacks = mutableListOf<BluetoothGattCallback>()

        val link = WatchLink(
            // Null: a real BluetoothGatt cannot be built on a JVM, and none is
            // needed. The callback is still constructed and handed over, which is
            // the thing under test.
            openClient = { callback -> callbacks += callback; null },
            post = { posted += it },
            dispatch = { dispatched += it },
        )

        /** The callback object `connectGatt` was given for the nth arm. */
        fun callback(index: Int): BluetoothGattCallback = callbacks[index]

        /** Drain everything queued for the serial thread, in order. */
        fun runPosted() {
            val queued = posted.toList()
            posted.clear()
            queued.forEach { it.run() }
        }
    }

    /**
     * The disconnect Android delivers because *we* called `disconnect()`.
     *
     * `onConnectionStateChange` is the only callback whose unused `BluetoothGatt`
     * parameter is declared nullable, so it is the only one reachable without a
     * real client — and it is the right one, because it carries the exact
     * delivery this mechanism exists to refuse. The others are one-line calls to
     * the same [WatchLink] `deliver`, so widening them too would distort five
     * signatures to re-test one code path.
     */
    private fun BluetoothGattCallback.deliverDisconnect(status: Int = GATT_SUCCESS) {
        onConnectionStateChange(null as BluetoothGatt?, status, BluetoothProfile.STATE_DISCONNECTED)
    }

    private fun BluetoothGattCallback.deliverConnect() {
        onConnectionStateChange(null as BluetoothGatt?, GATT_SUCCESS, BluetoothProfile.STATE_CONNECTED)
    }

    @Test
    fun `the disconnect echo of our own close never reaches the machine`() {
        // This is the whole reason ClientEpoch exists. After a successful
        // exchange the machine emits CloseConnection then ArmAutoConnect; the
        // disconnect() inside that close makes Android deliver one last
        // STATE_DISCONNECTED. By the time it lands the machine is Armed, where
        // PROTOCOL.md §6.2 reads a disconnect as `status 133` — so admitting it
        // turns every successful sync into a sync plus a backoff step.
        val h = Harness()

        h.link.arm()
        h.runPosted()
        h.dispatched.clear()
        val closedClient = h.callback(0)

        h.link.close()
        h.link.arm()

        closedClient.deliverDisconnect()
        h.runPosted()

        assertTrue(
            h.dispatched.none { it is TransportEvent.Disconnected },
            "a closed client's disconnect reached the machine: ${h.dispatched}",
        )
    }

    @Test
    fun `a callback captured before close, delivered after it, is dropped`() {
        // The ordering the check depends on: `admits` is asked when the posted
        // body runs, not when it is posted. Posting the decision instead of the
        // question would let a callback that was already in flight through.
        val h = Harness()
        h.link.arm()
        h.runPosted()
        h.dispatched.clear()

        // In flight: posted while the client is still live...
        h.callback(0).deliverDisconnect()
        assertEquals(1, h.posted.size, "the callback must be posted, not dispatched inline")

        // ...and the close lands first, as it does on a real serial thread.
        h.link.close()
        h.runPosted()

        assertTrue(h.dispatched.isEmpty(), "an in-flight callback survived the close: ${h.dispatched}")
    }

    @Test
    fun `the live client's callbacks do get through`() {
        // The other half, and the one that fails if the gate is too tight: a test
        // that only proves things are dropped passes on a link that drops
        // everything.
        val h = Harness()
        h.link.arm()
        h.runPosted()
        h.dispatched.clear()

        h.callback(0).deliverConnect()
        h.runPosted()

        assertEquals(1, h.dispatched.size)
        val event = h.dispatched.single()
        assertTrue(event is TransportEvent.Connected)
        assertEquals(GATT_SUCCESS, event.gattStatus, "the status must be forwarded verbatim, not interpreted")
    }

    @Test
    fun `re-arming retires the previous client without any close in between`() {
        // arm() closes first, so even a caller that skipped CloseConnection
        // cannot end up with two live callback streams.
        val h = Harness()
        h.link.arm()
        val first = h.callback(0)
        h.link.arm()
        h.runPosted()
        h.dispatched.clear()

        first.deliverDisconnect(status = 133)
        h.runPosted()

        assertTrue(h.dispatched.isEmpty(), "the superseded client is still being heard: ${h.dispatched}")
    }

    @Test
    fun `close is idempotent and does not reopen the gate`() {
        val h = Harness()
        h.link.arm()
        h.runPosted()
        h.dispatched.clear()
        val client = h.callback(0)

        h.link.close()
        h.link.close()
        client.deliverDisconnect()
        h.runPosted()

        assertTrue(h.dispatched.isEmpty())
    }

    @Test
    fun `an operation performed with no client fails the exchange rather than stalling`() {
        // GATT_LOCAL_FAILURE's reason for existing: no callback is ever coming,
        // so saying so now is the difference between one retry and a five-second
        // timeout. And it must still go through the gate.
        val h = Harness()
        h.link.arm()
        h.runPosted()
        h.dispatched.clear()

        h.link.discoverServices()
        h.runPosted()

        val event = h.dispatched.single()
        assertTrue(event is TransportEvent.ServicesDiscovered)
        assertTrue(event.gattStatus != GATT_SUCCESS, "a failure to issue must not read as success")
    }

    @Test
    fun `an operation performed after close reaches nothing at all`() {
        val h = Harness()
        h.link.arm()
        h.runPosted()
        h.link.close()
        h.dispatched.clear()

        h.link.discoverServices()
        h.link.enableStatusNotifications()
        h.link.writeTime(ByteArray(12))
        h.runPosted()

        assertTrue(h.dispatched.isEmpty(), "a closed link still spoke to the machine: ${h.dispatched}")
    }
}
