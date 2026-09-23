#include "core/sync_policy.h"

namespace core {
namespace {

// PROTOCOL.md §5.1's third gate: the hour has turned since the last window opened.
//
// The boundary needs two things to be true at once — a clock worth reading now,
// and an hour recorded from a clock that was worth reading then. Either missing
// and there is no boundary to compare against, so the elapsed counter decides:
// "the last sync is under 60 minutes old" closes the gate, exactly 60 opens it,
// the same >= convention as the battery sample schedule.
//
// Note which way round the fallback is written. It is not "if the clock is bad,
// refuse" — that is the failure §5.1 spends three paragraphs on, a watch with a
// dead clock locked out of the one window that could fix it.
bool windowDue(const SyncContext& context) {
  if (context.clock_valid && context.last_window.known) {
    return context.hour != context.last_window.hour;
  }
  return context.minutes_since_window >= kSyncWindowIntervalMinutes;
}

// §5.1: no radio in a degraded mode. Safe and Recovery exist to keep a faulting
// watch alive for weeks on what is left of the cell; the largest consumer on the
// board is not something to make an exception for.
bool radioPermitted(RunMode mode) {
  switch (mode) {
    case RunMode::Safe:
    case RunMode::Recovery:
      return false;
    case RunMode::Normal:
      break;
  }
  return true;
}

// A result byte read back out of RTC memory. The block's magic and version have
// already been checked by the time this module sees it, so this only catches
// corruption inside an otherwise valid block — but the value goes straight onto
// the air as §3.2's `result` field, where a code outside the table is something
// the app has no defined reading for.
//
// Busy is the same substitution core::Status makes for a status nobody filled in,
// and it is already in the contract. Nothing new is invented here.
SyncResult knownResultOr(SyncResult result, SyncResult fallback) {
  switch (result) {
    case SyncResult::Ok:
    case SyncResult::BadLength:
    case SyncResult::BadVersion:
    case SyncResult::OutOfRange:
    case SyncResult::RtcWriteFailed:
    case SyncResult::BadType:
    case SyncResult::Busy:
      return result;
  }
  return fallback;
}

}  // namespace

SyncDecision evaluateSyncWindow(const SyncContext& context) {
  SyncDecision decision;

  // Order matters: §5.1 lists the gates mode, battery, interval, and reporting the
  // first one that closed is what lets the UI say something true about why a
  // request was refused. For a user request only the first can close, so the
  // Find phone screen has exactly one refusal left to render — see
  // core::findOutcomeForGate().
  if (!radioPermitted(context.mode)) {
    decision.gate = SyncGate::DegradedMode;
    return decision;
  }

  // core::radioPermitted(BatteryLevel) gates the **schedule**, not the radio, so
  // the user request is checked first here rather than after it. Below 10 % the
  // watch stops reaching for the phone on its own; it does not stop being able to.
  if (!context.user_requested && !radioPermitted(context.battery)) {
    decision.gate = SyncGate::BatteryTooLow;
    return decision;
  }

  // The other gate a user request overrides. "Opens a window immediately" (§5.1)
  // is precisely this line.
  if (!context.user_requested && !windowDue(context)) {
    decision.gate = SyncGate::IntervalNotElapsed;
    return decision;
  }

  decision.open = true;
  decision.gate = SyncGate::Open;
  return decision;
}

void resetSyncState(SyncState& state) { state = SyncState{}; }

void advanceSyncTimer(SyncState& state, uint32_t elapsed_minutes) {
  // Compared against the headroom rather than summed and then clamped. The obvious
  // form — add into a uint32 and clamp the result — wraps for exactly the input
  // this is here to survive: 65535 + 0xFFFFFFFF is 65534 in 32-bit arithmetic, so
  // a corrupt elapsed would leave the counter reading a minute short of the
  // maximum instead of pinned at it. Subtracting first cannot overflow, because
  // both operands are uint16.
  const uint32_t headroom = static_cast<uint32_t>(UINT16_MAX - state.minutes_since_window);
  if (elapsed_minutes >= headroom) {
    state.minutes_since_window = UINT16_MAX;
    return;
  }
  state.minutes_since_window =
      static_cast<uint16_t>(state.minutes_since_window + elapsed_minutes);
}

void noteSyncWindowOpened(SyncState& state, uint32_t hour, bool clock_valid) {
  // The whole module in two lines. They run when the window is *granted*, so a
  // window that reaches nobody — or one that dies to a watchdog reset halfway
  // through — still costs the full hour before the next attempt.
  state.minutes_since_window = 0;
  if (clock_valid) {
    state.last_window.hour = hour;
    state.last_window.known = true;
  }
  // An untrustworthy clock leaves the recorded hour alone rather than clearing it.
  // Clearing would drop the watch onto the counter for an hour on every bad read;
  // keeping a stale hour is harmless, because the gate ignores it until the clock
  // is readable again and by then the hour has genuinely turned.
}

void noteSyncResult(SyncState& state, SyncResult result, uint32_t applied_utc_epoch_s) {
  state.last_result = knownResultOr(result, SyncResult::Busy);
  if (result == SyncResult::Ok) {
    state.last_applied_epoch_s = applied_utc_epoch_s;
  }
  // Anything else committed nothing, so last_applied_epoch_s keeps the last value
  // that really did reach the PCF8563 (§3.2).
}

Status statusFromSyncState(const SyncState& state, uint8_t battery_percent, bool battery_sampled,
                           uint16_t fw_build) {
  Status status;
  status.result = knownResultOr(state.last_result, SyncResult::Busy);
  // §3.2: "Report 0xFF rather than a stale guess if no sample has ever been
  // taken." A watch that has not sampled holds a plain 0, which is a perfectly
  // valid percentage and would be read as an empty battery.
  status.battery_percent = battery_sampled ? battery_percent : kBatteryPercentUnknown;
  status.applied_utc_epoch_s = state.last_applied_epoch_s;
  status.fw_build = fw_build;
  return status;
}

}  // namespace core
