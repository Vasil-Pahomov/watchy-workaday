#include <unity.h>

#include <cstdint>

#include "core/sync_policy.h"
#include "core/time_model.h"

using core::BatteryLevel;
using core::RunMode;
using core::SyncContext;
using core::SyncDecision;
using core::SyncGate;
using core::SyncResult;
using core::SyncState;

void setUp(void) {}
void tearDown(void) {}

// The §7.1 golden vector's epoch, reused so that anything encoded here can be
// checked against PROTOCOL.md §7.2 byte for byte.
static const uint32_t kGoldenEpoch = 1786970096u;

// An hour-aligned instant, so a window opened at wake zero sits exactly on a
// boundary and the next one is 60 minutes away rather than however far the golden
// vector happens to be from the top of its hour.
static const uint32_t kHourAlignedEpoch = kGoldenEpoch - (kGoldenEpoch % 3600u);

// A watch, and the radio traffic it caused. `windows` is the number that decides
// whether this feature costs ~0.75 mAh/day or ~26.
//
// **Its clock is unreadable, and that is the point of this one.** §5.1 schedules
// the window on the hour boundary and falls back to the elapsed counter when there
// is no clock to find a boundary in — and that fallback is not an exotic path, it
// is every watch that has never synced. ClockedWatch below drives the primary
// schedule; this one drives what happens when there isn't one.
struct Watch {
  SyncState state;
  RunMode mode = RunMode::Normal;
  BatteryLevel battery = BatteryLevel::Normal;
  int windows = 0;
};

// The same watch with a PCF8563 that works. Time is carried as an epoch and turned
// into a DateTime through core::localFromUnixEpoch(), so the arithmetic that
// advances it is the tested kind rather than a second implementation of calendars
// written inside a test.
struct ClockedWatch {
  SyncState state;
  RunMode mode = RunMode::Normal;
  BatteryLevel battery = BatteryLevel::Normal;
  uint32_t epoch_s = kHourAlignedEpoch;
  bool clock_valid = true;
  int windows = 0;
};

static core::DateTime clockOf(const ClockedWatch& watch) {
  return core::localFromUnixEpoch(watch.epoch_s, 0);
}

// One wake of a watch that knows what time it is. Time advances whether or not the
// clock can be *read* this wake, because time does.
static bool clockedWake(ClockedWatch& watch, uint32_t elapsed_minutes, bool user_requested) {
  watch.epoch_s += elapsed_minutes * 60u;
  core::advanceSyncTimer(watch.state, elapsed_minutes);

  SyncContext context;
  context.mode = watch.mode;
  context.battery = watch.battery;
  context.minutes_since_window = watch.state.minutes_since_window;
  context.clock_valid = watch.clock_valid;
  context.hour = core::hoursSinceEpoch(clockOf(watch));
  context.last_window = watch.state.last_window;
  context.user_requested = user_requested;

  const SyncDecision decision = core::evaluateSyncWindow(context);
  if (!decision.open) {
    return false;
  }
  ++watch.windows;
  core::noteSyncWindowOpened(watch.state, context.hour, watch.clock_valid);
  return true;
}

// The minute of the hour this watch's clock is showing.
static uint8_t minuteOf(const ClockedWatch& watch) { return clockOf(watch).minute; }

// One wake, in the order main.cpp and the radio path run it: advance the timer,
// ask the policy, and — when a window is granted — spend the hour **before** the
// radio is touched, then record whatever the window achieved.
//
// A window nobody joined records no result at all: PROTOCOL.md §6.1 makes that the
// normal ending, not a fault, and §3.2 wants a read to report the previous *sync*.
// The tests below lean on that: what a window achieves must make no difference to
// when the next one opens.
static bool wake(Watch& watch, uint32_t elapsed_minutes, bool user_requested,
                 bool phone_present) {
  core::advanceSyncTimer(watch.state, elapsed_minutes);

  SyncContext context;
  context.mode = watch.mode;
  context.battery = watch.battery;
  context.minutes_since_window = watch.state.minutes_since_window;
  context.user_requested = user_requested;

  const SyncDecision decision = core::evaluateSyncWindow(context);
  if (!decision.open) {
    return false;
  }

  ++watch.windows;
  core::noteSyncWindowOpened(watch.state, /*hour=*/0, /*clock_valid=*/false);
  if (phone_present) {
    core::noteSyncResult(watch.state, SyncResult::Ok, kGoldenEpoch);
  }
  return true;
}

// A context with no usable clock, so the elapsed counter is what decides. Every
// test written against this one is a test of §5.1's fallback; the hour boundary
// has its own section below.
static SyncContext contextAt(uint16_t minutes_since_window) {
  SyncContext context;
  context.mode = RunMode::Normal;
  context.battery = BatteryLevel::Normal;
  context.minutes_since_window = minutes_since_window;
  context.clock_valid = false;
  context.user_requested = false;
  return context;
}

// ── the rule the module exists for ───────────────────────────────────────────

void test_the_timer_resets_when_a_window_opens_not_when_it_succeeds(void) {
  // PROTOCOL.md §5.1, and the single most important line in this module. If the
  // hourly timer were reset on success instead, a watch whose phone is out of
  // range would never reset it and would advertise on every wake for the rest of
  // the charge — the unbounded retry Law 2 exists to prevent.
  Watch watch;  // a fresh persisted block is due immediately

  TEST_ASSERT_TRUE(wake(watch, 1, /*user_requested=*/false, /*phone_present=*/false));

  // The window achieved nothing. That must buy exactly the same hour of silence a
  // successful one buys.
  for (int minute = 0; minute < 59; ++minute) {
    TEST_ASSERT_FALSE(wake(watch, 1, false, false));
  }
  TEST_ASSERT_EQUAL_INT(1, watch.windows);

  TEST_ASSERT_TRUE(wake(watch, 1, false, false));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_a_watch_with_no_phone_in_range_opens_24_windows_a_day_not_1440(void) {
  // The same rule priced out over a day. 1440 wakes must produce 24 windows
  // (~0.75 mAh/day, PROTOCOL.md §5.3) and not 1440 (~26 mAh/day against a
  // 9.5 mAh/day allowance — a flat cell in about a week).
  Watch watch;
  for (int minute = 0; minute < 1440; ++minute) {
    wake(watch, 1, /*user_requested=*/false, /*phone_present=*/false);
  }
  TEST_ASSERT_EQUAL_INT(24, watch.windows);
}

void test_a_successful_window_costs_the_same_hour(void) {
  // The other half: success does not earn a shorter interval either.
  Watch watch;
  for (int minute = 0; minute < 1440; ++minute) {
    wake(watch, 1, /*user_requested=*/false, /*phone_present=*/true);
  }
  TEST_ASSERT_EQUAL_INT(24, watch.windows);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Ok), static_cast<int>(watch.state.last_result));
}

void test_recording_a_result_does_not_touch_the_timer(void) {
  // noteSyncWindowOpened() is the only thing that may reset the hour. If recording
  // an outcome reset it too, the rule above would quietly become reset-on-success.
  SyncState state;
  state.minutes_since_window = 42;
  core::noteSyncResult(state, SyncResult::Ok, kGoldenEpoch);
  TEST_ASSERT_EQUAL_UINT16(42, state.minutes_since_window);
}

void test_a_window_is_never_granted_twice_in_the_same_minute(void) {
  Watch watch;
  TEST_ASSERT_TRUE(wake(watch, 1, false, false));
  TEST_ASSERT_FALSE(wake(watch, 0, false, false));
  TEST_ASSERT_EQUAL_INT(1, watch.windows);
}

// ── the hourly boundary ──────────────────────────────────────────────────────

void test_the_hourly_boundary_is_exact(void) {
  // §5.1: "the last sync is under 60 minutes old" closes the gate, so 60 opens it.
  TEST_ASSERT_FALSE(core::evaluateSyncWindow(contextAt(0)).open);
  TEST_ASSERT_FALSE(
      core::evaluateSyncWindow(contextAt(core::kSyncWindowIntervalMinutes - 1)).open);
  TEST_ASSERT_TRUE(core::evaluateSyncWindow(contextAt(core::kSyncWindowIntervalMinutes)).open);
  TEST_ASSERT_TRUE(
      core::evaluateSyncWindow(contextAt(core::kSyncWindowIntervalMinutes + 1)).open);
}

void test_a_fresh_persisted_block_is_due_immediately(void) {
  // A watch out of the box has never synced and its PCF8563 is unset. Making it
  // wait an hour before it may even ask for the time would mean `--:--` for that
  // hour with the phone sitting next to it.
  SyncState state;
  TEST_ASSERT_TRUE(core::evaluateSyncWindow(contextAt(state.minutes_since_window)).open);
}

void test_reset_returns_the_state_to_a_fresh_watch(void) {
  SyncState state;
  state.minutes_since_window = 3;
  core::noteSyncResult(state, SyncResult::RtcWriteFailed, 0);
  state.last_applied_epoch_s = kGoldenEpoch;

  state.last_window.hour = 12345;
  state.last_window.known = true;

  core::resetSyncState(state);
  TEST_ASSERT_TRUE(core::evaluateSyncWindow(contextAt(state.minutes_since_window)).open);
  TEST_ASSERT_FALSE(state.last_window.known);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Busy), static_cast<int>(state.last_result));
  TEST_ASSERT_EQUAL_UINT32(0, state.last_applied_epoch_s);
}

// ── the gates that outrank the schedule ──────────────────────────────────────

void test_a_degraded_mode_never_opens_a_window(void) {
  // Law 1: no radio in a degraded mode. Safe and Recovery exist to keep a faulting
  // watch alive for weeks on what is left of the cell.
  const RunMode gated[] = {RunMode::Safe, RunMode::Recovery};
  for (const RunMode mode : gated) {
    SyncContext context = contextAt(core::kSyncWindowIntervalMinutes * 100);
    context.mode = mode;
    const SyncDecision decision = core::evaluateSyncWindow(context);
    TEST_ASSERT_FALSE(decision.open);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::DegradedMode),
                          static_cast<int>(decision.gate));
  }
}

void test_a_low_battery_never_opens_a_SCHEDULED_window(void) {
  const BatteryLevel gated[] = {BatteryLevel::Low, BatteryLevel::Critical};
  for (const BatteryLevel level : gated) {
    SyncContext context = contextAt(core::kSyncWindowIntervalMinutes * 100);
    context.battery = level;
    const SyncDecision decision = core::evaluateSyncWindow(context);
    TEST_ASSERT_FALSE(decision.open);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::BatteryTooLow),
                          static_cast<int>(decision.gate));
  }
}

void test_a_healthy_battery_does_open_a_window(void) {
  // The other half of the battery gate, so the test above is not passing because
  // nothing ever opens.
  const BatteryLevel healthy[] = {BatteryLevel::Normal, BatteryLevel::Full};
  for (const BatteryLevel level : healthy) {
    SyncContext context = contextAt(core::kSyncWindowIntervalMinutes);
    context.battery = level;
    TEST_ASSERT_TRUE(core::evaluateSyncWindow(context).open);
  }
}

void test_the_gate_names_the_first_rule_that_closed(void) {
  // §5.1 lists mode, battery, interval in that order, and the UI needs to be able
  // to say something true about a refusal rather than appearing to ignore it.
  SyncContext context = contextAt(0);
  context.mode = RunMode::Safe;
  context.battery = BatteryLevel::Critical;
  context.user_requested = true;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::DegradedMode),
                        static_cast<int>(core::evaluateSyncWindow(context).gate));

  context.mode = RunMode::Normal;
  context.user_requested = false;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::BatteryTooLow),
                        static_cast<int>(core::evaluateSyncWindow(context).gate));

  context.battery = BatteryLevel::Normal;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::IntervalNotElapsed),
                        static_cast<int>(core::evaluateSyncWindow(context).gate));

  context.user_requested = true;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::Open),
                        static_cast<int>(core::evaluateSyncWindow(context).gate));
}

// ── the user asking for a sync ───────────────────────────────────────────────

void test_a_user_request_overrides_the_interval(void) {
  // §5.1: "the Sync menu item opens a window immediately".
  SyncContext context = contextAt(0);
  TEST_ASSERT_FALSE(core::evaluateSyncWindow(context).open);

  context.user_requested = true;
  const SyncDecision decision = core::evaluateSyncWindow(context);
  TEST_ASSERT_TRUE(decision.open);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::Open), static_cast<int>(decision.gate));
}

void test_a_user_window_spends_the_hour_too(void) {
  // "...and resets the hourly timer" — the rest of the same §5.1 line. Without it,
  // pressing Sync twice would be two windows a minute apart.
  Watch watch;
  watch.state.minutes_since_window = 0;
  TEST_ASSERT_TRUE(wake(watch, 0, /*user_requested=*/true, /*phone_present=*/false));
  TEST_ASSERT_FALSE(wake(watch, 1, /*user_requested=*/false, false));
  TEST_ASSERT_EQUAL_UINT16(1, watch.state.minutes_since_window);
}

void test_a_user_request_does_not_override_a_degraded_mode(void) {
  // The decision this module makes and PROTOCOL.md §5.1 leaves as a list rather
  // than a precedence: a request overrides the *schedule* and nothing else. Safe
  // and Recovery are what keep a faulting watch alive, and the largest consumer on
  // the board is the last thing to exempt from them.
  const RunMode gated[] = {RunMode::Safe, RunMode::Recovery};
  for (const RunMode mode : gated) {
    SyncContext context = contextAt(0);
    context.mode = mode;
    context.user_requested = true;
    TEST_ASSERT_FALSE(core::evaluateSyncWindow(context).open);
  }
}

void test_a_user_request_overrides_a_low_battery(void) {
  // The battery gate stops the firmware spending ~0.9 mAh/day of radio on a
  // standing guess that a phone is nearby. It was never a claim that the radio is
  // unsafe below 10 %, and a wearer pressing Sync or Find phone is not a guess —
  // refusing them saves ~0.03 mAh and loses the one moment the feature exists for.
  const BatteryLevel gated[] = {BatteryLevel::Low, BatteryLevel::Critical};
  for (const BatteryLevel level : gated) {
    // Not due on the schedule either, so nothing but the request can be opening it.
    SyncContext context = contextAt(0);
    context.battery = level;
    TEST_ASSERT_FALSE(core::evaluateSyncWindow(context).open);

    context.user_requested = true;
    const SyncDecision decision = core::evaluateSyncWindow(context);
    TEST_ASSERT_TRUE(decision.open);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::Open), static_cast<int>(decision.gate));
  }
}

void test_only_the_mode_can_refuse_a_request(void) {
  // The property the Find phone screen depends on: a press has exactly one refusal
  // left to render, so core::findOutcomeForGate()'s BatteryTooLow arm is now
  // unreachable from a press. Swept rather than asserted case by case, because the
  // thing being checked is that no *other* gate can close on a request.
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
  const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal, BatteryLevel::Low,
                                 BatteryLevel::Critical};
  const uint16_t elapsed[] = {0, core::kSyncWindowIntervalMinutes, UINT16_MAX};

  for (const RunMode mode : modes) {
    for (const BatteryLevel level : levels) {
      for (const uint16_t minutes : elapsed) {
        SyncContext context = contextAt(minutes);
        context.mode = mode;
        context.battery = level;
        context.user_requested = true;

        const SyncDecision decision = core::evaluateSyncWindow(context);
        const bool degraded = mode != RunMode::Normal;
        TEST_ASSERT_EQUAL_INT(!degraded, decision.open);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(degraded ? SyncGate::DegradedMode : SyncGate::Open),
            static_cast<int>(decision.gate));
      }
    }
  }
}

void test_a_refused_request_is_not_queued_for_later(void) {
  // A request lives on the wake it arrived on. A queued one would fire hours later
  // when the fault cleared, powering the radio up at a moment the wearer has
  // forgotten about and has no phone ready for — and a request that is never
  // cleared is an unbounded retry wearing a different hat.
  //
  // Driven through the degraded-mode gate, which is the only one left that can
  // refuse a press: the battery gate now stops the schedule and not the wearer.
  Watch watch;
  watch.mode = RunMode::Safe;
  watch.state.minutes_since_window = 0;
  TEST_ASSERT_FALSE(wake(watch, 1, /*user_requested=*/true, /*phone_present=*/false));

  watch.mode = RunMode::Normal;
  for (int minute = 0; minute < 58; ++minute) {
    TEST_ASSERT_FALSE(wake(watch, 1, false, false));
  }
  TEST_ASSERT_EQUAL_INT(0, watch.windows);

  // And the refusal did not spend the hour either: the scheduled window still
  // arrives on time.
  TEST_ASSERT_TRUE(wake(watch, 1, false, false));
  TEST_ASSERT_EQUAL_INT(1, watch.windows);
}

// ── the counter, and the clock behind it ─────────────────────────────────────

void test_an_unreadable_clock_neither_stalls_nor_storms_the_window(void) {
  // A dead PCF8563, driven through the real fallback rather than a hand-picked
  // elapsed: core::elapsedMinutes() is where the decision lives, and this wires it
  // to the policy exactly as main.cpp does. The guarantee PROTOCOL.md §5.1 states
  // is a property of the pair — neither half can make it alone — and both failure
  // directions are live here. A counter that stopped advancing would lock a
  // dead-clock watch out of the one window that could set its clock (0 windows);
  // a gate that evaporated with the clock would advertise on every wake (1440).
  //
  // The fallback's own branches — no previous reading, a backwards clock, the
  // 30-day clamp, the floor at 1 — are covered in test_time_model.
  const core::DateTime unusable{};  // what a watch with no trustworthy time holds

  Watch watch;
  for (int minute = 0; minute < 1440; ++minute) {
    const uint32_t elapsed =
        core::elapsedMinutes(unusable, unusable, /*now_valid=*/false, core::kNormalTickSeconds);
    wake(watch, elapsed, /*user_requested=*/false, /*phone_present=*/false);
  }

  TEST_ASSERT_EQUAL_INT(24, watch.windows);
}

void test_a_recovering_clock_does_not_change_the_cadence(void) {
  // The RTC comes back mid-day: elapsed switches from inferred to measured and the
  // hour keeps its meaning across the change, because both feed the same counter.
  Watch watch;
  const core::DateTime unusable{};

  for (int minute = 0; minute < 30; ++minute) {
    wake(watch, core::elapsedMinutes(unusable, unusable, false, core::kNormalTickSeconds), false,
         false);
  }
  TEST_ASSERT_EQUAL_INT(1, watch.windows);  // the boot window, from a fresh block

  core::DateTime last{};
  last.year = 2026;
  last.month = 8;
  last.day = 17;
  last.hour = 12;
  last.minute = 0;

  for (int minute = 1; minute <= 31; ++minute) {
    core::DateTime now = last;
    now.minute = static_cast<uint8_t>(minute);
    wake(watch, core::elapsedMinutes(last, now, /*now_valid=*/true, core::kNormalTickSeconds),
         false, false);
    last = now;
  }
  // The hour is counted across the change of regime rather than restarting with
  // it: 29 inferred minutes since the boot window plus 31 measured ones is 60.
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_a_wake_where_no_time_passed_spends_nothing(void) {
  // Two wakes inside one minute — a button press right after a tick. The clock is
  // fine and genuinely reports zero elapsed; the counter must simply not move.
  SyncState state;
  state.minutes_since_window = 17;
  core::advanceSyncTimer(state, 0);
  TEST_ASSERT_EQUAL_UINT16(17, state.minutes_since_window);
}

void test_the_timer_saturates_instead_of_wrapping(void) {
  // A wrap is worse than a stuck maximum: a counter that rolled over would come
  // back reading a few minutes and lock the watch out for another hour, and it
  // would do it repeatedly.
  SyncState state;
  state.minutes_since_window = 0;

  core::advanceSyncTimer(state, 0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, state.minutes_since_window);

  core::advanceSyncTimer(state, 0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, state.minutes_since_window);

  core::advanceSyncTimer(state, 1);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, state.minutes_since_window);
  TEST_ASSERT_TRUE(core::evaluateSyncWindow(contextAt(state.minutes_since_window)).open);
}

void test_a_long_absence_opens_exactly_one_window(void) {
  // 30 days is the largest delta elapsedMinutes() will report before it gives up
  // and infers one. Coming back from it is one window, not a burst.
  Watch watch;
  watch.state.minutes_since_window = 0;

  TEST_ASSERT_TRUE(wake(watch, 60u * 24u * 30u, false, false));
  TEST_ASSERT_EQUAL_INT(1, watch.windows);
  TEST_ASSERT_FALSE(wake(watch, 1, false, false));
  TEST_ASSERT_EQUAL_INT(1, watch.windows);
}

void test_a_saturated_counter_still_opens_and_then_resets(void) {
  // Every wake here reports an absurd elapsed — 69 days — so the counter pins at
  // its maximum and every wake genuinely is more than an hour after the last. The
  // point is that it keeps working at the pin: an off-by-one in the saturation
  // would show up as a counter stuck below the interval (never syncs again) or as
  // one that never resets (a window every wake regardless of elapsed time).
  Watch watch;
  for (int wake_index = 0; wake_index < 5000; ++wake_index) {
    TEST_ASSERT_TRUE(wake(watch, 100000u, false, false));
    TEST_ASSERT_EQUAL_UINT16(0, watch.state.minutes_since_window);
  }
  TEST_ASSERT_EQUAL_INT(5000, watch.windows);
}

// ── the hour boundary (§5.1's primary schedule) ─────────────────────────

void test_a_clocked_watch_opens_a_window_on_each_hour(void) {
  // Same cadence as the counter fallback — 24 a day, ~0.75 mAh — but at times
  // anybody can predict instead of at whatever minute the watch happened to boot.
  ClockedWatch watch;
  // The boot window first, and on the hour, so what is counted below is the
  // schedule and not the fresh block's "due now".
  TEST_ASSERT_TRUE(clockedWake(watch, 0, /*user_requested=*/false));
  watch.windows = 0;

  for (int minute = 0; minute < 1440; ++minute) {
    clockedWake(watch, 1, /*user_requested=*/false);
  }
  TEST_ASSERT_EQUAL_INT(24, watch.windows);
}

void test_the_window_moves_onto_the_hour_whatever_minute_the_watch_booted_on(void) {
  // The defect the boundary replaces: a free-running hourly timer met the phone at
  // :23 past every hour for the rest of the charge, because that is when the watch
  // was first switched on. The first window is still immediate — a fresh block has
  // never synced and the fallback says so — and the *second* is what moves.
  ClockedWatch watch;
  watch.epoch_s += 23u * 60u;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));
  TEST_ASSERT_EQUAL_UINT8(23, minuteOf(watch));

  // 36 minutes of silence, then the top of the hour.
  for (int minute = 0; minute < 36; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_UINT8(0, minuteOf(watch));

  // And it stays there.
  for (int minute = 0; minute < 59; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_UINT8(0, minuteOf(watch));
  TEST_ASSERT_EQUAL_INT(3, watch.windows);
}

void test_the_boundary_is_found_at_a_five_minute_tick_too(void) {
  // Saving mode ticks every five minutes and core::alignedTickMinutes() puts those
  // ticks on minutes divisible by five. Minute 0 is one of them, so the window is
  // still reachable — which is the whole reason the alignment and the boundary
  // have to agree on 60 being divisible by both.
  ClockedWatch watch;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));  // the boot window, on the hour

  for (int tick = 0; tick < 11; ++tick) {
    TEST_ASSERT_FALSE(clockedWake(watch, 5, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 5, false));
  TEST_ASSERT_EQUAL_UINT8(0, minuteOf(watch));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_the_boundary_survives_midnight(void) {
  // dt.hour alone would compare 23 against 23 a day later and call it the same
  // hour. hoursSinceEpoch() is monotonic, which is the entire reason it exists.
  ClockedWatch watch;
  watch.epoch_s = kGoldenEpoch - (kGoldenEpoch % 86400u) + 23u * 3600u;  // 23:00 UTC
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));
  TEST_ASSERT_EQUAL_UINT8(23, clockOf(watch).hour);

  for (int minute = 0; minute < 59; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_UINT8(0, clockOf(watch).hour);
}

void test_a_day_in_a_drawer_is_one_window_and_not_none(void) {
  // The other half of the same property: 23:xx to 23:xx the next day is a
  // different hour, so the watch syncs when it comes back rather than waiting for
  // the clock to pass an hour number it is already sitting on.
  ClockedWatch watch;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));

  TEST_ASSERT_TRUE(clockedWake(watch, 60u * 24u, false));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
  TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_a_request_at_59_past_does_not_swallow_the_window_on_the_hour(void) {
  // A press records the hour it happened in, so the boundary a minute later is
  // still a different hour and still opens. Two windows a minute apart is the
  // intended reading of "the wearer asked" — and it is bounded at one extra
  // window per press, which is what keeps the schedule's 24 a day a ceiling.
  ClockedWatch watch;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));  // the boot window, on the hour

  TEST_ASSERT_TRUE(clockedWake(watch, 59, /*user_requested=*/true));
  TEST_ASSERT_EQUAL_UINT8(59, minuteOf(watch));

  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_UINT8(0, minuteOf(watch));
  TEST_ASSERT_EQUAL_INT(3, watch.windows);

  // And the press did not earn a third one inside the new hour.
  for (int minute = 0; minute < 59; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_EQUAL_INT(3, watch.windows);
}

void test_pressing_sync_all_hour_cannot_beat_the_schedule_to_the_radio(void) {
  // A user request opens a window every time it is made — that is §5.1 — but each
  // one spends the hour, so the *scheduled* windows are not multiplied by it.
  ClockedWatch watch;
  for (int minute = 0; minute < 60; ++minute) {
    clockedWake(watch, 1, /*user_requested=*/true);
  }
  TEST_ASSERT_EQUAL_INT(60, watch.windows);

  // The hour recorded is the one the last press happened in, so the next scheduled
  // window is the next boundary and not a 61st press-shaped one.
  TEST_ASSERT_FALSE(clockedWake(watch, 0, false));
}

void test_a_clock_that_dies_falls_back_to_the_counter(void) {
  ClockedWatch watch;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));

  watch.clock_valid = false;
  for (int minute = 0; minute < 59; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_a_clock_that_comes_back_returns_to_the_boundary(void) {
  // The recorded hour is left alone while the clock is unreadable rather than
  // cleared, so the watch does not drop onto the counter for an hour on every bad
  // read — and when the clock returns, the hour has genuinely turned.
  ClockedWatch watch;
  TEST_ASSERT_TRUE(clockedWake(watch, 0, false));

  watch.clock_valid = false;
  for (int minute = 0; minute < 30; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }

  watch.clock_valid = true;
  for (int minute = 0; minute < 29; ++minute) {
    TEST_ASSERT_FALSE(clockedWake(watch, 1, false));
  }
  TEST_ASSERT_TRUE(clockedWake(watch, 1, false));
  TEST_ASSERT_EQUAL_UINT8(0, minuteOf(watch));
  TEST_ASSERT_EQUAL_INT(2, watch.windows);
}

void test_a_window_opened_with_a_dead_clock_records_no_hour(void) {
  // An hour taken from a clock that cannot be read would schedule the next window
  // against fiction. The elapsed counter is reset either way, so the window still
  // costs the full interval.
  SyncState state;
  state.minutes_since_window = 42;
  core::noteSyncWindowOpened(state, /*hour=*/999u, /*clock_valid=*/false);
  TEST_ASSERT_EQUAL_UINT16(0, state.minutes_since_window);
  TEST_ASSERT_FALSE(state.last_window.known);

  core::noteSyncWindowOpened(state, /*hour=*/999u, /*clock_valid=*/true);
  TEST_ASSERT_TRUE(state.last_window.known);
  TEST_ASSERT_EQUAL_UINT32(999u, state.last_window.hour);
}

// ── what a read of Status reports (§3.2) ─────────────────────────────────────

void test_status_is_answered_from_rtc_backed_state_alone(void) {
  // §3.2's last paragraph: a read before any write reports the previous sync from
  // persisted state, so the app's diagnostic screen works without setting the
  // clock first. No ADC read, no RTC read, no radio state.
  SyncState state;
  core::noteSyncResult(state, SyncResult::Ok, kGoldenEpoch);

  const core::Status status = core::statusFromSyncState(state, 78, /*battery_sampled=*/true, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Ok), static_cast<int>(status.result));
  TEST_ASSERT_EQUAL_UINT8(78, status.battery_percent);
  TEST_ASSERT_EQUAL_UINT32(kGoldenEpoch, status.applied_utc_epoch_s);
  TEST_ASSERT_EQUAL_UINT16(1, status.fw_build);

  // And it is PROTOCOL.md §7.2's golden vector, byte for byte, straight out of the
  // persisted block.
  uint8_t out[core::kStatusPayloadLength] = {};
  TEST_ASSERT_TRUE(core::encodeStatus(out, sizeof(out), status));
  const uint8_t expected[core::kStatusPayloadLength] = {0x01, 0x81, 0x00, 0x4E, 0xF0, 0xFF,
                                                        0x82, 0x6A, 0x01, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, core::kStatusPayloadLength);
}

void test_a_watch_that_has_never_synced_says_so(void) {
  SyncState state;
  const core::Status status = core::statusFromSyncState(state, 0, /*battery_sampled=*/false, 7);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Busy), static_cast<int>(status.result));
  TEST_ASSERT_EQUAL_UINT32(0, status.applied_utc_epoch_s);
  // §3.2: 0xFF rather than a stale guess. A watch that has not sampled holds a
  // plain 0, which is a valid percentage and would read as a flat battery.
  TEST_ASSERT_EQUAL_UINT8(core::kBatteryPercentUnknown, status.battery_percent);
}

void test_a_real_zero_percent_is_not_reported_as_unknown(void) {
  // The other side of the same field: 0 % is a genuine reading once a sample has
  // been taken, and must not be laundered into "unknown".
  SyncState state;
  const core::Status status = core::statusFromSyncState(state, 0, /*battery_sampled=*/true, 0);
  TEST_ASSERT_EQUAL_UINT8(0, status.battery_percent);
}

void test_a_failed_sync_keeps_the_epoch_that_really_was_applied(void) {
  // §3.2 asks for "the last successfully applied value", not the last attempted
  // one. Zeroing it here would tell the app the watch had never been set.
  SyncState state;
  core::noteSyncResult(state, SyncResult::Ok, kGoldenEpoch);
  core::noteSyncResult(state, SyncResult::RtcWriteFailed, 999u);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::RtcWriteFailed),
                        static_cast<int>(state.last_result));
  TEST_ASSERT_EQUAL_UINT32(kGoldenEpoch, state.last_applied_epoch_s);

  core::noteSyncResult(state, SyncResult::BadVersion, 0);
  TEST_ASSERT_EQUAL_UINT32(kGoldenEpoch, state.last_applied_epoch_s);
}

void test_a_corrupt_persisted_result_never_reaches_the_wire(void) {
  // last_result lives in RTC memory, which is not zeroed and survives reflashing.
  // The block's magic and version catch a stale layout; this catches a corrupt
  // byte inside a block that still looks valid. A code outside §3.2's table is one
  // the app has no defined reading for.
  SyncState state;
  state.last_result = static_cast<SyncResult>(0xAB);
  const core::Status status = core::statusFromSyncState(state, 50, true, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Busy), static_cast<int>(status.result));

  core::noteSyncResult(state, static_cast<SyncResult>(0x7F), 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncResult::Busy), static_cast<int>(state.last_result));
}

// ── the invariant that keeps the decision honest ─────────────────────────────

void test_open_and_the_gate_never_disagree(void) {
  // Two fields describing one decision can drift apart, and the one that would be
  // acted on is `open`. Also checks the properties a granted window must have, so
  // a future edit cannot loosen a gate without this failing.
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
  const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal, BatteryLevel::Low,
                                 BatteryLevel::Critical};
  const uint16_t minutes[] = {0, 1, static_cast<uint16_t>(core::kSyncWindowIntervalMinutes - 1),
                              core::kSyncWindowIntervalMinutes, UINT16_MAX};

  bool granted_at_least_once = false;

  for (const RunMode mode : modes) {
    for (const BatteryLevel level : levels) {
      for (const uint16_t elapsed : minutes) {
        for (int requested = 0; requested < 2; ++requested) {
          SyncContext context;
          context.mode = mode;
          context.battery = level;
          context.minutes_since_window = elapsed;
          context.user_requested = (requested != 0);

          const SyncDecision decision = core::evaluateSyncWindow(context);
          TEST_ASSERT_EQUAL_INT(decision.open, decision.gate == SyncGate::Open);

          if (decision.open) {
            granted_at_least_once = true;
            // The mode gate binds everything. The battery and the schedule bind
            // only what the wearer did not ask for.
            TEST_ASSERT_EQUAL_INT(static_cast<int>(RunMode::Normal), static_cast<int>(mode));
            TEST_ASSERT_TRUE(context.user_requested || core::radioPermitted(level));
            TEST_ASSERT_TRUE(context.user_requested ||
                             elapsed >= core::kSyncWindowIntervalMinutes);
          }
        }
      }
    }
  }

  TEST_ASSERT_TRUE(granted_at_least_once);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_the_timer_resets_when_a_window_opens_not_when_it_succeeds);
  RUN_TEST(test_a_watch_with_no_phone_in_range_opens_24_windows_a_day_not_1440);
  RUN_TEST(test_a_successful_window_costs_the_same_hour);
  RUN_TEST(test_recording_a_result_does_not_touch_the_timer);
  RUN_TEST(test_a_window_is_never_granted_twice_in_the_same_minute);

  RUN_TEST(test_the_hourly_boundary_is_exact);
  RUN_TEST(test_a_fresh_persisted_block_is_due_immediately);
  RUN_TEST(test_reset_returns_the_state_to_a_fresh_watch);

  RUN_TEST(test_a_degraded_mode_never_opens_a_window);
  RUN_TEST(test_a_low_battery_never_opens_a_SCHEDULED_window);
  RUN_TEST(test_a_healthy_battery_does_open_a_window);
  RUN_TEST(test_the_gate_names_the_first_rule_that_closed);

  RUN_TEST(test_a_user_request_overrides_the_interval);
  RUN_TEST(test_a_user_window_spends_the_hour_too);
  RUN_TEST(test_a_user_request_does_not_override_a_degraded_mode);
  RUN_TEST(test_a_user_request_overrides_a_low_battery);
  RUN_TEST(test_only_the_mode_can_refuse_a_request);

  RUN_TEST(test_a_clocked_watch_opens_a_window_on_each_hour);
  RUN_TEST(test_the_window_moves_onto_the_hour_whatever_minute_the_watch_booted_on);
  RUN_TEST(test_the_boundary_is_found_at_a_five_minute_tick_too);
  RUN_TEST(test_the_boundary_survives_midnight);
  RUN_TEST(test_a_day_in_a_drawer_is_one_window_and_not_none);
  RUN_TEST(test_a_request_at_59_past_does_not_swallow_the_window_on_the_hour);
  RUN_TEST(test_pressing_sync_all_hour_cannot_beat_the_schedule_to_the_radio);
  RUN_TEST(test_a_clock_that_dies_falls_back_to_the_counter);
  RUN_TEST(test_a_clock_that_comes_back_returns_to_the_boundary);
  RUN_TEST(test_a_window_opened_with_a_dead_clock_records_no_hour);
  RUN_TEST(test_a_refused_request_is_not_queued_for_later);

  RUN_TEST(test_an_unreadable_clock_neither_stalls_nor_storms_the_window);
  RUN_TEST(test_a_recovering_clock_does_not_change_the_cadence);
  RUN_TEST(test_a_wake_where_no_time_passed_spends_nothing);
  RUN_TEST(test_the_timer_saturates_instead_of_wrapping);
  RUN_TEST(test_a_long_absence_opens_exactly_one_window);
  RUN_TEST(test_a_saturated_counter_still_opens_and_then_resets);

  RUN_TEST(test_status_is_answered_from_rtc_backed_state_alone);
  RUN_TEST(test_a_watch_that_has_never_synced_says_so);
  RUN_TEST(test_a_real_zero_percent_is_not_reported_as_unknown);
  RUN_TEST(test_a_failed_sync_keeps_the_epoch_that_really_was_applied);
  RUN_TEST(test_a_corrupt_persisted_result_never_reaches_the_wire);

  RUN_TEST(test_open_and_the_gate_never_disagree);

  return UNITY_END();
}
