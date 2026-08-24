package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * The gate that stops a closed GATT client's callbacks reaching the state
 * machine.
 *
 * The scenario every one of these is really about: the machine finishes a
 * successful exchange, emits [Action.CloseConnection] then [Action.ArmAutoConnect],
 * and the `disconnect()` inside that close makes Android deliver one last
 * `onConnectionStateChange(STATE_DISCONNECTED)`. By the time it lands the machine
 * is [ConnectionState.Armed], where PROTOCOL.md §6.2 says a disconnect means
 * `status 133` — so admitting it would turn every successful sync into a sync
 * plus a backoff step.
 */
class ClientEpochTest {

    @Test
    fun `nothing is admitted before the first client is opened`() {
        val epoch = ClientEpoch()
        assertFalse(epoch.isOpen)
        assertEquals(ClientEpoch.NONE, epoch.current)
        assertFalse(epoch.admits(ClientEpoch.NONE))
        assertFalse(epoch.admits(1L))
        assertFalse(epoch.admits(-1L))
    }

    @Test
    fun `the live client is admitted`() {
        val epoch = ClientEpoch()
        val live = epoch.open()
        assertTrue(epoch.isOpen)
        assertEquals(live, epoch.current)
        assertTrue(epoch.admits(live))
    }

    @Test
    fun `a callback that arrives after close is refused`() {
        // The exact case: the disconnect echo of our own close().
        val epoch = ClientEpoch()
        val closedClient = epoch.open()
        epoch.close()

        assertFalse(epoch.isOpen)
        assertFalse(epoch.admits(closedClient), "a closed client's callback must not reach the machine")
    }

    @Test
    fun `re-arming does not resurrect the previous client`() {
        val epoch = ClientEpoch()
        val first = epoch.open()
        epoch.close()
        val second = epoch.open()

        assertTrue(second != first, "a superseded epoch must never be handed out again")
        assertFalse(epoch.admits(first))
        assertTrue(epoch.admits(second))
    }

    @Test
    fun `close is idempotent`() {
        val epoch = ClientEpoch()
        val client = epoch.open()
        epoch.close()
        epoch.close()
        epoch.close()

        assertFalse(epoch.isOpen)
        assertFalse(epoch.admits(client))
        // And a fresh client still works afterwards, so a double close cannot
        // wedge the gate shut.
        assertTrue(epoch.admits(epoch.open()))
    }

    @Test
    fun `NONE is never admitted, not even while a client is open`() {
        // An unstamped callback is the caller's bug. Silence is the safe half of
        // it; treating it as the live client's is the expensive half.
        val epoch = ClientEpoch()
        epoch.open()
        assertFalse(epoch.admits(ClientEpoch.NONE))
    }

    @Test
    fun `opening while open supersedes rather than keeping two live clients`() {
        val epoch = ClientEpoch()
        val leaked = epoch.open()
        val replacement = epoch.open()

        assertFalse(epoch.admits(leaked))
        assertTrue(epoch.admits(replacement))
    }

    @Test
    fun `every epoch across a long run of connections is distinct and only the last is live`() {
        // This process lives for months and the watch opens a window every hour,
        // so "the numbering never repeats" is a property with real mileage on it.
        val epoch = ClientEpoch()
        val issued = mutableSetOf<Long>()
        repeat(10_000) {
            val client = epoch.open()
            assertTrue(issued.add(client), "epoch $client was handed out twice")
            assertTrue(epoch.admits(client))
            epoch.close()
            assertFalse(epoch.admits(client))
        }
        assertEquals(10_000, issued.size)
        assertFalse(issued.contains(ClientEpoch.NONE))
    }
}
