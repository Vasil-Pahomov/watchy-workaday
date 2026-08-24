#include <unity.h>

#include <cstdint>

#include "core/sync_policy.h"

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

// A watch, and the radio traffic it caused. `windows` is the number that decides
// whether this feature costs ~0.75 mAh/day or ~26.
struct Watch {
  SyncState state;
  RunMode mode = RunMode::Normal;
  BatteryLevel battery = BatteryLevel::Normal;
  int windows = 0;
};

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
  core::noteSyncWindowOpened(watch.state);
  if (phone_present) {
    core::noteSyncResult(watch.state, SyncResult::Ok, kGoldenEpoch);
  }
  return true;
}

static SyncContext contextAt(uint16_t minutes_since_window) {
  SyncContext context;
  context.mode = RunMode::Normal;
  context.battery = BatteryLevel::Normal;
  context.minutes_since_window = minutes_since_window;
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

  core::resetSyncState(state);
  TEST_ASSERT_TRUE(core::evaluateSyncWindow(contextAt(state.minutes_since_window)).open);
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

void test_a_low_battery_never_opens_a_window(void) {
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
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::BatteryTooLow),
                        static_cast<int>(core::evaluateSyncWindow(context).gate));

  context.battery = BatteryLevel::Normal;
  context.user_requested = false;
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

void test_a_user_request_does_not_override_a_low_battery(void) {
  // core::radioPermitted() already states this in one line: at these levels the
  // radio stays off "regardless of what the user asked for".
  const BatteryLevel gated[] = {BatteryLevel::Low, BatteryLevel::Critical};
  for (const BatteryLevel level : gated) {
    SyncContext context = contextAt(core::kSyncWindowIntervalMinutes);
    context.battery = level;
    context.user_requested = true;
    const SyncDecision decision = core::evaluateSyncWindow(context);
    TEST_ASSERT_FALSE(decision.open);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncGate::BatteryTooLow),
                          static_cast<int>(decision.gate));
  }
}

void test_a_refused_request_is_not_queued_for_later(void) {
  // A request lives on the wake it arrived on. A queued one would fire hours later
  // when the battery recovered, powering the radio up at a moment the wearer has
  // forgotten about and has no phone ready for — and a request that is never
  // cleared is an unbounded retry wearing a different hat.
  Watch watch;
  watch.battery = BatteryLevel::Low;
  watch.state.minutes_since_window = 0;
  TEST_ASSERT_FALSE(wake(watch, 1, /*user_requested=*/true, /*phone_present=*/false));

  watch.battery = BatteryLevel::Normal;
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
            TEST_ASSERT_EQUAL_INT(static_cast<int>(RunMode::Normal), static_cast<int>(mode));
            TEST_ASSERT_TRUE(core::radioPermitted(level));
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
  RUN_TEST(test_a_low_battery_never_opens_a_window);
  RUN_TEST(test_a_healthy_battery_does_open_a_window);
  RUN_TEST(test_the_gate_names_the_first_rule_that_closed);

  RUN_TEST(test_a_user_request_overrides_the_interval);
  RUN_TEST(test_a_user_window_spends_the_hour_too);
  RUN_TEST(test_a_user_request_does_not_override_a_degraded_mode);
  RUN_TEST(test_a_user_request_does_not_override_a_low_battery);
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
