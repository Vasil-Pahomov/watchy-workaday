package com.workaday.core.protocol

import java.time.Instant
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The mechanism that stops the app and the firmware drifting apart.
 *
 * The vectors below are **transcribed from PROTOCOL.md §7**, not round-tripped
 * through our own encoder: a test that encodes and then decodes with the same
 * code passes just as happily when both halves are wrong together. Every
 * expected byte string here appears verbatim in the document, and the firmware
 * has a test containing the identical bytes.
 */
class WatchProtocolTest {

    // ── §7 golden vectors, transcribed ───────────────────────────────────────

    /** §7.1 input: `2026-08-17T12:34:56Z`, UTC offset +180 minutes. */
    private val goldenEpochSeconds = 1_786_970_096L
    private val goldenOffsetMinutes = 180

    /** §7.1, the encoded 12 bytes exactly as the document prints them. */
    private val goldenTime = hex("01 01 F0 FF 82 6A B4 00 00 00 00 00")

    /** §7.2: result Ok, battery 78 %, applied epoch as above, fw_build 1. */
    private val goldenStatus = hex("01 81 00 4E F0 FF 82 6A 01 00 00 00")

    @Test
    fun `hex helper is itself correct`() {
        // The transcription helper is the one thing between the document and the
        // assertions, so it gets pinned against explicit byte literals. 0x81 and
        // 0x4E are the interesting cases: one is negative as a Kotlin Byte, one
        // is not.
        assertContentEquals(byteArrayOf(0x01, -127, 0x00, 0x4E), hex("01 81 00 4E"))
        assertEquals(12, goldenTime.size)
        assertEquals(12, goldenStatus.size)
    }

    @Test
    fun `golden vector 7_1 - the document's epoch really is the document's instant`() {
        // Checks the document's own arithmetic with an independent implementation
        // before anything is built on it. If PROTOCOL.md ever mistypes the epoch,
        // this fails here rather than on hardware.
        assertEquals(goldenEpochSeconds, Instant.parse("2026-08-17T12:34:56Z").epochSecond)
        assertEquals(goldenEpochSeconds, 0x6A82FFF0L)
        assertEquals(goldenOffsetMinutes, 0x00B4)
    }

    @Test
    fun `golden vector 7_1 - Time encodes byte for byte`() {
        assertContentEquals(
            goldenTime,
            WatchProtocol.encodeTime(goldenEpochSeconds, goldenOffsetMinutes),
        )
    }

    @Test
    fun `golden vector 7_2 - Status decodes to the document's field values`() {
        val decoded = WatchProtocol.decodeStatus(goldenStatus)
        val status = assertIs<StatusDecode.Valid>(decoded).status

        assertEquals(SyncResult.Ok, status.result)
        assertEquals(0, status.resultCode)
        assertTrue(status.isSuccess)
        assertEquals(78, status.batteryPercent)
        assertEquals(goldenEpochSeconds, status.appliedUtcEpochSeconds)
        assertEquals(1, status.fwBuild)
    }

    @Test
    fun `golden vector 7_3 - the two Time rows our encoder can produce`() {
        // §7.3 is the watch's rejection table and the firmware owns it: the app
        // never decodes a Time payload. But two of its rows are payloads this
        // encoder can emit, so they are a genuine cross-check that both sides
        // agree on the layout at values the happy path never reaches — a zero
        // epoch, and an offset large enough to exercise the second byte of the
        // i16.
        assertContentEquals(
            hex("01 01 00 00 00 00 B4 00 00 00 00 00"),
            WatchProtocol.encodeTime(0, 180),
        )
        assertContentEquals(
            hex("01 01 F0 FF 82 6A 59 03 00 00 00 00"),
            WatchProtocol.encodeTime(goldenEpochSeconds, 857),
        )
    }

    // ── Identity (§2.1) ──────────────────────────────────────────────────────

    @Test
    fun `UUIDs match the section 2_1 table`() {
        assertEquals("57444159-6461-4779-b0a3-1f4c7e25d908", WatchProtocol.SYNC_SERVICE_UUID.toString())
        assertEquals("57444101-6461-4779-b0a3-1f4c7e25d908", WatchProtocol.TIME_CHARACTERISTIC_UUID.toString())
        assertEquals("57444102-6461-4779-b0a3-1f4c7e25d908", WatchProtocol.STATUS_CHARACTERISTIC_UUID.toString())
        assertEquals("57444103-6461-4779-b0a3-1f4c7e25d908", WatchProtocol.FIND_CHARACTERISTIC_UUID.toString())
        assertEquals("00002902-0000-1000-8000-00805f9b34fb", WatchProtocol.CCCD_UUID.toString())
    }

    @Test
    fun `the five UUIDs are distinct`() {
        val all = setOf(
            WatchProtocol.SYNC_SERVICE_UUID,
            WatchProtocol.TIME_CHARACTERISTIC_UUID,
            WatchProtocol.STATUS_CHARACTERISTIC_UUID,
            WatchProtocol.FIND_CHARACTERISTIC_UUID,
            WatchProtocol.CCCD_UUID,
        )
        assertEquals(5, all.size)
    }

    // ── §3.3 / §4.1, the find-phone additions ────────────────────────────────

    @Test
    fun `golden vector 7_5 - FindDismiss encodes byte for byte, a fresh array each call`() {
        assertContentEquals(hex("01 02 00 00"), WatchProtocol.encodeFindDismiss())
        assertEquals(WatchProtocol.FIND_PAYLOAD_LENGTH, WatchProtocol.encodeFindDismiss().size)
        val first = WatchProtocol.encodeFindDismiss()
        first[0] = 0x7F
        assertContentEquals(hex("01 02 00 00"), WatchProtocol.encodeFindDismiss())
    }

    @Test
    fun `golden vector 7_4 - a Status carrying FIND_PHONE decodes to the 7_2 fields plus the flag`() {
        val plain = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(goldenStatus)).status
        val flagged = assertIs<StatusDecode.Valid>(
            WatchProtocol.decodeStatus(hex("01 81 00 4E F0 FF 82 6A 01 00 01 00")),
        ).status

        assertFalse(plain.findPhoneRequested, "the sync window's frame never asks the phone to ring")
        assertTrue(flagged.findPhoneRequested)
        // Everything else is §7.2, byte for byte — which is what rule 2's exception
        // requires of a receiver that ignores the byte.
        assertEquals(plain.resultCode, flagged.resultCode)
        assertEquals(plain.batteryPercent, flagged.batteryPercent)
        assertEquals(plain.appliedUtcEpochSeconds, flagged.appliedUtcEpochSeconds)
        assertEquals(plain.fwBuild, flagged.fwBuild)
        assertTrue(flagged.isSuccess, "the flag says nothing about result")
    }

    @Test
    fun `FIND_PHONE is independent of result`() {
        // §4.1: a clock that could not be set is no reason to leave the phone lost.
        val busyAndFinding = hex("01 81 06 4E 00 00 00 00 01 00 01 00")
        val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(busyAndFinding)).status
        assertTrue(status.findPhoneRequested)
        assertFalse(status.isSuccess)
        assertEquals(SyncResult.Busy, status.result)
    }

    @Test
    fun `golden vector 7_4 - a Status carrying FIND_PHONE and FIND_SOUND asks for the tone as well`() {
        // §7.4's second vector: the wearer pressed Menu before the phone connected.
        val plain = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(goldenStatus)).status
        val vibrating = assertIs<StatusDecode.Valid>(
            WatchProtocol.decodeStatus(hex("01 81 00 4E F0 FF 82 6A 01 00 01 00")),
        ).status
        val sounding = assertIs<StatusDecode.Valid>(
            WatchProtocol.decodeStatus(hex("01 81 00 4E F0 FF 82 6A 01 00 03 00")),
        ).status

        assertFalse(plain.findSoundRequested)
        assertFalse(vibrating.findSoundRequested, "§4.1: a search starts vibration-only")
        assertTrue(sounding.findPhoneRequested)
        assertTrue(sounding.findSoundRequested)
        // Byte 10 is all that differs from §7.2.
        assertEquals(plain.resultCode, sounding.resultCode)
        assertEquals(plain.batteryPercent, sounding.batteryPercent)
        assertEquals(plain.appliedUtcEpochSeconds, sounding.appliedUtcEpochSeconds)
        assertEquals(plain.fwBuild, sounding.fwBuild)
        assertTrue(sounding.isSuccess)
    }

    @Test
    fun `only bits 0 and 1 of flags are read - the reserved bits cannot ring the phone`() {
        // Bits 2..7 are reserved, and a later v1.x may give one meaning this build
        // does not know. Every byte with bit 0 clear must read as "not finding",
        // every byte with bit 1 clear as "no tone", whatever else is in it.
        for (flags in 0..255) {
            val payload = goldenStatus.copyOf().also { it[10] = flags.toByte() }
            val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status
            assertEquals((flags and 0x01) != 0, status.findPhoneRequested, "flags 0x%02X".format(flags))
            assertEquals((flags and 0x02) != 0, status.findSoundRequested, "flags 0x%02X".format(flags))
        }
        assertEquals(0x01, WatchProtocol.STATUS_FLAG_FIND_PHONE)
        assertEquals(0x02, WatchProtocol.STATUS_FLAG_FIND_SOUND)
    }

    @Test
    fun `section 3_3 constants`() {
        assertEquals(0x02, WatchProtocol.MSG_TYPE_FIND_DISMISS)
        assertEquals(0x83, WatchProtocol.MSG_TYPE_FIND_MODE)
        assertEquals(0x01, WatchProtocol.FIND_MODE_SOUND)
        assertEquals(4, WatchProtocol.FIND_PAYLOAD_LENGTH)
        // Four message types, all distinct; the high bit marks the watch's two.
        val types = setOf(
            WatchProtocol.MSG_TYPE_SET_TIME,
            WatchProtocol.MSG_TYPE_FIND_DISMISS,
            WatchProtocol.MSG_TYPE_SYNC_RESULT,
            WatchProtocol.MSG_TYPE_FIND_MODE,
        )
        assertEquals(4, types.size)
    }

    @Test
    fun `golden vector 7_7 - FindMode decodes to the document's two modes`() {
        assertEquals(FindMode(sound = true), WatchProtocol.decodeFindMode(hex("01 83 01 00")))
        assertEquals(FindMode(sound = false), WatchProtocol.decodeFindMode(hex("01 83 00 00")))
        // Bits 1–7 of `mode` are reserved and ignored, as is the reserved byte.
        assertEquals(FindMode(sound = true), WatchProtocol.decodeFindMode(hex("01 83 FF 00")))
        assertEquals(FindMode(sound = false), WatchProtocol.decodeFindMode(hex("01 83 FE 00")))
        assertEquals(FindMode(sound = true), WatchProtocol.decodeFindMode(hex("01 83 01 5A")))
    }

    @Test
    fun `golden vector 7_8 - anything that is not a FindMode is null, not a crash and not a mode`() {
        // §3.3: an invalid frame is ignored and the phone keeps doing what it was
        // doing — so the decoder answers "not a mode", never throws, and never
        // reads a mode out of bytes that only look like one.
        val ignored = listOf<Pair<String, ByteArray?>>(
            "null" to null,
            "empty" to ByteArray(0),
            "3 bytes" to hex("01 83 01"),
            "5 bytes" to hex("01 83 01 00 00"),
            "wrong version" to hex("02 83 01 00"),
            "a Status frame" to hex("01 81 00 4E F0 FF 82 6A 01 00 01 00"),
            "our own FindDismiss echoed" to hex("01 02 00 00"),
            "a Time msg_type" to hex("01 01 01 00"),
        )
        for ((name, payload) in ignored) {
            assertNull(WatchProtocol.decodeFindMode(payload), name)
        }
    }

    @Test
    fun `decodeFindMode does not mutate the payload it was given`() {
        val payload = hex("01 83 01 00")
        WatchProtocol.decodeFindMode(payload)
        assertContentEquals(hex("01 83 01 00"), payload)
    }

    @Test
    fun `the ring backstop outlives the watch's find cap`() {
        // §5.2: "Longer than the watch's 120 s find cap, so the watch's hang-up is
        // the normal ending and this is the safety net behind a disconnect that
        // never arrived." If the two ever crossed, the phone would hang up on a
        // watch that was still searching, reconnect at once, and ring again.
        assertEquals(120_000L, WatchProtocol.WATCH_FIND_PHONE_TIMEOUT_MS)
        assertEquals(135_000L, WatchProtocol.FIND_RING_BACKSTOP_MS)
        assertTrue(WatchProtocol.FIND_RING_BACKSTOP_MS > WatchProtocol.WATCH_FIND_PHONE_TIMEOUT_MS)
    }

    @Test
    fun `CCCD enable value is 0x0001 little-endian and a fresh array each call`() {
        assertContentEquals(hex("01 00"), WatchProtocol.cccdEnableNotificationValue())
        val first = WatchProtocol.cccdEnableNotificationValue()
        first[0] = 0x7F
        assertContentEquals(hex("01 00"), WatchProtocol.cccdEnableNotificationValue())
    }

    @Test
    fun `section 5_2 timing constants`() {
        assertEquals(5_000L, WatchProtocol.OPERATION_TIMEOUT_MS)
        assertEquals(15_000L, WatchProtocol.EXCHANGE_TIMEOUT_MS)
        assertEquals(30_000L, WatchProtocol.BACKOFF_BASE_MS)
        assertEquals(900_000L, WatchProtocol.BACKOFF_CAP_MS)
        assertEquals(0.20, WatchProtocol.BACKOFF_JITTER_FRACTION)
    }

    @Test
    fun `section 5_1 sync window interval, the one watch-side number the phone mirrors`() {
        // 60 min, in seconds because everything it is compared against is an epoch
        // in seconds. The diagnostic screen's "that is too long ago" is a multiple
        // of this and of nothing else, so a second copy of "an hour" living beside
        // a text view is what this pins shut (§8).
        assertEquals(3_600L, WatchProtocol.SYNC_WINDOW_INTERVAL_SECONDS)
    }

    @Test
    fun `the phone's exchange budget outlives the watch's session cap`() {
        // §5.1's absolute session cap, and §5.2's whole-exchange timeout chosen to
        // sit behind it: "longer than the watch's 12 s cap, so the watch's own
        // teardown is the normal ending and the phone's timeout is the backstop."
        assertEquals(12_000L, WatchProtocol.WATCH_SESSION_CAP_MS)
        assertTrue(
            WatchProtocol.EXCHANGE_TIMEOUT_MS > WatchProtocol.WATCH_SESSION_CAP_MS,
            "ConnectionState.Settling waits out the remainder of the exchange budget after a success, " +
                "and that only closes the watch's window if the budget is the longer of the two - " +
                "otherwise the app re-arms into the window it just used and syncs it again",
        )
    }

    @Test
    fun `BadVersion is the only code that retrying cannot fix`() {
        // §6.2, verbatim: "BadVersion in particular means the two sides have
        // drifted and retrying will not help — show it, do not hot-loop." Every
        // other code describes this attempt, and the watch's next window is a
        // fresh one.
        assertFalse(SyncResult.BadVersion.retryingWillHelp)
        for (result in SyncResult.entries - SyncResult.BadVersion) {
            assertTrue(result.retryingWillHelp, "$result")
        }
    }

    // ── Time encoding boundaries ─────────────────────────────────────────────

    @Test
    fun `encodeTime always produces exactly 12 bytes, at both ends of the u32`() {
        assertEquals(12, WatchProtocol.encodeTime(0, 0).size)
        assertEquals(12, WatchProtocol.encodeTime(0xFFFF_FFFFL, 0).size)
        assertContentEquals(hex("01 01 FF FF FF FF 00 00 00 00 00 00"), WatchProtocol.encodeTime(0xFFFF_FFFFL, 0))
    }

    @Test
    fun `a negative UTC offset is encoded as a two's complement i16`() {
        // -300 minutes = UTC-5. 0xFED4 little-endian is D4 FE. The watch reads an
        // i16 here, so getting the sign wrong would put it five hours out with no
        // error reported by either side.
        assertContentEquals(
            hex("01 01 F0 FF 82 6A D4 FE 00 00 00 00"),
            WatchProtocol.encodeTime(goldenEpochSeconds, -300),
        )
    }

    @Test
    fun `flags and reserved are written as zero`() {
        val encoded = WatchProtocol.encodeTime(0xFFFF_FFFFL, -32768)
        assertContentEquals(ByteArray(4), encoded.copyOfRange(8, 12))
    }

    @Test
    fun `isEncodableTime rejects what the u32 and i16 cannot carry`() {
        assertTrue(WatchProtocol.isEncodableTime(goldenEpochSeconds, goldenOffsetMinutes))
        assertTrue(WatchProtocol.isEncodableTime(0, 0))
        assertTrue(WatchProtocol.isEncodableTime(0xFFFF_FFFFL, 32767))
        assertTrue(WatchProtocol.isEncodableTime(0, -32768))

        // A phone whose clock was set before 1970, or past 2106.
        assertFalse(WatchProtocol.isEncodableTime(-1, 0))
        assertFalse(WatchProtocol.isEncodableTime(0x1_0000_0000L, 0))
        assertFalse(WatchProtocol.isEncodableTime(goldenEpochSeconds, 32768))
        assertFalse(WatchProtocol.isEncodableTime(goldenEpochSeconds, -32769))
    }

    @Test
    fun `encodeTime refuses to truncate rather than emitting a wrong time`() {
        assertFailsWith<IllegalArgumentException> { WatchProtocol.encodeTime(-1, 0) }
        assertFailsWith<IllegalArgumentException> { WatchProtocol.encodeTime(0x1_0000_0000L, 0) }
        assertFailsWith<IllegalArgumentException> { WatchProtocol.encodeTime(0, 40_000) }
    }

    @Test
    fun `encodeTime hands out a fresh array`() {
        val first = WatchProtocol.encodeTime(goldenEpochSeconds, goldenOffsetMinutes)
        first[0] = 0x7F
        assertContentEquals(goldenTime, WatchProtocol.encodeTime(goldenEpochSeconds, goldenOffsetMinutes))
    }

    // ── Status decoding boundaries ───────────────────────────────────────────

    @Test
    fun `a null payload is a wrong length, not a crash`() {
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(null))
        assertEquals(StatusRejection.WrongLength, rejected.reason)
        assertEquals(-1, rejected.observed)
    }

    @Test
    fun `an empty payload is rejected`() {
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(ByteArray(0)))
        assertEquals(StatusRejection.WrongLength, rejected.reason)
        assertEquals(0, rejected.observed)
    }

    @Test
    fun `a truncated payload is rejected without being parsed`() {
        val truncated = goldenStatus.copyOfRange(0, 11)
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(truncated))
        assertEquals(StatusRejection.WrongLength, rejected.reason)
        assertEquals(11, rejected.observed)
    }

    @Test
    fun `an oversized payload is rejected rather than having its tail ignored`() {
        val oversized = goldenStatus + byteArrayOf(0x00)
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(oversized))
        assertEquals(StatusRejection.WrongLength, rejected.reason)
        assertEquals(13, rejected.observed)
    }

    @Test
    fun `every length from 0 to 32 except 12 is rejected`() {
        for (size in 0..32) {
            if (size == WatchProtocol.STATUS_PAYLOAD_LENGTH) continue
            val padded = ByteArray(size) { i -> goldenStatus.getOrElse(i) { 0 } }
            assertIs<StatusDecode.Rejected>(
                WatchProtocol.decodeStatus(padded),
                "length $size should be rejected",
            )
        }
    }

    @Test
    fun `an unsupported proto_version is rejected`() {
        val wrongVersion = hex("02 81 00 4E F0 FF 82 6A 01 00 00 00")
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(wrongVersion))
        assertEquals(StatusRejection.UnsupportedVersion, rejected.reason)
        assertEquals(2, rejected.observed)
    }

    @Test
    fun `an unexpected msg_type is rejected`() {
        // 0x01 is SetTime — our own outgoing type echoed back, which is exactly
        // the sort of loopback a broken bridge produces.
        val wrongType = hex("01 01 00 4E F0 FF 82 6A 01 00 00 00")
        val rejected = assertIs<StatusDecode.Rejected>(WatchProtocol.decodeStatus(wrongType))
        assertEquals(StatusRejection.UnexpectedMessageType, rejected.reason)
        assertEquals(1, rejected.observed)
    }

    @Test
    fun `every result code in the section 3_2 table decodes to its name`() {
        val expected = listOf(
            0 to SyncResult.Ok,
            1 to SyncResult.BadLength,
            2 to SyncResult.BadVersion,
            3 to SyncResult.OutOfRange,
            4 to SyncResult.RtcWriteFailed,
            5 to SyncResult.BadType,
            6 to SyncResult.Busy,
        )
        for ((code, name) in expected) {
            val payload = goldenStatus.copyOf().also { it[2] = code.toByte() }
            val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status
            assertEquals(name, status.result, "code $code")
            assertEquals(code, status.resultCode)
            assertEquals(code == 0, status.isSuccess, "only Ok is a success (code $code)")
        }
        assertEquals(7, SyncResult.entries.size, "a new result code needs a PROTOCOL.md change first")
    }

    @Test
    fun `an unknown result code keeps the frame but is never a success`() {
        for (code in 7..255) {
            val payload = goldenStatus.copyOf().also { it[2] = code.toByte() }
            val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status
            assertNull(status.result, "code $code should not map to a known result")
            assertEquals(code, status.resultCode)
            assertFalse(status.isSuccess, "code $code must not be mistaken for Ok")
            // The rest of the frame is still readable for the diagnostic screen.
            assertEquals(78, status.batteryPercent)
        }
    }

    @Test
    fun `battery 0xFF is unknown, not minus one`() {
        // Kotlin's Byte is signed: an unmasked read turns 0xFF into -1 and a
        // careless UI shows "-1 %".
        val payload = goldenStatus.copyOf().also { it[3] = 0xFF.toByte() }
        val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status
        assertNull(status.batteryPercent)
        assertEquals(0xFF, WatchProtocol.BATTERY_PERCENT_UNKNOWN)
    }

    @Test
    fun `battery 0 and 100 are real readings, 101 to 254 are treated as unknown`() {
        fun battery(raw: Int): Int? {
            val payload = goldenStatus.copyOf().also { it[3] = raw.toByte() }
            return assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status.batteryPercent
        }
        assertEquals(0, battery(0))
        assertEquals(100, battery(100))
        // Section 3.2 promises the watch normalises these to 0xFF. We hold it to
        // that promise instead of trusting it, so a garbled sample can never
        // reach the UI as "200 %".
        for (raw in 101..254) assertNull(battery(raw), "raw $raw")
    }

    @Test
    fun `applied epoch reads the full unsigned u32 range`() {
        val maxEpoch = goldenStatus.copyOf().also {
            it[4] = 0xFF.toByte(); it[5] = 0xFF.toByte(); it[6] = 0xFF.toByte(); it[7] = 0xFF.toByte()
        }
        val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(maxEpoch)).status
        assertEquals(0xFFFF_FFFFL, status.appliedUtcEpochSeconds)

        val zeroEpoch = goldenStatus.copyOf().also {
            it[4] = 0; it[5] = 0; it[6] = 0; it[7] = 0
        }
        assertEquals(0L, assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(zeroEpoch)).status.appliedUtcEpochSeconds)
    }

    @Test
    fun `fw_build reads the full unsigned u16 range`() {
        val payload = goldenStatus.copyOf().also { it[8] = 0xFF.toByte(); it[9] = 0xFF.toByte() }
        val status = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(payload)).status
        assertEquals(65535, status.fwBuild)
    }

    @Test
    fun `reserved bytes are ignored, so a later v1_x can use them`() {
        // Offset 10 was the proof of this rule — it became `flags` on 3 Sep 2026
        // without a version bump — so the bits that are still reserved are what is
        // exercised now: flags bits 2..7 (0x58 has bits 0 and 1 clear) and offset 11.
        val withReserved = goldenStatus.copyOf().also { it[10] = 0x58; it[11] = 0xA5.toByte() }
        val a = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(goldenStatus)).status
        val b = assertIs<StatusDecode.Valid>(WatchProtocol.decodeStatus(withReserved)).status
        assertEquals(a.resultCode, b.resultCode)
        assertEquals(a.batteryPercent, b.batteryPercent)
        assertEquals(a.appliedUtcEpochSeconds, b.appliedUtcEpochSeconds)
        assertEquals(a.fwBuild, b.fwBuild)
        assertEquals(a.findPhoneRequested, b.findPhoneRequested)
        assertEquals(a.findSoundRequested, b.findSoundRequested)
    }

    @Test
    fun `decodeStatus does not mutate the payload it was given`() {
        val payload = goldenStatus.copyOf()
        WatchProtocol.decodeStatus(payload)
        assertContentEquals(goldenStatus, payload)
    }

    @Test
    fun `the two message types are distinct and the version is 1`() {
        assertEquals(1, WatchProtocol.PROTO_VERSION)
        assertEquals(0x01, WatchProtocol.MSG_TYPE_SET_TIME)
        assertEquals(0x81, WatchProtocol.MSG_TYPE_SYNC_RESULT)
        assertNotEquals(WatchProtocol.MSG_TYPE_SET_TIME, WatchProtocol.MSG_TYPE_SYNC_RESULT)
        assertEquals(12, WatchProtocol.TIME_PAYLOAD_LENGTH)
        assertEquals(12, WatchProtocol.STATUS_PAYLOAD_LENGTH)
    }
}

/** Transcribes a PROTOCOL.md byte row, e.g. `"01 81 00 4E …"`, unchanged. */
internal fun hex(row: String): ByteArray =
    row.trim().split(" ").map { it.toInt(16).toByte() }.toByteArray()
