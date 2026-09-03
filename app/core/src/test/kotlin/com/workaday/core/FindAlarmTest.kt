package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/** The find-phone alarm's volume curve: quiet at first, full at the end, never past either. */
class FindAlarmTest {

    @Test
    fun `starts at the start volume and ends at full`() {
        assertEquals(FindAlarm.START_VOLUME, FindAlarm.volumeAt(0))
        assertEquals(1.0f, FindAlarm.volumeAt(FindAlarm.RAMP_MS))
    }

    @Test
    fun `halfway through the ramp is halfway between`() {
        val expected = FindAlarm.START_VOLUME + (1.0f - FindAlarm.START_VOLUME) / 2f
        assertEquals(expected, FindAlarm.volumeAt(FindAlarm.RAMP_MS / 2), 0.001f)
    }

    @Test
    fun `never rises past full, however long it rings`() {
        for (elapsed in listOf(FindAlarm.RAMP_MS + 1, FindAlarm.RAMP_MS * 10, Long.MAX_VALUE)) {
            assertEquals(1.0f, FindAlarm.volumeAt(elapsed), "at $elapsed ms")
        }
    }

    @Test
    fun `a clock that stepped backwards is the start, not silence or a crash`() {
        assertEquals(FindAlarm.START_VOLUME, FindAlarm.volumeAt(-1))
        assertEquals(FindAlarm.START_VOLUME, FindAlarm.volumeAt(Long.MIN_VALUE))
    }

    @Test
    fun `monotonic and inside the player's range at every step the app will take`() {
        var previous = 0f
        var elapsed = 0L
        while (elapsed <= FindAlarm.RAMP_MS + FindAlarm.STEP_MS) {
            val volume = FindAlarm.volumeAt(elapsed)
            assertTrue(volume >= previous, "volume fell from $previous to $volume at $elapsed ms")
            assertTrue(volume in FindAlarm.START_VOLUME..1.0f, "volume $volume out of range at $elapsed ms")
            previous = volume
            elapsed += FindAlarm.STEP_MS
        }
    }

    @Test
    fun `the start is audible and the ramp is neither instant nor endless`() {
        // The brief: starts quiet, rises to full. Silent is not quiet, and a ramp
        // shorter than the step would be a jump; one longer than the watch's whole
        // search would never reach full while there was still a search to hear.
        assertTrue(FindAlarm.START_VOLUME > 0f)
        assertTrue(FindAlarm.START_VOLUME < 1f)
        assertTrue(FindAlarm.RAMP_MS >= FindAlarm.STEP_MS)
        assertTrue(FindAlarm.RAMP_MS < com.workaday.core.protocol.WatchProtocol.WATCH_FIND_PHONE_TIMEOUT_MS)
    }
}
