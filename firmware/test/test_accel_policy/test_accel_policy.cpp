#include <unity.h>

#include "core/accel_policy.h"

using core::AccelAction;
using core::AccelProbe;
using core::AccelState;

void setUp(void) {}
void tearDown(void) {}

// What one wake did to the sensor. Every field is an I2C cost the watch paid.
struct Traffic {
  int probes = 0;
  int uploads = 0;
  int suspends = 0;
  int reads = 0;
};

// One wake's worth of the state machine, with the sensor's behaviour supplied by
// the caller.
//
// This mirrors main.cpp exactly — begin, maybe probe, maybe configure, maybe
// park, maybe read — so the tests below exercise the same sequence the firmware
// runs, and `Traffic` counts the things that actually cost energy.
static void wake(AccelState& state, AccelProbe probe, bool configure_succeeds,
                 bool suspend_succeeds, Traffic& traffic) {
  AccelAction action = core::accelBeginWake(state);

  if (action == AccelAction::Probe) {
    ++traffic.probes;
    action = core::accelPlan(state, probe);
  }
  if (action == AccelAction::Configure) {
    ++traffic.uploads;
    action = core::accelAfterConfigure(state, configure_succeeds);
  }
  if (action == AccelAction::Suspend) {
    ++traffic.suspends;
    core::accelAfterSuspend(state, suspend_succeeds);
  }
  if (action == AccelAction::Read) {
    ++traffic.reads;
  }
}

// The common case: the park works when it is asked for.
static bool wake(AccelState& state, AccelProbe probe, bool configure_succeeds, Traffic& traffic) {
  const int before = traffic.reads;
  wake(state, probe, configure_succeeds, /*suspend_succeeds=*/true, traffic);
  return traffic.reads != before;
}

// ── the steady state: nothing expensive ever happens ─────────────────────────

void test_a_configured_sensor_is_just_read(void) {
  AccelState state;
  Traffic traffic;
  TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));
  TEST_ASSERT_EQUAL_INT(0, traffic.uploads);
  TEST_ASSERT_EQUAL_UINT8(0, state.config_attempts);
  TEST_ASSERT_FALSE(state.gave_up);
}

void test_a_thousand_healthy_wakes_upload_nothing(void) {
  // The steady state is 1440 of these a day. One stray upload in here is ~0.85 s
  // of I2C, and 1440 of them is the whole battery budget.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 1000; ++i) {
    TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));
  }
  TEST_ASSERT_EQUAL_INT(0, traffic.uploads);
  TEST_ASSERT_EQUAL_INT(0, traffic.suspends);
}

void test_first_boot_configures_once_then_settles(void) {
  // The intended lifetime of this feature: one upload, ever.
  AccelState state;
  Traffic traffic;

  TEST_ASSERT_TRUE(wake(state, AccelProbe::Unconfigured, true, traffic));
  TEST_ASSERT_EQUAL_INT(1, traffic.uploads);
  TEST_ASSERT_FALSE(state.config_incomplete);

  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));
  }
  TEST_ASSERT_EQUAL_INT(1, traffic.uploads);
  TEST_ASSERT_EQUAL_UINT8(0, state.config_attempts);  // refunded by a healthy wake
}

// ── the attempt limit ────────────────────────────────────────────────────────

void test_a_sensor_that_never_configures_is_given_up_on(void) {
  // The P1 failure: probe says Unconfigured for ever, so without a limit the 6 KB
  // upload runs every minute for the life of the device.
  AccelState state;
  Traffic traffic;

  for (int i = 0; i < 5000; ++i) {
    TEST_ASSERT_FALSE(wake(state, AccelProbe::Unconfigured, false, traffic));
  }
  TEST_ASSERT_EQUAL_INT(core::kMaxAccelConfigAttempts, traffic.uploads);
  TEST_ASSERT_TRUE(state.gave_up);
}

void test_the_limit_boundary_is_exact(void) {
  AccelState state;
  Traffic traffic;

  for (uint8_t attempt = 1; attempt <= core::kMaxAccelConfigAttempts; ++attempt) {
    wake(state, AccelProbe::Unconfigured, false, traffic);
    TEST_ASSERT_EQUAL_INT(attempt, traffic.uploads);
    TEST_ASSERT_EQUAL_UINT8(attempt, state.config_attempts);
  }
  // The last attempt used the budget up, so the give-up latches on the way out of
  // that same wake rather than costing one more upload to discover.
  TEST_ASSERT_TRUE(state.gave_up);

  wake(state, AccelProbe::Unconfigured, false, traffic);
  TEST_ASSERT_EQUAL_INT(core::kMaxAccelConfigAttempts, traffic.uploads);
}

void test_giving_up_stops_the_probe_too(void) {
  AccelState state;
  state.gave_up = true;
  // Nothing owed, so nothing to do: not even the cheap status reads.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Skip),
                        static_cast<int>(core::accelBeginWake(state)));
  // And if the plan is called anyway, it still refuses.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Skip),
                        static_cast<int>(core::accelPlan(state, AccelProbe::Unconfigured)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Skip),
                        static_cast<int>(core::accelPlan(state, AccelProbe::Ready)));
}

void test_an_attempt_that_never_returns_is_still_counted(void) {
  // The watchdog case: the upload wedges the bus, the wake dies partway through
  // and accelAfterConfigure() is never reached. The attempt must already be on
  // record, or the reset cycle repeats for ever at ~10 s of full current a go.
  AccelState state;
  for (uint8_t attempt = 1; attempt <= core::kMaxAccelConfigAttempts; ++attempt) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Probe),
                          static_cast<int>(core::accelBeginWake(state)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Configure),
                          static_cast<int>(core::accelPlan(state, AccelProbe::Unconfigured)));
    TEST_ASSERT_EQUAL_UINT8(attempt, state.config_attempts);
    TEST_ASSERT_TRUE(state.config_incomplete);
    // ... reset here. RTC-backed state is what survives, and it has the count.
  }
  // The fourth wake gives up instead of uploading again — and asks for the park.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Suspend),
                        static_cast<int>(core::accelPlan(state, AccelProbe::Unconfigured)));
  TEST_ASSERT_TRUE(state.gave_up);
}

void test_a_corrupt_attempt_count_still_gives_up(void) {
  // config_attempts lives in RTC memory, which survives reflashing and is not
  // zeroed. A value past the limit must latch the give-up, not wrap the counter.
  AccelState state;
  state.config_attempts = 255;
  Traffic traffic;

  TEST_ASSERT_FALSE(wake(state, AccelProbe::Unconfigured, true, traffic));
  TEST_ASSERT_EQUAL_INT(0, traffic.uploads);
  TEST_ASSERT_EQUAL_UINT8(255, state.config_attempts);  // no wrap
  TEST_ASSERT_TRUE(state.gave_up);
}

// ── a configuration that only half worked ────────────────────────────────────

void test_a_ready_verdict_is_not_trusted_after_a_failed_attempt(void) {
  // The P2 failure. begin() sets the chip's "initialised" bit, then a later step
  // of configure() fails and leaves nothing counting. From the next wake on the
  // bit alone would say Ready and the watch would read a constant zero for ever.
  AccelState state;
  Traffic traffic;

  TEST_ASSERT_FALSE(wake(state, AccelProbe::Unconfigured, false, traffic));
  TEST_ASSERT_TRUE(state.config_incomplete);

  // Next wake: the chip claims to be ready. It is not, and we know it.
  TEST_ASSERT_FALSE(wake(state, AccelProbe::Ready, false, traffic));
  TEST_ASSERT_EQUAL_INT(2, traffic.uploads);
}

void test_a_half_configured_sensor_recovers_when_the_retry_works(void) {
  AccelState state;
  Traffic traffic;

  wake(state, AccelProbe::Unconfigured, false, traffic);  // begin() took, the rest did not
  TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));  // retried, worked
  TEST_ASSERT_EQUAL_INT(2, traffic.uploads);
  TEST_ASSERT_FALSE(state.config_incomplete);

  TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));
  TEST_ASSERT_EQUAL_INT(2, traffic.uploads);  // and it stays quiet
  TEST_ASSERT_EQUAL_UINT8(0, state.config_attempts);
}

void test_an_idle_sensor_is_reconfigured(void) {
  // Configured but not running: the config stream is loaded and the accelerometer
  // is switched off, so the step counter is frozen. Reading it would return a
  // plausible, permanently stale number.
  AccelState state;
  Traffic traffic;
  TEST_ASSERT_TRUE(wake(state, AccelProbe::Idle, true, traffic));
  TEST_ASSERT_EQUAL_INT(1, traffic.uploads);
}

void test_a_configure_that_reports_success_but_does_not_stick_is_bounded(void) {
  // The nastiest variant: configure() returns true every time and the chip is
  // Unconfigured again on the next wake. If a successful attempt refunded itself,
  // this would upload 6 KB a minute for ever while looking healthy.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 500; ++i) {
    wake(state, AccelProbe::Unconfigured, true, traffic);
  }
  TEST_ASSERT_EQUAL_INT(core::kMaxAccelConfigAttempts, traffic.uploads);
  TEST_ASSERT_TRUE(state.gave_up);
}

// ── parking the sensor after giving up ───────────────────────────────────────

void test_giving_up_parks_the_sensor_exactly_once(void) {
  // A BMA423 left enabled but never read costs ~14 uA — a fifth of the sleep
  // floor — for a feature that is off.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 1000; ++i) {
    wake(state, AccelProbe::Unconfigured, false, traffic);
  }
  TEST_ASSERT_EQUAL_INT(1, traffic.suspends);
  TEST_ASSERT_FALSE(state.suspend_pending);
}

void test_a_failed_park_is_retried_on_a_later_wake(void) {
  // The write can NACK. Discarding that result would leave the sensor drawing
  // current for the rest of the charge with nothing left to notice.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < core::kMaxAccelConfigAttempts; ++i) {
    wake(state, AccelProbe::Unconfigured, false, /*suspend_succeeds=*/false, traffic);
  }
  TEST_ASSERT_TRUE(state.gave_up);
  TEST_ASSERT_TRUE(state.suspend_pending);
  TEST_ASSERT_EQUAL_INT(1, traffic.suspends);

  // A later wake tries again — without probing, because there is nothing left to
  // probe for.
  const int probes_before = traffic.probes;
  wake(state, AccelProbe::Unconfigured, false, /*suspend_succeeds=*/true, traffic);
  TEST_ASSERT_EQUAL_INT(2, traffic.suspends);
  TEST_ASSERT_EQUAL_INT(probes_before, traffic.probes);
  TEST_ASSERT_FALSE(state.suspend_pending);
}

void test_park_retries_are_bounded(void) {
  // Two transactions a wake, for ever, is not a rounding error: it is ~1 mAh/day
  // on a wedged bus. The retry has to stop.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 1000; ++i) {
    wake(state, AccelProbe::Unconfigured, false, /*suspend_succeeds=*/false, traffic);
  }
  TEST_ASSERT_EQUAL_INT(core::kMaxAccelSuspendAttempts, traffic.suspends);
  TEST_ASSERT_FALSE(state.suspend_pending);
}

void test_a_park_that_never_returns_is_still_counted(void) {
  // Same watchdog argument as the upload: the attempt is spent when it is issued,
  // not when it reports back.
  AccelState state;
  state.gave_up = true;
  state.suspend_pending = true;

  for (uint8_t attempt = 1; attempt <= core::kMaxAccelSuspendAttempts; ++attempt) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Suspend),
                          static_cast<int>(core::accelBeginWake(state)));
    TEST_ASSERT_EQUAL_UINT8(attempt, state.suspend_attempts);
    // ... reset here, before accelAfterSuspend() is ever reached.
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Skip),
                        static_cast<int>(core::accelBeginWake(state)));
}

void test_a_parked_sensor_costs_nothing_on_every_later_wake(void) {
  // The whole point of the latch: after this settles, a dead sensor must not cost
  // a single transaction a minute for the rest of the charge.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 10; ++i) {
    wake(state, AccelProbe::Unconfigured, false, traffic);
  }
  const Traffic settled = traffic;

  for (int i = 0; i < 5000; ++i) {
    wake(state, AccelProbe::Unconfigured, false, traffic);
  }
  TEST_ASSERT_EQUAL_INT(settled.probes, traffic.probes);
  TEST_ASSERT_EQUAL_INT(settled.uploads, traffic.uploads);
  TEST_ASSERT_EQUAL_INT(settled.suspends, traffic.suspends);
  TEST_ASSERT_EQUAL_INT(settled.reads, traffic.reads);
}

void test_a_corrupt_park_count_does_not_wrap(void) {
  AccelState state;
  state.gave_up = true;
  state.suspend_pending = true;
  state.suspend_attempts = 255;

  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Skip),
                        static_cast<int>(core::accelBeginWake(state)));
  TEST_ASSERT_EQUAL_UINT8(255, state.suspend_attempts);
  TEST_ASSERT_FALSE(state.suspend_pending);
}

// ── an absent sensor ─────────────────────────────────────────────────────────

void test_an_absent_sensor_is_never_configured(void) {
  // No answer on the bus. Uploading into a device that will not acknowledge
  // cannot help, so it must not spend the attempt budget either.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 1000; ++i) {
    TEST_ASSERT_FALSE(wake(state, AccelProbe::Absent, true, traffic));
  }
  TEST_ASSERT_EQUAL_INT(0, traffic.uploads);
  TEST_ASSERT_EQUAL_INT(0, traffic.suspends);
  TEST_ASSERT_EQUAL_UINT8(0, state.config_attempts);
  TEST_ASSERT_FALSE(state.gave_up);
}

void test_a_sensor_that_reappears_is_used_again(void) {
  // Because an absent sensor never burned the budget, a bus that recovers costs
  // one upload, not a dead feature.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 100; ++i) {
    wake(state, AccelProbe::Absent, true, traffic);
  }
  TEST_ASSERT_TRUE(wake(state, AccelProbe::Unconfigured, true, traffic));
  TEST_ASSERT_EQUAL_INT(1, traffic.uploads);
}

// ── housekeeping ─────────────────────────────────────────────────────────────

void test_reset_clears_the_latch(void) {
  // What a power cycle or a persist-version bump does: the sensor gets a fresh
  // budget, which is the only way back from a give-up.
  AccelState state;
  Traffic traffic;
  for (int i = 0; i < 100; ++i) {
    wake(state, AccelProbe::Unconfigured, false, traffic);
  }
  TEST_ASSERT_TRUE(state.gave_up);

  core::resetAccelState(state);
  TEST_ASSERT_FALSE(state.gave_up);
  TEST_ASSERT_FALSE(state.config_incomplete);
  TEST_ASSERT_FALSE(state.suspend_pending);
  TEST_ASSERT_EQUAL_UINT8(0, state.config_attempts);
  TEST_ASSERT_EQUAL_UINT8(0, state.suspend_attempts);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AccelAction::Probe),
                        static_cast<int>(core::accelBeginWake(state)));
}

void test_intermittent_failures_do_not_accumulate_forever(void) {
  // A sensor that drops its configuration once in a blue moon must not creep
  // toward the give-up over months: a wake that starts Ready refunds the budget.
  AccelState state;
  Traffic traffic;
  for (int cycle = 0; cycle < 20; ++cycle) {
    wake(state, AccelProbe::Unconfigured, true, traffic);
    for (int i = 0; i < 50; ++i) {
      TEST_ASSERT_TRUE(wake(state, AccelProbe::Ready, true, traffic));
    }
  }
  TEST_ASSERT_EQUAL_INT(20, traffic.uploads);
  TEST_ASSERT_FALSE(state.gave_up);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_a_configured_sensor_is_just_read);
  RUN_TEST(test_a_thousand_healthy_wakes_upload_nothing);
  RUN_TEST(test_first_boot_configures_once_then_settles);

  RUN_TEST(test_a_sensor_that_never_configures_is_given_up_on);
  RUN_TEST(test_the_limit_boundary_is_exact);
  RUN_TEST(test_giving_up_stops_the_probe_too);
  RUN_TEST(test_an_attempt_that_never_returns_is_still_counted);
  RUN_TEST(test_a_corrupt_attempt_count_still_gives_up);

  RUN_TEST(test_a_ready_verdict_is_not_trusted_after_a_failed_attempt);
  RUN_TEST(test_a_half_configured_sensor_recovers_when_the_retry_works);
  RUN_TEST(test_an_idle_sensor_is_reconfigured);
  RUN_TEST(test_a_configure_that_reports_success_but_does_not_stick_is_bounded);

  RUN_TEST(test_giving_up_parks_the_sensor_exactly_once);
  RUN_TEST(test_a_failed_park_is_retried_on_a_later_wake);
  RUN_TEST(test_park_retries_are_bounded);
  RUN_TEST(test_a_park_that_never_returns_is_still_counted);
  RUN_TEST(test_a_parked_sensor_costs_nothing_on_every_later_wake);
  RUN_TEST(test_a_corrupt_park_count_does_not_wrap);

  RUN_TEST(test_an_absent_sensor_is_never_configured);
  RUN_TEST(test_a_sensor_that_reappears_is_used_again);

  RUN_TEST(test_reset_clears_the_latch);
  RUN_TEST(test_intermittent_failures_do_not_accumulate_forever);

  return UNITY_END();
}
