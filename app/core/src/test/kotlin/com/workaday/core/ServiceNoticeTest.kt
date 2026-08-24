package com.workaday.core

import com.workaday.core.protocol.hex
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs

/** What the always-on notification says, and when it changes. */
class ServiceNoticeTest {

    @Test
    fun `every connection state has a notice`() {
        val expected = listOf<Pair<ConnectionState, ServiceNotice>>(
            ConnectionState.Idle to ServiceNotice.Starting,
            ConnectionState.Armed to ServiceNotice.Waiting,
            ConnectionState.WaitingForRetry(30_000) to ServiceNotice.RetryingSoon,
            // The pause after a successful sync reads as "waiting", because that
            // is what it is: nothing wrong, nothing for the user to do.
            ConnectionState.Settling(10_000) to ServiceNotice.Waiting,
            ConnectionState.Discovering to ServiceNotice.Exchanging,
            ConnectionState.EnablingNotifications to ServiceNotice.Exchanging,
            ConnectionState.WritingTime to ServiceNotice.Exchanging,
            ConnectionState.AwaitingStatus to ServiceNotice.Exchanging,
            ConnectionState.Blocked(adapterOff = true, permissionMissing = false) to ServiceNotice.BluetoothOff,
        )
        for ((state, notice) in expected) {
            assertEquals(notice, serviceNoticeFor(state), "$state")
        }
        // Every StateLabel is covered above, so a ninth ConnectionState cannot be
        // added without this list going stale in a way that is visible.
        assertEquals(StateLabel.entries.size, expected.map { it.first.label() }.toSet().size)
    }

    @Test
    fun `a missing permission outranks a switched-off adapter`() {
        // Naming the adapter first would send the user to turn Bluetooth on, watch
        // nothing happen, and conclude the app is broken.
        assertEquals(
            ServiceNotice.PermissionMissing,
            serviceNoticeFor(ConnectionState.Blocked(adapterOff = true, permissionMissing = true)),
        )
        assertEquals(
            ServiceNotice.PermissionMissing,
            serviceNoticeFor(ConnectionState.Blocked(adapterOff = false, permissionMissing = true)),
        )
    }

    @Test
    fun `the notice a real run produces, event by event`() {
        // The mapping is trivial; that it is driven by a state the machine
        // actually reaches is the part worth pinning.
        val clock = FakeClock()
        val machine = ConnectionStateMachine(clock, Backoff(JitterSource.NONE))
        val okStatus = hex("01 81 00 4E F0 FF 82 6A 01 00 00 00")

        assertEquals(ServiceNotice.Starting, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.Started)
        assertEquals(ServiceNotice.Waiting, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.Connected(GATT_SUCCESS))
        assertEquals(ServiceNotice.Exchanging, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.ServicesDiscovered(GATT_SUCCESS))
        machine.onEvent(TransportEvent.DescriptorWritten(GATT_SUCCESS))
        machine.onEvent(TransportEvent.CharacteristicWritten(GATT_SUCCESS))
        assertEquals(ServiceNotice.Exchanging, serviceNoticeFor(machine.state))

        val settle = machine.onEvent(TransportEvent.NotificationReceived(okStatus))
        assertEquals(ServiceNotice.Waiting, serviceNoticeFor(machine.state), "a sync ends back at rest")

        // A disconnect during the settle is the echo of our own hang-up and means
        // nothing. Only once the app is armed again does one mean the pending
        // autoConnect failed — which is the §6.2 row the rest of this test walks.
        assertIs<ConnectionState.Settling>(machine.state)
        machine.onEvent(TransportEvent.Disconnected(GATT_SUCCESS))
        assertEquals(ServiceNotice.Waiting, serviceNoticeFor(machine.state), "our own hang-up is not a fault")

        val settleTimer = settle.filterIsInstance<Action.ArmOperationTimeout>().single()
        machine.onEvent(TransportEvent.OperationTimedOut(settleTimer.token))
        assertEquals(ServiceNotice.Waiting, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.Disconnected(133))
        assertEquals(ServiceNotice.RetryingSoon, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.AdapterOff)
        assertEquals(ServiceNotice.BluetoothOff, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.PermissionRevoked)
        assertEquals(ServiceNotice.PermissionMissing, serviceNoticeFor(machine.state))

        machine.onEvent(TransportEvent.AdapterOn)
        machine.onEvent(TransportEvent.PermissionGranted)
        assertEquals(ServiceNotice.Waiting, serviceNoticeFor(machine.state))
    }
}
