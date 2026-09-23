// Whether a BLE sync window opens on this wake. Pure logic — there is no radio in
// this file and there is not meant to be one. The decision lives here, where
// `pio test -e native` can reach it; the code that actually advertises obeys it.
//
// This is the most expensive decision in the firmware. One window is ~0.031 mAh
// against a 9.5 mAh/day allowance (PROTOCOL.md §5.3), and continuous advertising
// would be ~20 mAh/day — it would flatten the cell in about ten days. So the
// question is never "may we sync now?" but "can the watch afford a window, and is
// one due?".
//
// **The gates are PROTOCOL.md §5.1 and that document is the specification, not
// this comment.** The fallback interval lives in core/protocol.h as
// kSyncWindowIntervalMinutes, next to the rest of §5.1's numbers, because it is
// half of a contract the Android app mirrors — a copy here would be a second
// place for it to drift. Nothing in this module may relax a gate locally.
//
// ── The rule the whole module exists for ─────────────────────────────────────
//
// **The schedule is spent when a window OPENS, not when one succeeds.**
//
// Resetting on success sounds like the helpful thing and is the failure mode Law 2
// exists to prevent: a watch whose phone is out of range, switched off, or simply
// in another room never succeeds, so it would advertise on every single wake —
// 1440 windows a day, ~26 mAh/day against a 9.5 mAh/day allowance, a flat battery
// in about a week, and an unbounded retry with no upper limit anywhere in it. The
// watch that most needs its clock set is precisely the one that would kill itself
// trying. So the timer is spent the moment the window is granted, and a window
// that reached nobody costs exactly one hour of waiting, the same as one that
// worked.
//
// That is why noteSyncWindowOpened() is a separate call the caller makes *before*
// the radio comes up rather than something the radio path does on its way out: an
// exit path can be missed — by an early return, an exception-free error branch, or
// a watchdog reset partway through the window — and every one of those misses is
// the unbounded retry above. The same argument core::accel_policy makes for
// counting a configuration attempt before the upload runs, for the same reason.
//
// ── §5.1's third gate: the hour boundary, and its fallback ───────────────────
//
// §5.1 schedules the window **on the hour** — the wake at which the wall clock's
// hour differs from the hour the last window opened in. Not "60 minutes since the
// last one", which is what this used to be and which free-runs: a watch whose
// first window happened to land at 07:23 met its phone at :23 past every hour for
// the rest of the charge, for no reason anybody chose. On the boundary the cadence
// is the same 24 windows a day, but *when* is a fact about the clock rather than a
// fact about when the watch happened to boot — which is what makes it predictable
// from the outside, on both sides of the link and to the wearer.
//
// Two properties the boundary has to hold, and both come from hoursSinceEpoch()
// being a monotonic count rather than dt.hour:
//
//   * it survives midnight — 23:xx to 00:xx is a different hour, and so is 23:xx
//     to 23:xx the next day, which a bare hour number would call the same one;
//   * it cannot open twice in an hour. The hour is recorded when the window opens,
//     so a user-requested window at 10:59 sets the hour to 10 and the scheduled
//     one at 11:00 still opens — two windows a minute apart, which is the intended
//     reading of "the wearer asked" and is bounded at one extra window per press.
//
// A clock that cannot be read has no hour boundary to sit on, so the gate falls
// back to `minutes_since_window`, a counter the caller advances with
// core::elapsedMinutes() — which uses the clock when it is trustworthy and falls
// back to the tick interval it asked for when it is not:
//
//   * clock readable   — the boundary decides, and the counter rides along unused;
//   * clock unreadable — the counter still advances, at the rate the watch is
//     actually ticking, so the cadence stays hourly instead of the gate
//     evaporating and the watch advertising on every wake. That is the same
//     unbounded retry the paragraph above §5.1's gate list forbids.
//
// The fallback is not a rare path. It is the path a **brand new watch** takes:
// nothing has ever set its clock, so nothing can be aligned to it, and the counter
// is what lets it ask for the time at all.
//
// The counter must equally never *freeze*, which is the opposite failure and the
// more tempting mistake: advance it only when the clock is valid, and a watch
// whose PCF8563 has died can never reach 60 minutes, so it can never open the
// window that would set its clock. The feature would be unavailable exactly when
// it is needed.
//
// core::elapsedMinutes() (core/time_model.h) is what prevents both, and the
// coverage is split to match: its own branches — an unreadable clock, no previous
// reading, a clock running backwards, the 30-day clamp, the floor at 1 — are tested
// in test/test_time_model/, and test/test_sync_policy/ drives it into this module
// to check the composed guarantee, that a dead-clock watch opens 24 windows a day
// rather than 1440 or none.
//
// And it must never wrap. A uint16 counter that rolled over during a long absence
// would come back reading a handful of minutes and lock the watch out of syncing
// for another hour, so it saturates instead — see advanceSyncTimer().
#pragma once

#include <cstdint>

#include "core/battery_model.h"
#include "core/health.h"
#include "core/protocol.h"

namespace core {

// Which gate closed, reported rather than folded into a bare bool so that a
// refusal can be logged and shown ("not now: battery low") instead of the watch
// appearing to ignore the user. Anything other than Open is a refusal.
enum class SyncGate : uint8_t {
  Open,           // §5.1's three gates all passed
  DegradedMode,   // RunMode::Safe or RunMode::Recovery — no radio in degraded modes
  BatteryTooLow,  // BatteryLevel::Low or Critical, and nobody asked for this window
  // The hour has not turned since the last window opened — or, on a watch whose
  // clock cannot be read, fewer than kSyncWindowIntervalMinutes have passed. Kept
  // under its original name because the five enumerators are logged as integers
  // and read against a running watch during bring-up.
  IntervalNotElapsed,
};

// When a window last opened, on the clock. Carried in SyncState because it is what
// the next window is scheduled against, and copied into SyncContext because that
// is where every other input to the decision lives.
//
// `known` is false until a window has opened on a wake whose clock could be
// trusted — which is the normal state of a watch that has never synced, since a
// watch that has never synced usually has no clock either. The gate falls back to
// the elapsed-minutes counter until then; see evaluateSyncWindow().
struct SyncWindowHour {
  uint32_t hour = 0;  // core::hoursSinceEpoch()
  bool known = false;
};

// The part of the sync state that must survive deep sleep. Lives inside the
// caller's RTC-backed block, so it is only ever read after that block's magic and
// version have been checked — RTC memory is not zeroed and survives reflashing.
struct SyncState {
  // Minutes since a window last OPENED, saturating (never wrapping) — see the
  // header comment.
  //
  // Defaults to the interval, i.e. "due now", which is the same convention as
  // PersistedState::minutes_since_battery_sample. A watch coming out of a fresh
  // persisted block has never synced and its clock is almost certainly unset;
  // making it wait an hour before it is even allowed to ask for the time would
  // mean a newly flashed watch shows `--:--` for that hour with a phone sitting
  // next to it.
  uint16_t minutes_since_window = kSyncWindowIntervalMinutes;

  // The hour the last window opened in — §5.1's primary schedule, with the counter
  // above as its fallback. Defaults to "none recorded", which leaves a freshly
  // flashed watch on the counter, which defaults to "due now".
  SyncWindowHour last_window;

  // PROTOCOL.md §3.2, last paragraph: a plain read of Status, before any write has
  // happened in this connection, reports the result of the *previous* sync from
  // RTC-backed state. That is what makes the app's diagnostic screen work without
  // having to write the clock first.
  //
  // Busy is the honest starting value and is not an invention: it is already the
  // contract's code for "the watch is closing the window; try the next one", it is
  // what core::Status defaults to for the same reason, and the phone counts it as
  // a failed exchange and comes back — which is the correct reading of "this watch
  // has never synced".
  SyncResult last_result = SyncResult::Busy;

  // The last epoch actually committed to the PCF8563; 0 if none ever was. §3.2
  // asks for "the last successfully applied value", not the value from the last
  // attempt, so a failed sync must leave this alone rather than clearing it.
  uint32_t last_applied_epoch_s = 0;
};

// Everything the decision depends on. A default-constructed context refuses (the
// counter starts at 0, i.e. "a window just opened"), which is the same defensive
// default core::TimeWrite and core::Status use: a struct that never reached the
// code meant to fill it in must not read as permission to power up the radio.
struct SyncContext {
  RunMode mode = RunMode::Normal;
  BatteryLevel battery = BatteryLevel::Normal;
  uint16_t minutes_since_window = 0;

  // The wall clock on this wake, and whether it can be believed — core::
  // hoursSinceEpoch(now) and board::rtc::ReadResult::valid. Together with
  // `last_window` they are §5.1's hour boundary; without a trustworthy clock they
  // are ignored and `minutes_since_window` decides instead.
  uint32_t hour = 0;
  bool clock_valid = false;
  SyncWindowHour last_window;

  // The user picked the Sync menu item on **this** wake.
  //
  // Deliberately not part of SyncState, and that is a decision worth stating: a
  // request is a property of the wake it arrived on, and a queued one is a
  // liability. A request latched while the battery is Low would fire hours later
  // when the level recovers, powering up the radio at a moment the wearer has long
  // forgotten about and cannot connect a phone to — and a request that is never
  // cleared is an unbounded retry wearing a different hat. Refused here means
  // refused; the wearer can ask again.
  bool user_requested = false;
};

struct SyncDecision {
  bool open = false;
  // Always consistent with `open`: open == (gate == SyncGate::Open). The default is
  // a refusal.
  SyncGate gate = SyncGate::IntervalNotElapsed;
};

// The §5.1 gates, in the order §5.1 lists them, so a context that fails more than
// one reports the first — mode, then battery, then the interval.
//
// What a user request does and does not do:
//
//   * it overrides the **interval** — that is exactly what "opens a window
//     immediately" means;
//   * it overrides the **battery** gate. That gate exists to stop the firmware
//     spending ~0.9 mAh/day of radio on a standing guess that a phone is nearby;
//     it was never a statement that the radio is unsafe below 10 %. A wearer
//     pressing Sync or Find phone is not a guess, and a watch that refuses to help
//     find the phone it is paired to — at the one moment that is the whole point
//     of the feature — has saved 0.03 mAh and lost the feature. See
//     core::radioPermitted();
//   * it does **not** override the mode gate. Safe and Recovery are faults rather
//     than a low cell: the watch is already failing, the wearer cannot consent
//     their way out of that, and the largest consumer on the board is the last
//     thing to exempt from a degraded mode.
//
// Pure and const: opening a window is a state change and it belongs to
// noteSyncWindowOpened(), which the caller makes separately and *before* the radio
// starts.
SyncDecision evaluateSyncWindow(const SyncContext& context);

// First-boot defaults: due now, nothing synced yet.
void resetSyncState(SyncState& state);

// Advance the fallback timer by however long has passed since the previous wake.
// Called on every wake even while the hour boundary is the thing deciding: a clock
// can stop being readable between two wakes, and this counter has to already be
// right when it does.
// Call once per wake with the same elapsed value the rest of main.cpp uses, so
// that an untrustworthy clock degrades this counter the same way it degrades every
// other one rather than in some way of its own.
//
// Saturates at UINT16_MAX (~45 days) instead of wrapping. A wrap would take a
// watch that had been in a drawer for months and make it look freshly synced,
// locking out the one window it needs to fix its clock.
void advanceSyncTimer(SyncState& state, uint32_t elapsed_minutes);

// **Call this before the radio comes up, not after the window closes.** See the
// header comment: this is the single rule the module exists to enforce, and a
// caller that defers it to a success path has reintroduced the unbounded retry.
//
// `hour` is core::hoursSinceEpoch() of the wake's clock reading and is recorded
// only when `clock_valid` — an hour taken from a clock that cannot be read would
// schedule the next window against fiction. The elapsed counter is reset either
// way, so a window opened with a dead clock still costs the full interval.
void noteSyncWindowOpened(SyncState& state, uint32_t hour, bool clock_valid);

// Record what a window achieved, for §3.2's read path. `applied_utc_epoch_s` is
// only stored when `result` is Ok — a rejected or failed write committed nothing,
// and overwriting the last good value with 0 would tell the app the watch had
// never been set.
void noteSyncResult(SyncState& state, SyncResult result, uint32_t applied_utc_epoch_s);

// §3.2's read path, assembled from RTC-backed state alone: no ADC read, no clock
// read, nothing that costs energy.
//
// `battery_sampled` is whether a battery reading has ever been taken (the caller's
// BatteryFilter::primed()). §3.2 wants kBatteryPercentUnknown rather than a stale
// guess when it has not, and a watch that has not sampled yet holds a plain 0,
// which would otherwise go on the wire as a genuine "0 %".
Status statusFromSyncState(const SyncState& state, uint8_t battery_percent,
                           bool battery_sampled, uint16_t fw_build);

}  // namespace core
