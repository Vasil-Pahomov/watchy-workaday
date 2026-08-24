#include "core/protocol.h"

namespace core {
namespace {

// ── PROTOCOL.md §3.1, the Time layout ────────────────────────────────────────
// These offsets stay in this translation unit: nothing outside protocol.cpp is
// allowed to know where a field sits (§8), and a test that used them would be
// checking the decoder against itself instead of against the document.
constexpr size_t kTimeOffsetProtoVersion = 0;
constexpr size_t kTimeOffsetMsgType = 1;
constexpr size_t kTimeOffsetUtcEpoch = 2;
constexpr size_t kTimeOffsetUtcOffset = 6;
// Offset 8 (`flags`) and offsets 9..11 (`reserved`) are never read, on purpose.
// §3.1 makes ignoring them the mechanism that lets a v1.x sender add a field
// without a version bump; validating them would take that away.

// ── PROTOCOL.md §3.2, the Status layout ──────────────────────────────────────
constexpr size_t kStatusOffsetProtoVersion = 0;
constexpr size_t kStatusOffsetMsgType = 1;
constexpr size_t kStatusOffsetResult = 2;
constexpr size_t kStatusOffsetBattery = 3;
constexpr size_t kStatusOffsetAppliedEpoch = 4;
constexpr size_t kStatusOffsetFwBuild = 8;
constexpr size_t kStatusOffsetReserved = 10;

uint32_t readU32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int16_t readI16Le(const uint8_t* p) {
  const uint16_t raw =
      static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8));
  // Two's complement by hand. Casting an out-of-range uint16_t straight to int16_t
  // is implementation-defined before C++20, and this is the value that decides
  // whether a negative offset means "west of Greenwich" or "+18 hours".
  return raw <= 0x7FFF ? static_cast<int16_t>(raw)
                       : static_cast<int16_t>(static_cast<int32_t>(raw) - 0x10000);
}

void writeU16Le(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFu);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void writeU32Le(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFu);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

}  // namespace

TimeWrite decodeTimeWrite(const uint8_t* payload, size_t length) {
  TimeWrite decoded{};

  // The order below is §3.1's validation table, top to bottom. It is load-bearing
  // rather than stylistic: a payload with two faults must produce the same code on
  // both sides of the contract, or the app's diagnostic screen blames the wrong
  // thing.

  // A null pointer is a wrong length. Checked first so nothing below dereferences
  // it, and so a truncated or oversized write is refused before it is parsed.
  if (payload == nullptr || length != kTimePayloadLength) {
    decoded.result = SyncResult::BadLength;
    return decoded;
  }
  if (payload[kTimeOffsetProtoVersion] != kProtocolVersion) {
    decoded.result = SyncResult::BadVersion;
    return decoded;
  }
  if (payload[kTimeOffsetMsgType] != kMsgTypeSetTime) {
    decoded.result = SyncResult::BadType;
    return decoded;
  }

  const uint32_t epoch = readU32Le(payload + kTimeOffsetUtcEpoch);
  const int16_t offset = readI16Le(payload + kTimeOffsetUtcOffset);

  if (offset < kMinUtcOffsetMinutes || offset > kMaxUtcOffsetMinutes) {
    decoded.result = SyncResult::OutOfRange;
    return decoded;
  }

  // Validity is judged on the **local** time, not the UTC instant, because local
  // is what reaches the PCF8563. An offset can carry an otherwise fine timestamp
  // across either end of the accepted window, and that has to be caught here —
  // an alarm computed from an impossible date never fires, which is the frozen
  // watch Law 2 exists to prevent.
  //
  // This check is also why the u32 epoch's 2106 overflow is unreachable: kMaxYear
  // is 2099, so the range bites long before the wrap. Stated so that nobody
  // "fixes" the field into a u64 and breaks the 12-byte layout.
  const DateTime local = localFromUnixEpoch(epoch, offset);
  if (!isValid(local)) {
    decoded.result = SyncResult::OutOfRange;
    return decoded;
  }

  decoded.result = SyncResult::Ok;
  decoded.local = local;
  decoded.utc_epoch_s = epoch;
  decoded.utc_offset_min = offset;
  return decoded;
}

bool encodeStatus(uint8_t* out, size_t cap, const Status& status) {
  if (out == nullptr || cap < kStatusPayloadLength) {
    return false;
  }

  // §3.2 defines this field's domain as 0..100 or kBatteryPercentUnknown. A
  // caller that has no sample yet, or whose ADC read came back garbled, must not
  // put a fourth kind of value on the wire for the app to guess at; "unknown" is
  // the documented answer for "not a percentage".
  const uint8_t battery =
      (status.battery_percent <= 100) ? status.battery_percent : kBatteryPercentUnknown;

  out[kStatusOffsetProtoVersion] = kProtocolVersion;
  out[kStatusOffsetMsgType] = kMsgTypeSyncResult;
  out[kStatusOffsetResult] = static_cast<uint8_t>(status.result);
  out[kStatusOffsetBattery] = battery;
  writeU32Le(out + kStatusOffsetAppliedEpoch, status.applied_utc_epoch_s);
  writeU16Le(out + kStatusOffsetFwBuild, status.fw_build);
  // §3.2: the sender writes the reserved bytes as zero. Leaving whatever the
  // caller's buffer happened to hold would put stack contents on the air and
  // would make a future receiver that gives those bytes meaning behave randomly
  // against this build.
  out[kStatusOffsetReserved] = 0;
  out[kStatusOffsetReserved + 1] = 0;
  return true;
}

}  // namespace core
