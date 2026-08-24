#include <unity.h>

#include "core/sync_policy.h"
#include "core/wake_router.h"

using core::BatteryLevel;
using core::RunMode;
using core::WakeContext;
using core::WakePlan;
using core::WakeSource;

void setUp(void) {}
void tearDown(void) {}

// A context where the battery sample is NOT due and no sync window is due either,
// so tests that care about other peripherals are not confused by an incidental ADC
// power-up or by the radio.
static WakeContext freshContext(void) {
  WakeContext context;
  context.mode = RunMode::Normal;
  context.battery = BatteryLevel::Normal;
  context.minutes_since_battery_sample = 0;
  context.accel_features_enabled = false;
  context.ui_active = false;
  context.minutes_since_sync_window = 0;
  context.sync_requested = false;
  return context;
}

// The same context an hour later: a sync window is due.
static WakeContext syncDueContext(void) {
  WakeContext context = freshContext();
  context.minutes_since_sync_window = core::kSyncWindowIntervalMinutes;
  return context;
}

// ── the common case: a minute tick ───────────────────────────────────────────

void test_minute_tick_is_minimal(void) {
  // 1440 of these a day. Everything switched on here is paid for 1440 times.
  const WakePlan plan = core::routeWake(WakeSource::RtcAlarm, freshContext());

  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_FALSE(plan.need_battery);  // not due
  TEST_ASSERT_FALSE(plan.need_accel);    // motion features off
  TEST_ASSERT_FALSE(plan.run_ui);
  TEST_ASSERT_FALSE(plan.force_full_refresh);
  TEST_ASSERT_EQUAL_UINT16(60, plan.next_tick_seconds);
}

void test_battery_is_sampled_on_a_schedule_not_every_tick(void) {
  WakeContext context = freshContext();

  context.minutes_since_battery_sample = core::kBatterySampleIntervalMinutes - 1;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::RtcAlarm, context).need_battery);

  context.minutes_since_battery_sample = core::kBatterySampleIntervalMinutes;
  TEST_ASSERT_TRUE(core::routeWake(WakeSource::RtcAlarm, context).need_battery);

  context.minutes_since_battery_sample = core::kBatterySampleIntervalMinutes * 10;
  TEST_ASSERT_TRUE(core::routeWake(WakeSource::RtcAlarm, context).need_battery);
}

void test_accelerometer_is_opt_in(void) {
  // Arming the BMA423 interrupt costs up to 14 uA — about a quarter of the whole
  // sleep floor — so it must stay off unless a feature actually needs it.
  WakeContext context = freshContext();
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::RtcAlarm, context).need_accel);

  context.accel_features_enabled = true;
  TEST_ASSERT_TRUE(core::routeWake(WakeSource::RtcAlarm, context).need_accel);
}

// ── other wake sources ───────────────────────────────────────────────────────

void test_power_on_establishes_a_baseline(void) {
  const WakePlan plan = core::routeWake(WakeSource::PowerOn, freshContext());
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_TRUE(plan.need_battery);  // regardless of schedule
  TEST_ASSERT_TRUE(plan.force_full_refresh);
}

void test_button_wake_is_cheap(void) {
  // A button press needs the clock to render and nothing else.
  const WakePlan plan = core::routeWake(WakeSource::Button, freshContext());
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_TRUE(plan.run_ui);
  TEST_ASSERT_FALSE(plan.need_accel);
  TEST_ASSERT_FALSE(plan.need_battery);
  TEST_ASSERT_FALSE(plan.force_full_refresh);
}

void test_accelerometer_wake_does_not_touch_the_panel(void) {
  // Motion alone is not worth ~85 % of a wake's energy; the step count is shown
  // at the next tick.
  WakeContext context = freshContext();
  context.accel_features_enabled = true;

  const WakePlan plan = core::routeWake(WakeSource::Accelerometer, context);
  TEST_ASSERT_TRUE(plan.need_accel);
  TEST_ASSERT_FALSE(plan.need_display);
  TEST_ASSERT_FALSE(plan.need_rtc);
  TEST_ASSERT_FALSE(plan.need_battery);
}

void test_accelerometer_wake_updates_a_live_screen(void) {
  WakeContext context = freshContext();
  context.accel_features_enabled = true;
  context.ui_active = true;

  const WakePlan plan = core::routeWake(WakeSource::Accelerometer, context);
  TEST_ASSERT_TRUE(plan.need_accel);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_TRUE(plan.need_rtc);
}

void test_timer_backstop_behaves_like_a_tick(void) {
  // Reached only when the RTC alarm failed to arrive — the watch must still tick.
  const WakePlan plan = core::routeWake(WakeSource::Timer, freshContext());
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_FALSE(plan.need_battery);
  TEST_ASSERT_FALSE(plan.need_accel);  // motion features off
}

void test_unknown_wake_resynchronises_the_panel(void) {
  // Provenance unknown, so the panel may not match the state; one full refresh
  // beats leaving a stale screen up indefinitely.
  const WakePlan plan = core::routeWake(WakeSource::Unknown, freshContext());
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_TRUE(plan.force_full_refresh);
}

void test_every_tick_like_wake_collects_the_step_counter(void) {
  // A dead PCF8563 leaves the watch running on the timer backstop indefinitely.
  // If those wakes skipped the sensor, its free-running counter would drift past
  // what the step logic can plausibly credit and the count would stall — so every
  // wake that already opens the bus for the RTC collects the sensor too.
  WakeContext context = freshContext();
  context.accel_features_enabled = true;

  const WakeSource tick_like[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Timer,
                                  WakeSource::Unknown};
  for (const WakeSource source : tick_like) {
    const WakePlan plan = core::routeWake(source, context);
    TEST_ASSERT_TRUE(plan.need_accel);
    TEST_ASSERT_TRUE(plan.need_i2c);
  }
}

void test_the_step_counter_is_still_opt_in_on_every_wake_source(void) {
  // The other half of the invariant: with the feature off, no wake source may
  // touch the sensor. The BMA423 is a quarter of the sleep floor.
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Timer,   WakeSource::Unknown};
  for (const WakeSource source : sources) {
    TEST_ASSERT_FALSE(core::routeWake(source, freshContext()).need_accel);
  }
}

// ── degraded modes ───────────────────────────────────────────────────────────

void test_low_battery_stretches_the_tick(void) {
  WakeContext context = freshContext();
  context.battery = BatteryLevel::Low;
  TEST_ASSERT_EQUAL_UINT16(300, core::routeWake(WakeSource::RtcAlarm, context).next_tick_seconds);

  context.battery = BatteryLevel::Critical;
  TEST_ASSERT_EQUAL_UINT16(300, core::routeWake(WakeSource::RtcAlarm, context).next_tick_seconds);
}

void test_safe_mode_drops_everything_optional(void) {
  WakeContext context = freshContext();
  context.mode = RunMode::Safe;
  context.accel_features_enabled = true;
  context.minutes_since_battery_sample = 1000;  // would otherwise be due

  const WakePlan plan = core::routeWake(WakeSource::Button, context);
  TEST_ASSERT_FALSE(plan.need_accel);
  TEST_ASSERT_FALSE(plan.need_battery);
  TEST_ASSERT_FALSE(plan.run_ui);
  // Still a watch: clock and face survive.
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_EQUAL_UINT16(300, plan.next_tick_seconds);
}

void test_recovery_mode_is_nearly_inert(void) {
  // The anti-crash-loop state. Read the clock, arm the next alarm, leave the
  // panel alone, sleep for a quarter of an hour.
  WakeContext context = freshContext();
  context.mode = RunMode::Recovery;
  context.accel_features_enabled = true;
  context.minutes_since_battery_sample = 1000;

  const WakePlan plan = core::routeWake(WakeSource::RtcAlarm, context);
  TEST_ASSERT_TRUE(plan.need_rtc);
  TEST_ASSERT_FALSE(plan.need_display);
  TEST_ASSERT_FALSE(plan.need_accel);
  TEST_ASSERT_FALSE(plan.need_battery);
  TEST_ASSERT_FALSE(plan.run_ui);
  TEST_ASSERT_EQUAL_UINT16(core::kRecoveryTickSeconds, plan.next_tick_seconds);
}

void test_recovery_mode_still_draws_on_power_on(void) {
  // Otherwise a watch stuck in Recovery would show a blank screen forever and
  // look dead rather than degraded.
  WakeContext context = freshContext();
  context.mode = RunMode::Recovery;

  const WakePlan plan = core::routeWake(WakeSource::PowerOn, context);
  TEST_ASSERT_TRUE(plan.need_display);
  TEST_ASSERT_TRUE(plan.force_full_refresh);
}

void test_recovery_mode_overrides_every_wake_source(void) {
  WakeContext context = freshContext();
  context.mode = RunMode::Recovery;
  context.accel_features_enabled = true;
  context.ui_active = true;

  const WakeSource sources[] = {WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  for (const WakeSource source : sources) {
    const WakePlan plan = core::routeWake(source, context);
    TEST_ASSERT_FALSE(plan.need_accel);
    TEST_ASSERT_FALSE(plan.need_battery);
    TEST_ASSERT_FALSE(plan.run_ui);
    TEST_ASSERT_FALSE(plan.need_display);
    TEST_ASSERT_EQUAL_UINT16(core::kRecoveryTickSeconds, plan.next_tick_seconds);
  }
}

void test_tick_interval_precedence(void) {
  // Recovery outranks a low battery, which outranks normal.
  WakeContext context = freshContext();

  context.mode = RunMode::Normal;
  context.battery = BatteryLevel::Normal;
  TEST_ASSERT_EQUAL_UINT16(60, core::tickIntervalFor(context));

  context.battery = BatteryLevel::Low;
  TEST_ASSERT_EQUAL_UINT16(300, core::tickIntervalFor(context));

  context.mode = RunMode::Safe;
  context.battery = BatteryLevel::Full;
  TEST_ASSERT_EQUAL_UINT16(300, core::tickIntervalFor(context));

  context.mode = RunMode::Recovery;
  TEST_ASSERT_EQUAL_UINT16(core::kRecoveryTickSeconds, core::tickIntervalFor(context));
}

// ── the radio: which wakes may carry a sync window ───────────────────────────

void test_no_wake_carries_a_sync_window_before_the_hour(void) {
  // Law 1: never on a routine wake. The overwhelmingly common case is 1440 ticks a
  // day and 1416 of them must not touch the radio.
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  WakeContext context = freshContext();
  context.minutes_since_sync_window =
      static_cast<uint16_t>(core::kSyncWindowIntervalMinutes - 1);

  for (const WakeSource source : sources) {
    TEST_ASSERT_FALSE(core::routeWake(source, context).need_ble);
  }
}

void test_a_due_window_rides_on_the_tick_it_was_budgeted_to_extend(void) {
  // PROTOCOL.md §5.3 budgets the window as an extension of a wake the watch was
  // already taking, not as a wake of its own.
  const WakeSource carriers[] = {WakeSource::RtcAlarm, WakeSource::Timer, WakeSource::PowerOn,
                                 WakeSource::Button};
  for (const WakeSource source : carriers) {
    TEST_ASSERT_TRUE(core::routeWake(source, syncDueContext()).need_ble);
  }
}

void test_motion_never_carries_a_sync_window(void) {
  // An accelerometer wake is deliberately the cheapest path in this router — no
  // clock, no panel. A wrist flick must not be able to make it the most expensive
  // one, and not even a request riding along with it changes that.
  WakeContext context = syncDueContext();
  context.accel_features_enabled = true;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Accelerometer, context).need_ble);

  context.sync_requested = true;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Accelerometer, context).need_ble);
}

void test_an_unclassified_wake_never_carries_a_sync_window(void) {
  // Unknown means the previous run ended in a way board::power could not classify —
  // a panic, a brownout, an unexplained reset — and core::health has not yet had
  // the three consecutive faults it needs to escalate into Safe mode. Refusing
  // costs nothing: the next ordinary tick carries the window instead.
  WakeContext context = syncDueContext();
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Unknown, context).need_ble);

  context.sync_requested = true;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Unknown, context).need_ble);
}

void test_a_button_press_carries_a_user_requested_window(void) {
  // The Sync menu item. §5.1: it opens a window immediately, without waiting for
  // the hour.
  WakeContext context = freshContext();
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Button, context).need_ble);

  context.sync_requested = true;
  TEST_ASSERT_TRUE(core::routeWake(WakeSource::Button, context).need_ble);
}

void test_a_cold_boot_may_sync_because_its_clock_is_unset(void) {
  // The persisted block is re-initialised on a cold boot, which leaves the sync
  // timer reading "due" — this is the wake that can set a brand new watch's clock.
  WakeContext context = freshContext();
  context.minutes_since_sync_window = core::SyncState{}.minutes_since_window;
  TEST_ASSERT_TRUE(core::routeWake(WakeSource::PowerOn, context).need_ble);
}

void test_degraded_modes_never_carry_a_sync_window(void) {
  // No radio in Safe or Recovery, whatever the schedule says and whoever asked.
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  const RunMode gated[] = {RunMode::Safe, RunMode::Recovery};

  for (const RunMode mode : gated) {
    for (const WakeSource source : sources) {
      WakeContext context = syncDueContext();
      context.mode = mode;
      context.sync_requested = true;
      TEST_ASSERT_FALSE(core::routeWake(source, context).need_ble);
    }
  }
}

void test_a_low_battery_never_carries_a_sync_window(void) {
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  const BatteryLevel gated[] = {BatteryLevel::Low, BatteryLevel::Critical};

  for (const BatteryLevel level : gated) {
    for (const WakeSource source : sources) {
      WakeContext context = syncDueContext();
      context.battery = level;
      context.sync_requested = true;
      TEST_ASSERT_FALSE(core::routeWake(source, context).need_ble);
    }
  }
}

void test_a_granted_window_never_costs_a_peripheral_of_its_own(void) {
  // §5.3's budget assumes the window extends a wake that was already happening. If
  // granting one also switched on the ADC or the accelerometer, the ledger's
  // ~0.031 mAh per window would be wrong.
  const WakePlan without = core::routeWake(WakeSource::RtcAlarm, freshContext());
  const WakePlan with = core::routeWake(WakeSource::RtcAlarm, syncDueContext());

  TEST_ASSERT_TRUE(with.need_ble);
  TEST_ASSERT_EQUAL_INT(without.need_rtc, with.need_rtc);
  TEST_ASSERT_EQUAL_INT(without.need_accel, with.need_accel);
  TEST_ASSERT_EQUAL_INT(without.need_battery, with.need_battery);
  TEST_ASSERT_EQUAL_INT(without.need_display, with.need_display);
  TEST_ASSERT_EQUAL_INT(without.need_i2c, with.need_i2c);
  TEST_ASSERT_EQUAL_UINT16(without.next_tick_seconds, with.next_tick_seconds);
}

void test_ble_is_granted_only_when_every_rule_agrees(void) {
  // The radio is the largest consumer on the board, so this asserts the properties
  // a granted window must have rather than re-deriving the answer: a future edit
  // that loosens a gate fails here even if it also updates the test above it.
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
  const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal, BatteryLevel::Low,
                                 BatteryLevel::Critical};
  const uint16_t elapsed_minutes[] = {
      0, static_cast<uint16_t>(core::kSyncWindowIntervalMinutes - 1),
      core::kSyncWindowIntervalMinutes, UINT16_MAX};

  bool granted_at_least_once = false;

  for (const WakeSource source : sources) {
    for (const RunMode mode : modes) {
      for (const BatteryLevel level : levels) {
        for (const uint16_t elapsed : elapsed_minutes) {
          for (int requested = 0; requested < 2; ++requested) {
            WakeContext context = freshContext();
            context.mode = mode;
            context.battery = level;
            context.minutes_since_sync_window = elapsed;
            context.sync_requested = (requested != 0);

            const WakePlan plan = core::routeWake(source, context);
            if (!plan.need_ble) {
              continue;
            }
            granted_at_least_once = true;

            TEST_ASSERT_EQUAL_INT(static_cast<int>(RunMode::Normal), static_cast<int>(mode));
            TEST_ASSERT_TRUE(core::radioPermitted(level));
            TEST_ASSERT_TRUE(context.sync_requested ||
                             elapsed >= core::kSyncWindowIntervalMinutes);
            TEST_ASSERT_TRUE(source != WakeSource::Accelerometer);
            TEST_ASSERT_TRUE(source != WakeSource::Unknown);
          }
        }
      }
    }
  }

  TEST_ASSERT_TRUE(granted_at_least_once);
}

// ── the invariant that keeps the I2C bus honest ───────────────────────────────

void test_i2c_is_powered_exactly_when_a_bus_device_is_needed(void) {
  // If this drifts, either the bus is powered for nothing (wasted energy) or a
  // device is accessed on a dead bus (a timeout, and a wasted wake).
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
  const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal, BatteryLevel::Low,
                                 BatteryLevel::Critical};

  for (const WakeSource source : sources) {
    for (const RunMode mode : modes) {
      for (const BatteryLevel level : levels) {
        for (int accel = 0; accel < 2; ++accel) {
          for (int ui = 0; ui < 2; ++ui) {
            WakeContext context = freshContext();
            context.mode = mode;
            context.battery = level;
            context.accel_features_enabled = (accel != 0);
            context.ui_active = (ui != 0);

            const WakePlan plan = core::routeWake(source, context);
            TEST_ASSERT_EQUAL_INT(plan.need_rtc || plan.need_accel, plan.need_i2c);
          }
        }
      }
    }
  }
}

void test_a_tick_is_never_scheduled_at_zero(void) {
  // A zero interval means "wake immediately", which never reaches deep sleep.
  const WakeSource sources[] = {WakeSource::PowerOn, WakeSource::RtcAlarm, WakeSource::Button,
                                WakeSource::Accelerometer, WakeSource::Timer,
                                WakeSource::Unknown};
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};

  for (const WakeSource source : sources) {
    for (const RunMode mode : modes) {
      WakeContext context = freshContext();
      context.mode = mode;
      const WakePlan plan = core::routeWake(source, context);
      TEST_ASSERT_GREATER_THAN_UINT16(0, plan.next_tick_seconds);
    }
  }
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_minute_tick_is_minimal);
  RUN_TEST(test_battery_is_sampled_on_a_schedule_not_every_tick);
  RUN_TEST(test_accelerometer_is_opt_in);

  RUN_TEST(test_power_on_establishes_a_baseline);
  RUN_TEST(test_button_wake_is_cheap);
  RUN_TEST(test_accelerometer_wake_does_not_touch_the_panel);
  RUN_TEST(test_accelerometer_wake_updates_a_live_screen);
  RUN_TEST(test_timer_backstop_behaves_like_a_tick);
  RUN_TEST(test_unknown_wake_resynchronises_the_panel);
  RUN_TEST(test_every_tick_like_wake_collects_the_step_counter);
  RUN_TEST(test_the_step_counter_is_still_opt_in_on_every_wake_source);

  RUN_TEST(test_low_battery_stretches_the_tick);
  RUN_TEST(test_safe_mode_drops_everything_optional);
  RUN_TEST(test_recovery_mode_is_nearly_inert);
  RUN_TEST(test_recovery_mode_still_draws_on_power_on);
  RUN_TEST(test_recovery_mode_overrides_every_wake_source);
  RUN_TEST(test_tick_interval_precedence);

  RUN_TEST(test_no_wake_carries_a_sync_window_before_the_hour);
  RUN_TEST(test_a_due_window_rides_on_the_tick_it_was_budgeted_to_extend);
  RUN_TEST(test_motion_never_carries_a_sync_window);
  RUN_TEST(test_an_unclassified_wake_never_carries_a_sync_window);
  RUN_TEST(test_a_button_press_carries_a_user_requested_window);
  RUN_TEST(test_a_cold_boot_may_sync_because_its_clock_is_unset);
  RUN_TEST(test_degraded_modes_never_carry_a_sync_window);
  RUN_TEST(test_a_low_battery_never_carries_a_sync_window);
  RUN_TEST(test_a_granted_window_never_costs_a_peripheral_of_its_own);
  RUN_TEST(test_ble_is_granted_only_when_every_rule_agrees);

  RUN_TEST(test_i2c_is_powered_exactly_when_a_bus_device_is_needed);
  RUN_TEST(test_a_tick_is_never_scheduled_at_zero);

  return UNITY_END();
}
