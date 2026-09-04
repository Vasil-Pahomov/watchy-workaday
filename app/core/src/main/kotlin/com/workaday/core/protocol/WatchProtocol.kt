package com.workaday.core.protocol

import java.util.UUID

/**
 * The app's half of the watch ⇄ phone contract.
 *
 * **This file is a mirror, not a design.** `../PROTOCOL.md` at the top of the
 * Workaday tree is the single source of truth, and the firmware mirrors the same
 * document in `src/core/protocol.h`. Nothing here may be invented, extended or
 * tidied up locally: the two codebases never see each other and are tested
 * separately, so a silent deviation on one side shows up only on real hardware —
 * as a watch that quietly rejects every sync, or worse, accepts one and sets the
 * clock wrong.
 *
 * PROTOCOL.md §8 makes that concrete, and `CLAUDE.md` repeats it: this is the
 * **only** file in the app permitted to contain a UUID literal, a wire-format
 * field offset, or one of the §5.2 timeouts. A UUID appearing in `app/` is the
 * defect, not a convenience — the Android layer references the properties below.
 *
 * Note the asymmetry with the firmware. The watch only ever *decodes* Time and
 * *encodes* Status; the phone does the opposite. So the suspicious half here is
 * [decodeStatus], and it is as unwilling to trust the watch's bytes as the
 * firmware's decoder is to trust the phone's.
 *
 * Why this lives in `core/` rather than beside the GATT callback: deciding that
 * twelve bytes are trustworthy is a *decision*, and decisions are the things that
 * get a unit test (Law 3). `WatchProtocolTest` replays the golden vectors of
 * PROTOCOL.md §7 byte for byte — that test, not the prose, is what actually keeps
 * the two implementations from drifting apart.
 */
object WatchProtocol {

    // ── §2.1 Identity — fixed forever ────────────────────────────────────────
    //
    // `0x57444159` is 'WDAY' and `…01` / `…02` continue the family. That is a
    // mnemonic, not a mechanism: new UUIDs are not derived by incrementing, they
    // are added to the table in PROTOCOL.md first.
    //
    // Exposed as java.util.UUID rather than String so the Android layer never has
    // to call UUID.fromString on a literal of its own. java.util.UUID is a plain
    // JDK type — not `android.*` — so it is admissible in this module (Law 3).

    /** Service — Workaday Sync. */
    val SYNC_SERVICE_UUID: UUID = UUID.fromString("57444159-6461-4779-b0a3-1f4c7e25d908")

    /** Characteristic — Time. Phone → watch, write **with response**. */
    val TIME_CHARACTERISTIC_UUID: UUID = UUID.fromString("57444101-6461-4779-b0a3-1f4c7e25d908")

    /** Characteristic — Status. Watch → phone, read + notify. */
    val STATUS_CHARACTERISTIC_UUID: UUID = UUID.fromString("57444102-6461-4779-b0a3-1f4c7e25d908")

    /**
     * Characteristic — Find. Phone → watch, write **with response** (§3.3): the
     * one frame the phone sends when the user silences a find-phone alarm on the
     * phone (§4.1). Added without a `PROTO_VERSION` bump; an older watch simply
     * lacks it, and the write then fails locally (§6.2).
     */
    val FIND_CHARACTERISTIC_UUID: UUID = UUID.fromString("57444103-6461-4779-b0a3-1f4c7e25d908")

    /** The SIG-standard Client Characteristic Configuration descriptor on Status. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // ── §3 Wire format ───────────────────────────────────────────────────────
    //
    // Little-endian throughout. Both payloads are a fixed 12 bytes, which is what
    // lets them ride the default 23-byte ATT MTU with no MTU exchange — one fewer
    // GATT operation, one fewer timeout, one fewer failure mode, for no gain
    // (§9 forbids adding one). A wrong length is rejected without being parsed:
    // the decoder never reads past what it was handed and never guesses at a
    // truncated packet.

    /** §3.1 / §3.2 `proto_version`. Bumped by any change to a layout or a meaning. */
    const val PROTO_VERSION: Int = 0x01

    /** §3.1 `msg_type` for a Time write. */
    const val MSG_TYPE_SET_TIME: Int = 0x01

    /** §3.3 `msg_type` for a Find write: FindDismiss. */
    const val MSG_TYPE_FIND_DISMISS: Int = 0x02

    /** §3.2 `msg_type` for a Status frame. */
    const val MSG_TYPE_SYNC_RESULT: Int = 0x81

    /** §3.3 `msg_type` for a Find notify: FindMode, watch → phone. */
    const val MSG_TYPE_FIND_MODE: Int = 0x83

    /** §3.1. Exactly this, never more, never less. */
    const val TIME_PAYLOAD_LENGTH: Int = 12

    /** §3.2. Exactly this, never more, never less. */
    const val STATUS_PAYLOAD_LENGTH: Int = 12

    /** §3.3. Exactly this, never more, never less. */
    const val FIND_PAYLOAD_LENGTH: Int = 4

    /** §3.2 `battery_percent` sentinel: no sample has ever been taken. */
    const val BATTERY_PERCENT_UNKNOWN: Int = 0xFF

    /**
     * §3.2 `flags`, bit 0: the watch is running a find-phone session and asks the
     * phone to make itself felt — vibration and the alarm screen — for as long as
     * this link lasts (§4.1).
     */
    const val STATUS_FLAG_FIND_PHONE: Int = 0x01

    /**
     * §3.2 `flags`, bit 1: the wearer has asked for the alarm tone as well.
     * Meaningful only alongside [STATUS_FLAG_FIND_PHONE]. The two defined bits;
     * bits 2–7 are reserved and the decoder ignores them.
     */
    const val STATUS_FLAG_FIND_SOUND: Int = 0x02

    /** §3.3 FindMode `mode`, bit 0: tone and vibration when set, vibration only when clear. */
    const val FIND_MODE_SOUND: Int = 0x01

    // ── §5.2 Timing — the load-bearing numbers ───────────────────────────────
    //
    // Here rather than beside the radio code for the same reason as the UUIDs:
    // they are half of a contract, and a number that drifts on one side is
    // invisible from the other. The 15 s exchange timeout is deliberately a
    // backstop *behind* the watch's own 12 s session cap (§5.1), so the watch's
    // teardown is the normal ending and ours is the safety net; changing one of
    // these without changing the document changes which side ends the session.

    /** §5.2. Discovery, CCCD write, Time write — each individually. */
    const val OPERATION_TIMEOUT_MS: Long = 5_000

    /** §5.2. Measured from connect. Longer than the watch's 12 s session cap. */
    const val EXCHANGE_TIMEOUT_MS: Long = 15_000

    /**
     * §5.1's **absolute session cap** - the watch tears its radio down this long
     * after opening a window, whatever is happening.
     *
     * A watch-side number, mirrored here for the same reason as
     * [SYNC_WINDOW_INTERVAL_SECONDS] and under the same §8 rule: the phone needs it,
     * and this is the one file allowed to hold it.
     *
     * What needs it is [com.workaday.core.ConnectionState.Settling]. After a
     * successful exchange the phone must not re-arm into the window it has just
     * used - `autoConnect` reconnects immediately, because the watch is still
     * connectable - and the delay that makes that safe is only correct while
     * [EXCHANGE_TIMEOUT_MS] exceeds this. §5.2 already says it does, in prose;
     * `WatchProtocolTest` turns that sentence into an assertion, because if the two
     * numbers ever cross the result is a hot loop against the watch's radio and
     * nothing else in the app would notice.
     */
    const val WATCH_SESSION_CAP_MS: Long = 12_000

    /** §5.2. */
    const val BACKOFF_BASE_MS: Long = 30_000

    /** §5.2. 15 min — comfortably under the watch's hourly window, so the app is
     *  always armed again before the next one opens. */
    const val BACKOFF_CAP_MS: Long = 15L * 60L * 1_000L

    /** §5.2. ±20 %. */
    const val BACKOFF_JITTER_FRACTION: Double = 0.20

    /**
     * §5.1's sync window interval — the **watch's** cadence, not the phone's, and
     * the only number from that table the phone has any use for.
     *
     * The phone cannot observe the watch's schedule and must never try to predict
     * it: §5.1 says a window that opens resets the hourly timer *when it opens*,
     * and the phone only ever waits on a pending `autoConnect`. Nothing here
     * schedules against this value.
     *
     * What it is for is the diagnostic screen, which has to turn "last synced
     * 4 hours ago" into "that looks wrong" — and the only honest scale for
     * "wrong" is the interval at which the watch actually offers a chance
     * ([syncRecencyFor]). Mirrored here rather than written down beside the UI
     * because §8 allows exactly one file in this app to hold a §5 number, and
     * a hard-coded hour next to a text view is precisely how the two sides drift.
     *
     * In seconds, unlike the §5.2 constants above, because the wall-clock values
     * it is compared against — `HealthSnapshot.lastSuccessUtcEpochSeconds`, §3.2's
     * `applied_utc_epoch_s` — are all epoch **seconds**.
     */
    const val SYNC_WINDOW_INTERVAL_SECONDS: Long = 60L * 60L

    /**
     * §5.1's **find session cap** — how long the watch searches before it gives up
     * on its own. A watch-side number, mirrored under the same §8 rule as
     * [WATCH_SESSION_CAP_MS]: the phone's [FIND_RING_BACKSTOP_MS] is chosen against
     * it, and `WatchProtocolTest` pins the relationship.
     */
    const val WATCH_FIND_PHONE_TIMEOUT_MS: Long = 120_000

    /**
     * §5.2's **ring backstop**: how long the phone will sound a find-phone alarm
     * with no word from the watch. Longer than [WATCH_FIND_PHONE_TIMEOUT_MS] so the
     * watch's hang-up is the normal ending and this is the net behind a disconnect
     * that never arrived. When it fires the phone closes and re-arms.
     */
    const val FIND_RING_BACKSTOP_MS: Long = 135_000

    // ── Field offsets (§3.1, §3.2, §3.3) ─────────────────────────────────────
    //
    // Private: §8 says no field offset appears anywhere else in the app, and the
    // cheapest way to enforce that is to make it impossible to reference one.

    private const val OFFSET_PROTO_VERSION = 0
    private const val OFFSET_MSG_TYPE = 1

    private const val TIME_OFFSET_EPOCH = 2
    private const val TIME_OFFSET_UTC_OFFSET_MIN = 6

    private const val STATUS_OFFSET_RESULT = 2
    private const val STATUS_OFFSET_BATTERY = 3
    private const val STATUS_OFFSET_APPLIED_EPOCH = 4
    private const val STATUS_OFFSET_FW_BUILD = 8
    private const val STATUS_OFFSET_FLAGS = 10

    private const val FIND_OFFSET_MODE = 2

    /** Largest value a `u32` field can carry. */
    private const val MAX_U32: Long = 0xFFFF_FFFFL

    /**
     * Whether these values fit the §3.1 layout at all: `utc_epoch_s` is a `u32`
     * and `utc_offset_min` is an `i16`.
     *
     * This is deliberately **only** a representability check, not a copy of the
     * watch's §3.1 validation table. The watch's range rules (year 2020–2099,
     * offset ±840) are the watch's to enforce, and it answers a violation with
     * `OutOfRange` — which is defined, expected behaviour that the app reports.
     * Duplicating those rules here would be inventing a phone-side rule the
     * contract does not state, and the two copies would drift.
     *
     * What it does catch is a phone whose own clock is unusable — a system time
     * before 1970, or past 2106 — which cannot be put on the wire at all without
     * silently truncating. The state machine treats that as a failed exchange
     * rather than sending a corrupt packet.
     */
    fun isEncodableTime(utcEpochSeconds: Long, utcOffsetMinutes: Int): Boolean =
        utcEpochSeconds in 0L..MAX_U32 &&
            utcOffsetMinutes in Short.MIN_VALUE.toInt()..Short.MAX_VALUE.toInt()

    /**
     * Encode a §3.1 Time payload: exactly [TIME_PAYLOAD_LENGTH] bytes, always a
     * fresh array so no caller can mutate a shared buffer.
     *
     * Throws rather than truncating when the values do not fit the layout —
     * a half-correct packet is worse than none, and the state machine gates every
     * call with [isEncodableTime] so this is unreachable in production.
     */
    fun encodeTime(utcEpochSeconds: Long, utcOffsetMinutes: Int): ByteArray {
        require(isEncodableTime(utcEpochSeconds, utcOffsetMinutes)) {
            "not representable in the PROTOCOL.md §3.1 layout: " +
                "epoch=$utcEpochSeconds offset=$utcOffsetMinutes"
        }
        val out = ByteArray(TIME_PAYLOAD_LENGTH)
        out[OFFSET_PROTO_VERSION] = PROTO_VERSION.toByte()
        out[OFFSET_MSG_TYPE] = MSG_TYPE_SET_TIME.toByte()
        out.putU32Le(TIME_OFFSET_EPOCH, utcEpochSeconds)
        out.putI16Le(TIME_OFFSET_UTC_OFFSET_MIN, utcOffsetMinutes)
        // `flags` (offset 8) and `reserved` (9..11) stay zero: §3.1 says the
        // sender writes 0 and the receiver ignores them. That is what lets a
        // later v1.x add a field without a version bump.
        return out
    }

    /**
     * The CCCD value that enables notifications on Status — `0x0001`,
     * little-endian, per §4's "write CCCD(Status) = 0x0001".
     *
     * A fresh array each call for the same reason [encodeTime] returns one:
     * a shared `ByteArray` constant is mutable and one careless caller poisons
     * every later write.
     */
    fun cccdEnableNotificationValue(): ByteArray = byteArrayOf(0x01, 0x00)

    /**
     * Encode the §3.3 FindDismiss frame: exactly [FIND_PAYLOAD_LENGTH] bytes, the
     * reserved two written as zero, a fresh array each call for the reason
     * [encodeTime] returns one.
     *
     * There is nothing to parameterise. The frame has one meaning — "the phone
     * has been found, from the phone's side" — and the watch decides what to do
     * about it.
     */
    fun encodeFindDismiss(): ByteArray {
        val out = ByteArray(FIND_PAYLOAD_LENGTH)
        out[OFFSET_PROTO_VERSION] = PROTO_VERSION.toByte()
        out[OFFSET_MSG_TYPE] = MSG_TYPE_FIND_DISMISS.toByte()
        return out
    }

    /**
     * Decode a §3.3 FindMode notify — how the watch wants the phone to make itself
     * felt right now.
     *
     * Order of checks mirrors the firmware's Find decoder — length, version, type.
     * **Null, not an exception, for anything that is not a FindMode frame**: §3.3
     * says such a frame is ignored and the phone keeps doing what it was doing,
     * and the same callback delivers Status frames, so a 12-byte Status arriving
     * on a find link must come back as "not a mode" rather than as a crash. Only
     * bit 0 of `mode` is read; the reserved bits cannot change the answer.
     */
    fun decodeFindMode(payload: ByteArray?): FindMode? {
        if (payload == null || payload.size != FIND_PAYLOAD_LENGTH) return null
        if ((payload[OFFSET_PROTO_VERSION].toInt() and 0xFF) != PROTO_VERSION) return null
        if ((payload[OFFSET_MSG_TYPE].toInt() and 0xFF) != MSG_TYPE_FIND_MODE) return null
        return FindMode(sound = (payload[FIND_OFFSET_MODE].toInt() and FIND_MODE_SOUND) != 0)
    }

    /**
     * Decode a §3.2 Status payload.
     *
     * Order of checks mirrors the firmware's decoder — length, version, type —
     * so a frame with more than one fault is described the same way on both
     * sides. The payload is nullable because Android hands the value out of a
     * platform-typed getter that can be null; a null payload is a wrong length,
     * not a crash.
     *
     * An unknown `result` code does **not** reject the frame: the rest of it is
     * still useful to the diagnostic screen, and [WatchStatus.isSuccess] is false
     * for anything that is not exactly `Ok`, so a code from the future can never
     * be mistaken for a successful exchange.
     */
    fun decodeStatus(payload: ByteArray?): StatusDecode {
        if (payload == null || payload.size != STATUS_PAYLOAD_LENGTH) {
            return StatusDecode.Rejected(StatusRejection.WrongLength, payload?.size ?: -1)
        }
        val version = payload[OFFSET_PROTO_VERSION].toInt() and 0xFF
        if (version != PROTO_VERSION) {
            return StatusDecode.Rejected(StatusRejection.UnsupportedVersion, version)
        }
        val msgType = payload[OFFSET_MSG_TYPE].toInt() and 0xFF
        if (msgType != MSG_TYPE_SYNC_RESULT) {
            return StatusDecode.Rejected(StatusRejection.UnexpectedMessageType, msgType)
        }

        val resultCode = payload[STATUS_OFFSET_RESULT].toInt() and 0xFF
        // Kotlin's Byte is signed, so 0xFF arrives as -1: mask before comparing.
        val rawBattery = payload[STATUS_OFFSET_BATTERY].toInt() and 0xFF
        return StatusDecode.Valid(
            WatchStatus(
                resultCode = resultCode,
                result = SyncResult.fromCode(resultCode),
                // §3.2 promises the sender normalises anything outside 0–100 to
                // 0xFF, so 0–100 and "unknown" are the complete domain. We hold
                // it to that promise rather than trusting it: a watch that
                // shipped a garbled 200 % is reported as unknown, never as a
                // fourth kind of value the UI has to guess at.
                batteryPercent = rawBattery.takeIf { it in 0..100 },
                appliedUtcEpochSeconds = payload.u32Le(STATUS_OFFSET_APPLIED_EPOCH),
                fwBuild = payload.u16Le(STATUS_OFFSET_FW_BUILD),
                // §3.2 flags, bit 0 and only bit 0: the other seven are reserved
                // and a later v1.x may give one meaning, so this build must not
                // react to them. The masked test is what makes that true.
                findPhoneRequested =
                    (payload[STATUS_OFFSET_FLAGS].toInt() and STATUS_FLAG_FIND_PHONE) != 0,
                findSoundRequested =
                    (payload[STATUS_OFFSET_FLAGS].toInt() and STATUS_FLAG_FIND_SOUND) != 0,
            ),
        )
        // `reserved` (offset 11) is deliberately not read — §3 says the receiver
        // ignores it, which is what let offset 10 become `flags` without a bump.
    }
}

/**
 * §3.2 `result` codes. These values go on the wire, so they are the contract
 * rather than an internal ordering: written out explicitly, never renumbered,
 * never reordered.
 */
enum class SyncResult(val code: Int) {
    /** Validated and written to the PCF8563. The only success. */
    Ok(0),

    /** Payload was not 12 bytes. */
    BadLength(1),

    /**
     * `proto_version` mismatch. §6.2: this one means the two sides have drifted
     * and retrying will not help — surface it, do not hot-loop.
     */
    BadVersion(2),

    /** Epoch or offset outside the accepted range. */
    OutOfRange(3),

    /** Validated, but the I2C write to the PCF8563 did not take. */
    RtcWriteFailed(4),

    /** Unknown `msg_type`. */
    BadType(5),

    /** The watch is closing the window; try the next one. */
    Busy(6),
    ;

    /**
     * Whether waiting for the watch's next window could plausibly change this
     * answer.
     *
     * §6.2, verbatim: "`BadVersion` in particular means the two sides have drifted
     * and retrying will not help — show it, do not hot-loop." Every other code
     * describes this attempt — a truncated frame, a busy window, an I2C write that
     * did not take — and the next window is a fresh one.
     *
     * Nothing branches on this to *decide* whether to retry: the app's answer to a
     * failed exchange is always the same bounded, jittered backoff, and adding a
     * "give up" path would be the terminal error state Law 2 forbids. What it
     * changes is what the diagnostic screen tells the wearer, which for
     * `BadVersion` is "update one side", not "it will sort itself out".
     */
    val retryingWillHelp: Boolean get() = this != BadVersion

    companion object {
        /** Null when the watch sent a code this build does not know about. */
        fun fromCode(code: Int): SyncResult? = entries.firstOrNull { it.code == code }
    }
}

/**
 * A decoded §3.2 Status frame.
 *
 * [resultCode] is kept raw alongside [result] so the diagnostic screen can show
 * a code from a newer firmware instead of "unknown".
 */
class WatchStatus(
    /** The `result` byte exactly as received. */
    val resultCode: Int,
    /** [resultCode] mapped to a known code, or null if this build does not know it. */
    val result: SyncResult?,
    /** 0–100, or null when the watch reported `0xFF` (no sample ever taken). */
    val batteryPercent: Int?,
    /** What the watch actually committed; 0 if nothing was. */
    val appliedUtcEpochSeconds: Long,
    /** Firmware build tag. Diagnostic only — nothing branches on it. */
    val fwBuild: Int,
    /**
     * §3.2 `flags.FIND_PHONE`: the watch is running a find-phone session and asks
     * the phone to make itself heard for as long as this link lasts (§4.1).
     * Independent of [result] on purpose — a clock that could not be set is no
     * reason to leave the phone lost — and the state machine reads the two apart.
     */
    val findPhoneRequested: Boolean = false,
    /**
     * §3.2 `flags.FIND_SOUND`: the wearer wants the alarm tone as well as the
     * vibration (§4.1). Read only when [findPhoneRequested] is set; on its own it
     * means nothing and the state machine never looks at it alone.
     */
    val findSoundRequested: Boolean = false,
) {
    /**
     * §4: a notify with `result == 0` is *the* definition of a successful
     * exchange. Anything else — including a code this build does not recognise —
     * is a failure, and in particular must not reset backoff or health.
     */
    val isSuccess: Boolean get() = result == SyncResult.Ok

    override fun toString(): String =
        "WatchStatus(result=${result ?: "unknown"}($resultCode), " +
            "battery=${batteryPercent ?: "unknown"}, " +
            "applied=$appliedUtcEpochSeconds, fwBuild=$fwBuild" +
            (if (findPhoneRequested) ", findPhone" else "") +
            (if (findSoundRequested) "+sound" else "") + ")"
}

/**
 * A decoded §3.3 FindMode notify: how the watch wants the phone heard right now.
 * [sound] true is tone and vibration, false is vibration only.
 */
data class FindMode(val sound: Boolean)

/** Why a Status frame was thrown away without being interpreted. */
enum class StatusRejection {
    /** Not exactly [WatchProtocol.STATUS_PAYLOAD_LENGTH] bytes, or null. */
    WrongLength,

    /** `proto_version` was not [WatchProtocol.PROTO_VERSION]. */
    UnsupportedVersion,

    /** `msg_type` was not [WatchProtocol.MSG_TYPE_SYNC_RESULT]. */
    UnexpectedMessageType,
}

/** The outcome of [WatchProtocol.decodeStatus]. */
sealed interface StatusDecode {

    /** A well-formed §3.2 frame. Says nothing about whether the sync succeeded. */
    data class Valid(val status: WatchStatus) : StatusDecode

    /**
     * The bytes were not a Status frame at all.
     *
     * [observed] is the offending value, kept for the diagnostic screen: the
     * payload length for [StatusRejection.WrongLength] (−1 when the payload was
     * null), the version byte for [StatusRejection.UnsupportedVersion], the
     * message-type byte for [StatusRejection.UnexpectedMessageType].
     */
    data class Rejected(val reason: StatusRejection, val observed: Int) : StatusDecode
}

private fun ByteArray.putU32Le(offset: Int, value: Long) {
    this[offset] = (value and 0xFF).toByte()
    this[offset + 1] = ((value shr 8) and 0xFF).toByte()
    this[offset + 2] = ((value shr 16) and 0xFF).toByte()
    this[offset + 3] = ((value shr 24) and 0xFF).toByte()
}

private fun ByteArray.putI16Le(offset: Int, value: Int) {
    this[offset] = (value and 0xFF).toByte()
    this[offset + 1] = ((value shr 8) and 0xFF).toByte()
}

private fun ByteArray.u32Le(offset: Int): Long =
    (this[offset].toLong() and 0xFF) or
        ((this[offset + 1].toLong() and 0xFF) shl 8) or
        ((this[offset + 2].toLong() and 0xFF) shl 16) or
        ((this[offset + 3].toLong() and 0xFF) shl 24)

private fun ByteArray.u16Le(offset: Int): Int =
    (this[offset].toInt() and 0xFF) or ((this[offset + 1].toInt() and 0xFF) shl 8)
