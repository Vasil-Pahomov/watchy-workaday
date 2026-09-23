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

// A context where the battery sample is NOT due, so tests that care about other
// peripherals are not confused by an incidental ADC power-up.
static WakeContext freshContext(void) {
  WakeContext context;
  context.mode = RunMode::Normal;
  context.battery = BatteryLevel::Normal;
  context.minutes_since_battery_sample = 0;
  context.accel_features_enabled = false;
  context.ui_active = false;
  return context;
}

// Every wake source, for the sweeps below.
static const WakeSource kAllSources[] = {WakeSource::PowerOn,        WakeSource::RtcAlarm,
                                         WakeSource::Button,         WakeSource::Accelerometer,
                                         WakeSource::Timer,          WakeSource::Unknown};

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

// ── the radio: which wakes may carry a sync window ────────────────────────

// This router answers eligibility and nothing else: is this the KIND of wake
// PROTOCOL.md §5.3 budgets a window onto. Whether a window is actually due is
// core::evaluateSyncWindow()'s answer and test_sync_policy's subject — including
// the hour boundary, which needs a wall clock this router runs too early to have.

void test_a_wake_the_watch_was_taking_anyway_may_carry_a_window(void) {
  // §5.3 prices the window as an extension of a wake already happening, not as a
  // wake of its own. These four are the wakes that happen anyway.
  const WakeSource carriers[] = {WakeSource::RtcAlarm, WakeSource::Timer, WakeSource::PowerOn,
                                 WakeSource::Button};
  for (const WakeSource source : carriers) {
    TEST_ASSERT_TRUE(core::routeWake(source, freshContext()).may_carry_sync_window);
  }
}

void test_motion_never_carries_a_sync_window(void) {
  // An accelerometer wake is deliberately the cheapest path in this router — no
  // clock, no panel. A wrist flick must not be able to make it the most expensive
  // one, and it arrives at a rate nobody budgeted a radio against.
  WakeContext context = freshContext();
  context.accel_features_enabled = true;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Accelerometer, context).may_carry_sync_window);

  context.ui_active = true;
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Accelerometer, context).may_carry_sync_window);
}

void test_an_unclassified_wake_never_carries_a_sync_window(void) {
  // Unknown means the previous run ended in a way board::power could not classify —
  // a panic, a brownout, an unexplained reset — and core::health has not yet had
  // the three consecutive faults it needs to escalate into Safe mode. Refusing
  // costs nothing: the next ordinary tick carries the window instead.
  TEST_ASSERT_FALSE(core::routeWake(WakeSource::Unknown, freshContext()).may_carry_sync_window);
}

void test_degraded_modes_never_carry_a_sync_window(void) {
  // No radio in Safe or Recovery, whatever the source. core::evaluateSyncWindow()
  // refuses both modes as well — the radio is the one peripheral worth gating
  // twice, and this is the half of that pair which lives here.
  const RunMode gated[] = {RunMode::Safe, RunMode::Recovery};

  for (const RunMode mode : gated) {
    for (const WakeSource source : kAllSources) {
      WakeContext context = freshContext();
      context.mode = mode;
      TEST_ASSERT_FALSE(core::routeWake(source, context).may_carry_sync_window);
    }
  }
}

void test_eligibility_depends_on_the_source_and_the_mode_and_nothing_else(void) {
  // The property that keeps the split honest. This flag used to be the full §5.1
  // grant, evaluated here from top-of-wake state; it is now eligibility alone, and
  // an edit that quietly reintroduces a schedule input — the battery, a request,
  // anything — fails here rather than being discovered as a window that opened on
  // a stale reading.
  for (const WakeSource source : kAllSources) {
    const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
    for (const RunMode mode : modes) {
      WakeContext baseline = freshContext();
      baseline.mode = mode;
      const bool expected = core::routeWake(source, baseline).may_carry_sync_window;

      const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal,
                                     BatteryLevel::Low, BatteryLevel::Critical};
      for (const BatteryLevel level : levels) {
        for (int accel = 0; accel < 2; ++accel) {
          for (int ui = 0; ui < 2; ++ui) {
            for (uint32_t sample = 0; sample < 2; ++sample) {
              WakeContext context = freshContext();
              context.mode = mode;
              context.battery = level;
              context.accel_features_enabled = (accel != 0);
              context.ui_active = (ui != 0);
              context.minutes_since_battery_sample =
                  sample * core::kBatterySampleIntervalMinutes;
              TEST_ASSERT_EQUAL_INT(
                  expected, core::routeWake(source, context).may_carry_sync_window);
            }
          }
        }
      }
    }
  }
}

void test_eligibility_never_costs_a_peripheral_of_its_own(void) {
  // §5.3's budget assumes the window extends a wake that was already happening, so
  // a wake that may carry one has to look exactly like the same wake in a build
  // with no radio at all: no extra ADC, no extra sensor.
  //
  // PowerOn is excluded and it is not an exception to the rule — it powers up the
  // ADC and the sensor because a cold boot has no baseline, not because a window
  // may ride on it, and it happens once against the 24 windows a day that ride on
  // the tick. The three below are the ones the ledger is about.
  const WakeSource carriers[] = {WakeSource::RtcAlarm, WakeSource::Timer, WakeSource::Button};
  for (const WakeSource source : carriers) {
    const WakePlan plan = core::routeWake(source, freshContext());
    TEST_ASSERT_TRUE(plan.may_carry_sync_window);
    TEST_ASSERT_EQUAL_INT(plan.need_rtc || plan.need_accel, plan.need_i2c);
    TEST_ASSERT_FALSE(plan.need_battery);  // not due in freshContext()
    TEST_ASSERT_FALSE(plan.need_accel);    // motion features off
  }
}

// ── the invariant that keeps the I2C bus honest ───────────────────────────────

void test_i2c_is_powered_exactly_when_a_bus_device_is_needed(void) {
  // If this drifts, either the bus is powered for nothing (wasted energy) or a
  // device is accessed on a dead bus (a timeout, and a wasted wake).
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};
  const BatteryLevel levels[] = {BatteryLevel::Full, BatteryLevel::Normal, BatteryLevel::Low,
                                 BatteryLevel::Critical};

  for (const WakeSource source : kAllSources) {
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
  const RunMode modes[] = {RunMode::Normal, RunMode::Safe, RunMode::Recovery};

  for (const WakeSource source : kAllSources) {
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

  RUN_TEST(test_a_wake_the_watch_was_taking_anyway_may_carry_a_window);
  RUN_TEST(test_motion_never_carries_a_sync_window);
  RUN_TEST(test_an_unclassified_wake_never_carries_a_sync_window);
  RUN_TEST(test_degraded_modes_never_carry_a_sync_window);
  RUN_TEST(test_eligibility_depends_on_the_source_and_the_mode_and_nothing_else);
  RUN_TEST(test_eligibility_never_costs_a_peripheral_of_its_own);

  RUN_TEST(test_i2c_is_powered_exactly_when_a_bus_device_is_needed);
  RUN_TEST(test_a_tick_is_never_scheduled_at_zero);

  return UNITY_END();
}
