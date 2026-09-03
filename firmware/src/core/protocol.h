// The watch's half of the watch <-> phone contract. Pure logic — no radio, no
// hardware headers, host-tested.
//
// **This file is a mirror, not a design.** `PROTOCOL.md` at the top of the
// Workaday tree is the single source of truth, and the Android app mirrors the
// same document in `WatchProtocol.kt`. Nothing here may be invented, extended or
// tidied up locally: the two codebases never see each other, so a silent
// deviation on one side shows up in the field as a watch that quietly rejects
// every sync, or worse, accepts one and sets the clock wrong.
//
// PROTOCOL.md §8 makes that concrete. This is the **only** file in the firmware
// permitted to contain a UUID literal, a wire-format field offset, or one of the
// §5.1 timeouts. A UUID appearing in board/ or app/ is the defect, not a
// convenience.
//
// Why the parsing lives in core/ rather than beside the GATT callback: deciding
// that twelve bytes are trustworthy is a *decision*, and decisions are the things
// that get a unit test. `test/test_protocol/` replays the golden vectors of
// PROTOCOL.md §7 byte for byte — that test, not the prose, is what actually keeps
// the two implementations from drifting apart.
//
// Nothing in here allocates, and nothing takes a length it was not given.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/time_model.h"

namespace core {

// ── §2.1 Identity — fixed forever ────────────────────────────────────────────
//
// `0x57444159` is 'WDAY' and `…01` / `…02` continue the family. That is a
// mnemonic, not a mechanism: new UUIDs are not derived by incrementing, they are
// added to the table in PROTOCOL.md first.
constexpr const char* kSyncServiceUuid = "57444159-6461-4779-b0a3-1f4c7e25d908";
constexpr const char* kTimeCharacteristicUuid = "57444101-6461-4779-b0a3-1f4c7e25d908";
constexpr const char* kStatusCharacteristicUuid = "57444102-6461-4779-b0a3-1f4c7e25d908";
// §3.3, the phone's answer to a find-phone search (§4.1): write-only, one frame,
// "the phone has been found from the phone's side". Added 3 Sep 2026 without a
// PROTO_VERSION bump — a phone that does not know it never touches it.
constexpr const char* kFindCharacteristicUuid = "57444103-6461-4779-b0a3-1f4c7e25d908";
// The SIG-standard Client Characteristic Configuration descriptor on Status. It
// is in §2.1's table and therefore belongs here rather than being spelled out at
// whatever line of board/ first needs it.
constexpr const char* kCccdUuid = "00002902-0000-1000-8000-00805f9b34fb";

// ── §5.1 Timing — the load-bearing numbers ───────────────────────────────────
//
// Here rather than beside the radio code for the same reason as the UUIDs: they
// are half of a contract, and a number that drifts on one side is invisible from
// the other. The phone's matching numbers (§5.2) are chosen against these — its
// 15 s exchange timeout is a backstop *behind* the watch's 12 s cap, so changing
// one of these without the document changes which side ends the session.
//
// The binding constraint is the 10 s task watchdog, which Law 2 forbids feeding
// from inside a wait loop — and a BLE window is precisely a wait loop. The
// resolution: **no un-fed interval may exceed 6 s of window time, plus a
// teardown tail. Worst case ~6.05 s, margin ~3.95 s.** The tail used to be bounded
// at ~2.05 s by NimBLE's CONFIG_BT_NIMBLE_HS_STOP_TIMEOUT_MS, which is where
// PROTOCOL.md §5.1's ~8.05 s race row comes from; `board::ble`'s destructor no
// longer calls NimBLEDevice::deinit() (it panics core 0 — see ble.cpp), so that
// wait no longer happens and the margin is the larger figure. The document is
// conservative rather than wrong, and is the one to change first if it is
// re-derived. The watchdog is fed at three genuine
// progress points only — the stack came up and the controller synced, a central
// connected, and a Time write was processed. Advertising with nobody there is 6 s
// and then over; connected but idle is 4 s and then over.
//
// The worst case is a race: advertising expires and a central connects before
// teardown begins, so 6 s of window is followed by a teardown that must now
// terminate a live link. §5.1 carries the full table. Raising
// kAdvertiseTimeoutMs without re-deriving it walks that race into a reset.
constexpr uint16_t kSyncWindowIntervalMinutes = 60;
constexpr uint16_t kAdvertiseTimeoutMs = 6000;
constexpr uint16_t kIdleAfterConnectTimeoutMs = 4000;
constexpr uint16_t kSessionCapMs = 12000;

// §5.1's find session cap (§4.1). Two minutes of advertising, or of a live link
// with the phone ringing, and then the search ends whatever the radio is doing.
//
// This is NOT one un-fed interval and must never be waited on in one go: the
// watchdog is still 10 s. core::FindSession hands out waits of at most
// kAdvertiseTimeoutMs, each followed by a completed step the caller feeds after
// — a redraw, a connect, a processed write. The phone mirrors this number as the
// floor of its own ring backstop (§5.2), which is why it lives here and not in
// find_session.h.
constexpr uint32_t kFindPhoneTimeoutMs = 120000;

// ── §3 Wire format ───────────────────────────────────────────────────────────
//
// Little-endian throughout. Both payloads are a fixed 12 bytes, which is what
// lets them ride the default 23-byte ATT MTU with no MTU exchange — one fewer
// GATT operation, one fewer timeout, one fewer failure mode, for no gain. A wrong
// length is rejected without being parsed: the decoder never reads past what it
// was handed and never guesses at a truncated packet.
constexpr uint8_t kProtocolVersion = 0x01;

constexpr uint8_t kMsgTypeSetTime = 0x01;      // phone -> watch, on Time
constexpr uint8_t kMsgTypeFindDismiss = 0x02;  // phone -> watch, on Find (§3.3)
constexpr uint8_t kMsgTypeSyncResult = 0x81;   // watch -> phone

constexpr size_t kTimePayloadLength = 12;
constexpr size_t kStatusPayloadLength = 12;
constexpr size_t kFindPayloadLength = 4;

// §3.2 `flags`, bit 0: the watch is running a find-phone session and asks the
// phone to make itself heard for as long as this link lasts (§4.1). The only
// defined bit; encodeStatus() puts no other bit on the wire.
constexpr uint8_t kStatusFlagFindPhone = 0x01;

// §3.1. UTC+14 and UTC-14 are the real-world extremes; 840 == 14 * 60.
constexpr int16_t kMinUtcOffsetMinutes = -840;
constexpr int16_t kMaxUtcOffsetMinutes = 840;

// §3.2. Reported instead of a stale guess when no battery sample has been taken.
constexpr uint8_t kBatteryPercentUnknown = 0xFF;

// §3.2. These values go on the wire, so they are the contract rather than an
// internal ordering: written out explicitly, never renumbered, never reordered.
enum class SyncResult : uint8_t {
  Ok = 0,              // validated and written to the PCF8563
  BadLength = 1,       // payload was not 12 bytes
  BadVersion = 2,      // proto_version mismatch
  OutOfRange = 3,      // epoch or offset outside the accepted range
  RtcWriteFailed = 4,  // validated, but the I2C write to the PCF8563 did not take
  BadType = 5,         // unknown msg_type
  Busy = 6,            // the watch is closing the window; try the next one
};

// The outcome of decoding one Time write (§3.1).
//
// `result` defaults to a rejection rather than Ok, so a struct that never reached
// the decoder — an early return, a caller that forgot — cannot be mistaken for a
// validated time and pushed into the RTC.
struct TimeWrite {
  SyncResult result = SyncResult::BadLength;

  // Meaningful only when `result == Ok`. Already local: the offset has been
  // applied and the calendar date validated, so the caller hands this straight to
  // board::rtc::write() with no further arithmetic.
  DateTime local{};

  // The epoch as received, kept so that Status::applied_utc_epoch_s can echo the
  // exact value the phone sent instead of re-deriving it. Left at 0 on every
  // rejection path, which is also what §3.2 wants reported when nothing was
  // committed.
  uint32_t utc_epoch_s = 0;
  int16_t utc_offset_min = 0;
};

// Decode and validate a Time write (§3.1).
//
// The checks run in the order §3.1's validation table lists them — length,
// version, type, offset range, then local-calendar validity — so a payload with
// more than one fault reports the same code here as it does in the app. A null
// pointer is a wrong length.
//
// Note what this deliberately does not do: it never touches the RTC. Judging the
// bytes is a decision and lives here; writing them is an effect and lives in
// board/. `RtcWriteFailed` is therefore never returned from this function — the
// caller raises it after board::rtc::write() reports a failure.
TimeWrite decodeTimeWrite(const uint8_t* payload, size_t length);

// §3.2. What the watch reports back: on notify after a write, and on a plain read,
// where it describes the *previous* sync from RTC-backed state so the app's
// diagnostic screen works without having to write first.
struct Status {
  // Defaults to a failure, for the same reason TimeWrite::result does and with
  // more at stake: this is the struct that actually goes on the air.
  //
  // §4 makes a notify with `result == 0` *the* definition of a successful
  // exchange — it is what resets the phone's backoff and its health counters, and
  // a successful connect is explicitly not enough. So a path that fills in
  // battery_percent and fw_build but forgets this field would report that the
  // clock had been set when nothing was written, and the phone would stop
  // retrying. That is the silent stall both projects exist to refuse.
  //
  // Defaulting to Ok is also self-contradictory: paired with the default
  // applied_utc_epoch_s of 0 it claims both "validated and written to the
  // PCF8563" and "nothing was committed", a combination §3.2 never permits.
  //
  // Busy is the honest code for a status nobody filled in — already in the
  // contract, meaning "the watch is closing the window; try the next one", which
  // is exactly what an unfinished window is. The phone counts it as a failed
  // exchange and comes back on the next one.
  SyncResult result = SyncResult::Busy;

  // 0..100, or kBatteryPercentUnknown. Comes from a sample the watch already
  // holds — reporting it must never cost an extra ADC read.
  uint8_t battery_percent = kBatteryPercentUnknown;

  // What the watch actually committed; 0 if nothing was.
  uint32_t applied_utc_epoch_s = 0;

  uint16_t fw_build = 0;  // diagnostic only

  // §3.2 `flags`. Zero on every Status the sync window sends; kStatusFlagFindPhone
  // only from inside a find session (§4.1). Independent of `result` on purpose: a
  // find session that reached a phone but could not set the clock still wants the
  // phone to ring, and the phone reads the two fields separately.
  uint8_t flags = 0;
};

// Encode a Status payload (§3.2) into `out`, writing exactly kStatusPayloadLength
// bytes including the reserved ones as zero.
//
// Returns false and writes nothing when the buffer is too small — the same
// convention as formatTime(), for the same reason: a half-written packet is worse
// than no packet.
bool encodeStatus(uint8_t* out, size_t cap, const Status& status);

// Decode and validate a Find write (§3.3). The checks run in §3.3's order —
// length, version, type — and the codes are §3.2's, reused as a vocabulary: Ok is
// a valid FindDismiss, anything else is a frame the watch ignores. A null pointer
// is a wrong length. The reserved bytes are never read.
//
// There is deliberately nothing to decode beyond validity: the frame has one
// meaning, and what the watch does about it — end the search, say the phone
// stopped it — is core::FindSession::noteFindWrite()'s decision.
SyncResult decodeFindWrite(const uint8_t* payload, size_t length);

}  // namespace core
