package com.workaday.core

/**
 * Which GATT client the app is listening to right now.
 *
 * [Action.CloseConnection] says `disconnect()`, then `close()`, then **deliver no
 * further callbacks from that client**. `close()` unregisters the client, so the
 * platform stops calling — but "stops calling" and "nothing is already in flight"
 * are different claims. A callback dispatched on a binder thread a microsecond
 * before `close()` is still on its way, and the Android layer hands events to the
 * machine through a queue, so it arrives *after* the close has been performed.
 *
 * That one stale delivery is expensive. [ConnectionStateMachine] reads a
 * [TransportEvent.Disconnected] while [ConnectionState.Armed] as PROTOCOL.md
 * §6.2's `status 133` row — and `disconnect()` is precisely what makes the
 * platform emit one. Route it back in and **every successful sync is followed by
 * a spurious failure and a backoff step**: the app would work, look like it was
 * working, and quietly spend its life one rung up the ladder.
 *
 * So each client gets an epoch when it is opened, every callback carries the
 * epoch of the client that produced it, and only the live one is admitted. This
 * is a decision, which is why it is here and not in the GATT callback (Law 3).
 *
 * Not thread-safe, deliberately, and for the same reason [ConnectionStateMachine]
 * is not: the Android layer owns both from one serialising context. Callbacks
 * arrive on binder threads and are *posted* to that context; [admits] is then
 * asked on the same thread that ran [open] and [close], which is what makes the
 * answer meaningful rather than a race.
 */
class ClientEpoch {

    /** Monotonic, so a number handed out once is never handed out again. */
    private var lastIssued: Long = NONE

    /** The epoch of the live client, or [NONE] when no client is open. */
    var current: Long = NONE
        private set

    /** True between [open] and [close]. */
    val isOpen: Boolean get() = current != NONE

    /**
     * Register a new client and return its epoch.
     *
     * Opening while one is already open supersedes it — the older epoch stops
     * being admitted from this moment. [ConnectionStateMachine] never asks for
     * that (it emits [Action.CloseConnection] before every [Action.ArmAutoConnect]),
     * but if the Android layer ever did, refusing the older client's callbacks is
     * the safe half of the mistake.
     */
    fun open(): Long {
        lastIssued++
        current = lastIssued
        return current
    }

    /** Stop admitting the live client. Idempotent. */
    fun close() {
        current = NONE
    }

    /**
     * Whether a callback stamped [epoch] still belongs to the live client.
     *
     * [NONE] is never admitted, so a caller that forgets to stamp its callbacks
     * gets silence rather than a stale link's events treated as the current one's.
     */
    fun admits(epoch: Long): Boolean = epoch != NONE && epoch == current

    companion object {
        /** "No client". Never a valid epoch, so it can never be admitted. */
        const val NONE: Long = 0L
    }
}
