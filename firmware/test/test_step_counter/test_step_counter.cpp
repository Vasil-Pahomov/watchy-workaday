#include <unity.h>

#include "core/step_counter.h"

using core::DateTime;
using core::StepState;
using core::StepUpdate;

void setUp(void) {}
void tearDown(void) {}

static const DateTime kNoon{2026, 8, 12, 12, 0, 0};

static StepUpdate feed(StepState& state, uint32_t raw, const DateTime& when, bool time_valid,
                       uint32_t elapsed = 1) {
  return core::updateSteps(state, raw, when, time_valid, elapsed);
}

// Primed on a known day, as after one wake has already happened.
static StepState primedOn(uint32_t raw, const DateTime& when) {
  StepState state;
  feed(state, raw, when, true);
  return state;
}

static StepState primedAt(uint32_t raw) { return primedOn(raw, kNoon); }

// Primed with no trustworthy clock, so the day is still unknown.
static StepState primedWithoutClock(uint32_t raw) {
  StepState state;
  feed(state, raw, DateTime{}, false);
  return state;
}

// ── the plausibility limit ───────────────────────────────────────────────────

void test_limit_scales_with_the_interval(void) {
  TEST_ASSERT_EQUAL_UINT32(core::kMaxStepsPerMinute, core::maxPlausibleDelta(1));
  TEST_ASSERT_EQUAL_UINT32(core::kMaxStepsPerMinute * 5, core::maxPlausibleDelta(5));
  TEST_ASSERT_EQUAL_UINT32(core::kMaxStepsPerMinute * 15, core::maxPlausibleDelta(15));
}

void test_limit_treats_zero_minutes_as_one(void) {
  TEST_ASSERT_EQUAL_UINT32(core::maxPlausibleDelta(1), core::maxPlausibleDelta(0));
}

void test_limit_stops_growing_after_an_hour(void) {
  // Otherwise a long gap would make the cap meaningless and let garbage through.
  const uint32_t ceiling = core::kMaxStepsPerMinute * core::kMaxCreditedGapMinutes;
  TEST_ASSERT_EQUAL_UINT32(ceiling, core::maxPlausibleDelta(core::kMaxCreditedGapMinutes));
  TEST_ASSERT_EQUAL_UINT32(ceiling, core::maxPlausibleDelta(600));
  TEST_ASSERT_EQUAL_UINT32(ceiling, core::maxPlausibleDelta(UINT32_MAX));
}

// ── the baseline ─────────────────────────────────────────────────────────────

void test_first_reading_only_establishes_a_baseline(void) {
  // The chip's total may predate this boot; crediting it would invent steps.
  StepState state;
  const StepUpdate update = feed(state, 4321, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(0, update.added);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
  TEST_ASSERT_TRUE(state.primed);
  TEST_ASSERT_EQUAL_UINT32(4321, state.last_raw);
}

void test_reset_clears_everything(void) {
  StepState state = primedAt(500);
  feed(state, 600, kNoon, true);
  feed(state, UINT32_MAX, kNoon, true);  // leaves a rejection on the record
  core::noteStepWake(state, true, false, 30);  // and a stale stretch
  core::resetStepState(state);
  TEST_ASSERT_FALSE(state.primed);
  TEST_ASSERT_FALSE(state.day_known);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
  TEST_ASSERT_EQUAL_UINT32(0, state.last_raw);
  TEST_ASSERT_EQUAL_UINT8(0, state.rejected_streak);
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_reading);
}

// ── ordinary counting ────────────────────────────────────────────────────────

void test_delta_is_credited(void) {
  StepState state = primedAt(1000);
  const StepUpdate update = feed(state, 1120, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(120, update.added);
  TEST_ASSERT_EQUAL_UINT32(120, state.today);
  TEST_ASSERT_FALSE(update.counter_restarted);
}

void test_deltas_accumulate(void) {
  StepState state = primedAt(0);
  uint32_t raw = 0;
  for (int minute = 0; minute < 60; ++minute) {
    raw += 50;
    feed(state, raw, kNoon, true);
  }
  TEST_ASSERT_EQUAL_UINT32(3000, state.today);
}

void test_a_still_wrist_adds_nothing(void) {
  StepState state = primedAt(777);
  for (int i = 0; i < 20; ++i) {
    const StepUpdate update = feed(state, 777, kNoon, true);
    TEST_ASSERT_EQUAL_UINT32(0, update.added);
  }
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

void test_dead_sensor_reading_zero_forever(void) {
  // An unconfigured sensor reads 0 on every wake. That must stay a flat zero, not
  // look like a restart and be credited over and over.
  StepState state;
  for (int i = 0; i < 50; ++i) {
    feed(state, 0, kNoon, true);
  }
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

// ── the sensor restarting its count ──────────────────────────────────────────

void test_counter_restart_is_detected(void) {
  // The BMA423 zeroes its counter whenever it is reconfigured — after a power
  // glitch, for instance. Treating that as a negative delta would underflow.
  StepState state = primedAt(9000);
  const StepUpdate update = feed(state, 40, kNoon, true);
  TEST_ASSERT_TRUE(update.counter_restarted);
  TEST_ASSERT_EQUAL_UINT32(40, update.added);
  TEST_ASSERT_EQUAL_UINT32(40, state.today);
  TEST_ASSERT_EQUAL_UINT32(40, state.last_raw);
}

void test_restart_to_zero_credits_nothing(void) {
  StepState state = primedAt(9000);
  const StepUpdate update = feed(state, 0, kNoon, true);
  TEST_ASSERT_TRUE(update.counter_restarted);
  TEST_ASSERT_EQUAL_UINT32(0, update.added);
  TEST_ASSERT_EQUAL_UINT32(0, state.last_raw);
}

void test_counting_resumes_normally_after_a_restart(void) {
  StepState state = primedAt(9000);
  feed(state, 40, kNoon, true);
  feed(state, 90, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(90, state.today);
}

// ── rejecting garbage ────────────────────────────────────────────────────────

void test_implausible_jump_is_rejected(void) {
  StepState state = primedAt(1000);
  const StepUpdate update = feed(state, 1000 + core::kMaxStepsPerMinute + 1, kNoon, true, 1);
  TEST_ASSERT_EQUAL_UINT32(0, update.added);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

void test_a_jump_exactly_at_the_limit_is_accepted(void) {
  StepState state = primedAt(1000);
  const StepUpdate update = feed(state, 1000 + core::kMaxStepsPerMinute, kNoon, true, 1);
  TEST_ASSERT_EQUAL_UINT32(core::kMaxStepsPerMinute, update.added);
}

void test_the_same_jump_is_accepted_over_a_longer_interval(void) {
  // 1000 steps is nonsense in a minute and ordinary over five. This is exactly why
  // the limit is not a flat number.
  const uint32_t jump = core::kMaxStepsPerMinute * 4;

  StepState fast = primedAt(1000);
  TEST_ASSERT_EQUAL_UINT32(0, feed(fast, 1000 + jump, kNoon, true, 1).added);

  StepState slow = primedAt(1000);
  TEST_ASSERT_EQUAL_UINT32(jump, feed(slow, 1000 + jump, kNoon, true, 5).added);
}

void test_a_rejected_reading_does_not_move_the_baseline(void) {
  // The subtle one. If a garbled 0xFFFFFFFF became the baseline, the next healthy
  // read would look like a counter restart and be credited in full — one bad I2C
  // transfer turning into thousands of phantom steps.
  StepState state = primedAt(1000);
  feed(state, UINT32_MAX, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(1000, state.last_raw);

  const StepUpdate update = feed(state, 1050, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(50, update.added);
  TEST_ASSERT_FALSE(update.counter_restarted);
  TEST_ASSERT_EQUAL_UINT32(50, state.today);
}

void test_garbage_is_rejected_even_over_a_long_gap(void) {
  StepState state = primedAt(1000);
  feed(state, UINT32_MAX, kNoon, true, 100000);
  TEST_ASSERT_EQUAL_UINT32(1000, state.last_raw);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

void test_today_saturates_rather_than_wrapping(void) {
  StepState state = primedAt(0);
  state.today = UINT32_MAX - 10;
  feed(state, 100, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, state.today);
}

// ── recovering from a gap that was real ──────────────────────────────────────

void test_a_single_rejection_holds_the_baseline(void) {
  // One garbled transfer must not disturb anything. This is the behaviour the
  // escape hatch below must NOT weaken.
  StepState state = primedAt(1000);
  const StepUpdate update = feed(state, UINT32_MAX, kNoon, true);
  TEST_ASSERT_TRUE(update.rejected);
  TEST_ASSERT_FALSE(update.baseline_dropped);
  TEST_ASSERT_TRUE(state.primed);
  TEST_ASSERT_EQUAL_UINT32(1000, state.last_raw);
}

void test_the_baseline_is_dropped_on_the_exact_rejection_limit(void) {
  StepState state = primedAt(1000);

  for (uint8_t i = 1; i < core::kMaxConsecutiveRejections; ++i) {
    const StepUpdate update = feed(state, UINT32_MAX, kNoon, true);
    TEST_ASSERT_TRUE(update.rejected);
    TEST_ASSERT_FALSE(update.baseline_dropped);
    TEST_ASSERT_TRUE(state.primed);
    TEST_ASSERT_EQUAL_UINT8(i, state.rejected_streak);
  }

  const StepUpdate update = feed(state, UINT32_MAX, kNoon, true);
  TEST_ASSERT_TRUE(update.rejected);
  TEST_ASSERT_TRUE(update.baseline_dropped);
  TEST_ASSERT_FALSE(state.primed);
  TEST_ASSERT_EQUAL_UINT32(0, update.added);
  TEST_ASSERT_EQUAL_UINT8(0, state.rejected_streak);
}

void test_an_accepted_reading_clears_the_rejection_streak(void) {
  // Occasional bad transfers spread over hours must never add up to a re-baseline.
  StepState state = primedAt(1000);
  for (int cycle = 0; cycle < 10; ++cycle) {
    for (uint8_t i = 1; i < core::kMaxConsecutiveRejections; ++i) {
      feed(state, UINT32_MAX, kNoon, true);
    }
    const StepUpdate good = feed(state, 1000 + 10u * (cycle + 1), kNoon, true);
    TEST_ASSERT_FALSE(good.rejected);
    TEST_ASSERT_EQUAL_UINT8(0, state.rejected_streak);
  }
  TEST_ASSERT_TRUE(state.primed);
  TEST_ASSERT_EQUAL_UINT32(100, state.today);
}

void test_a_real_gap_past_the_ceiling_recovers_instead_of_dying(void) {
  // The permanent-death case. A day of walking while the accelerometer was not
  // being read leaves a delta above the hard ceiling of
  // kMaxCreditedGapMinutes * kMaxStepsPerMinute, and since the chip's counter only
  // rises, every later reading is worse. Without an escape hatch `today` never
  // advances again — a reboot does not help, because last_raw is in RTC memory.
  StepState state = primedAt(1000);
  uint32_t raw = 1000 + core::kMaxStepsPerMinute * core::kMaxCreditedGapMinutes + 1;

  // Rejected while the streak builds, and the count stays stuck. The elapsed time
  // is generous on purpose: even the largest cap the limit can reach is exceeded,
  // which is what makes this unrecoverable without the escape hatch.
  for (uint8_t i = 0; i < core::kMaxConsecutiveRejections; ++i) {
    raw += 20;
    feed(state, raw, kNoon, true, 100000);
    TEST_ASSERT_EQUAL_UINT32(0, state.today);
  }
  TEST_ASSERT_FALSE(state.primed);

  // Next reading re-baselines and credits nothing — the gap is unknowable.
  raw += 20;
  const StepUpdate rebase = feed(state, raw, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(0, rebase.added);
  TEST_ASSERT_TRUE(state.primed);
  TEST_ASSERT_EQUAL_UINT32(raw, state.last_raw);

  // And counting is alive again.
  raw += 60;
  const StepUpdate resumed = feed(state, raw, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(60, resumed.added);
  TEST_ASSERT_EQUAL_UINT32(60, state.today);
}

void test_a_burst_of_garbage_does_not_leave_a_garbage_baseline(void) {
  // Why the escape hatch un-primes rather than adopting the offending value: if
  // 0xFFFFFFFF became the baseline, the next healthy read would look like a
  // counter restart and be credited in full — thousands of phantom steps, which
  // is exactly what rejecting was protecting against.
  StepState state = primedAt(1000);
  for (uint8_t i = 0; i < core::kMaxConsecutiveRejections; ++i) {
    feed(state, UINT32_MAX, kNoon, true);
  }
  TEST_ASSERT_FALSE(state.primed);

  const StepUpdate first = feed(state, 1050, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(0, first.added);
  TEST_ASSERT_FALSE(first.counter_restarted);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);

  const StepUpdate second = feed(state, 1100, kNoon, true);
  TEST_ASSERT_EQUAL_UINT32(50, second.added);
}

void test_recovery_survives_a_reboot_mid_streak(void) {
  // rejected_streak is persisted, so a reboot between rejections must not restart
  // the escape hatch's clock and strand the counter.
  StepState state = primedAt(1000);
  feed(state, UINT32_MAX, kNoon, true);

  const StepState carried = state;  // as if written to RTC memory and read back
  StepState resumed_state = carried;
  TEST_ASSERT_EQUAL_UINT8(1, resumed_state.rejected_streak);

  for (uint8_t i = 1; i < core::kMaxConsecutiveRejections; ++i) {
    feed(resumed_state, UINT32_MAX, kNoon, true);
  }
  TEST_ASSERT_FALSE(resumed_state.primed);
}

// ── midnight ─────────────────────────────────────────────────────────────────

void test_first_call_learns_the_day_without_rolling(void) {
  // day_epoch starts at 0, which is a real date (2000-01-01). Rolling on the first
  // call would fabricate a "yesterday".
  StepState state;
  const StepUpdate update = feed(state, 0, kNoon, true);
  TEST_ASSERT_FALSE(update.day_rolled);
  TEST_ASSERT_TRUE(state.day_known);
  TEST_ASSERT_EQUAL_UINT32(0, state.yesterday);
}

void test_midnight_moves_today_to_yesterday(void) {
  StepState state = primedOn(0, DateTime{2026, 8, 12, 23, 0, 0});
  feed(state, 200, DateTime{2026, 8, 12, 23, 59, 0}, true);
  TEST_ASSERT_EQUAL_UINT32(200, state.today);

  const StepUpdate update = feed(state, 200, DateTime{2026, 8, 13, 0, 0, 0}, true);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(200, state.yesterday);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

void test_steps_across_the_midnight_tick_belong_to_the_old_day(void) {
  // The delta measured at 00:00 covers 23:59 -> 00:00, which is yesterday's
  // walking. Crediting it to the new day would be visibly wrong at a glance.
  StepState state = primedOn(1000, DateTime{2026, 8, 12, 23, 0, 0});
  feed(state, 1200, DateTime{2026, 8, 12, 23, 59, 0}, true);

  feed(state, 1260, DateTime{2026, 8, 13, 0, 0, 0}, true);
  TEST_ASSERT_EQUAL_UINT32(260, state.yesterday);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

void test_rollover_across_a_month_boundary(void) {
  StepState state = primedOn(0, DateTime{2026, 8, 31, 23, 0, 0});
  feed(state, 200, DateTime{2026, 8, 31, 23, 59, 0}, true);
  const StepUpdate update = feed(state, 200, DateTime{2026, 9, 1, 0, 0, 0}, true);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(200, state.yesterday);
}

void test_rollover_across_a_year_boundary(void) {
  StepState state = primedOn(0, DateTime{2026, 12, 31, 23, 0, 0});
  feed(state, 150, DateTime{2026, 12, 31, 23, 59, 0}, true);
  const StepUpdate update = feed(state, 150, DateTime{2027, 1, 1, 0, 0, 0}, true);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(150, state.yesterday);
}

void test_rollover_across_a_leap_day(void) {
  StepState state = primedOn(0, DateTime{2024, 2, 28, 23, 0, 0});
  feed(state, 120, DateTime{2024, 2, 28, 23, 59, 0}, true);
  const StepUpdate update = feed(state, 120, DateTime{2024, 2, 29, 0, 0, 0}, true);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(120, state.yesterday);
}

void test_a_multi_day_gap_rolls_exactly_once(void) {
  // Watch flat for two days. The missing days cannot be reconstructed, but the
  // stale total must not be carried into today either.
  StepState state = primedAt(0);
  feed(state, 200, DateTime{2026, 8, 12, 13, 0, 0}, true);
  TEST_ASSERT_EQUAL_UINT32(200, state.today);

  const StepUpdate update = feed(state, 200, DateTime{2026, 8, 15, 9, 0, 0}, true, 4000);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(200, state.yesterday);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);

  const StepUpdate again = feed(state, 300, DateTime{2026, 8, 15, 9, 1, 0}, true);
  TEST_ASSERT_FALSE(again.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(100, state.today);
}

void test_the_same_day_never_rolls(void) {
  StepState state = primedAt(0);
  uint32_t raw = 0;
  for (uint8_t hour = 0; hour < 24; ++hour) {
    raw += 100;
    const StepUpdate update = feed(state, raw, DateTime{2026, 8, 12, hour, 30, 0}, true);
    TEST_ASSERT_FALSE(update.day_rolled);
  }
  TEST_ASSERT_EQUAL_UINT32(2400, state.today);
}

// ── an untrustworthy clock ───────────────────────────────────────────────────

void test_invalid_time_keeps_counting_but_does_not_roll(void) {
  // A dead RTC must not stop the step counter, and must not file the steps under a
  // fabricated date either.
  StepState state = primedWithoutClock(0);
  const StepUpdate update = feed(state, 250, DateTime{}, false);
  TEST_ASSERT_EQUAL_UINT32(250, update.added);
  TEST_ASSERT_EQUAL_UINT32(250, state.today);
  TEST_ASSERT_FALSE(update.day_rolled);
  TEST_ASSERT_FALSE(state.day_known);
}

void test_a_nonsense_datetime_flagged_valid_is_still_rejected(void) {
  // Defence in depth: time_valid says yes, the fields say otherwise.
  StepState state = primedWithoutClock(0);
  const StepUpdate update = feed(state, 100, DateTime{2026, 2, 30, 0, 0, 0}, true);
  TEST_ASSERT_EQUAL_UINT32(100, update.added);
  TEST_ASSERT_FALSE(update.day_rolled);
  TEST_ASSERT_FALSE(state.day_known);
}

void test_the_clock_coming_back_picks_the_day_up_again(void) {
  StepState state = primedWithoutClock(0);
  feed(state, 200, DateTime{}, false);
  TEST_ASSERT_FALSE(state.day_known);
  TEST_ASSERT_EQUAL_UINT32(200, state.today);

  // Clock set by the user: adopt the day without discarding what was counted.
  const StepUpdate update = feed(state, 200, kNoon, true);
  TEST_ASSERT_FALSE(update.day_rolled);
  TEST_ASSERT_TRUE(state.day_known);
  TEST_ASSERT_EQUAL_UINT32(200, state.today);
}

// ── what the face is allowed to show ─────────────────────────────────────────

void test_a_working_counter_is_shown(void) {
  const StepState state = primedAt(1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_the_feature_being_off_hides_the_line_entirely(void) {
  // Not "Stopped": nothing has failed, there is simply no step feature.
  const StepState state = primedAt(1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Hidden),
                        static_cast<int>(core::stepsDisplayFor(state, false, false)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Hidden),
                        static_cast<int>(core::stepsDisplayFor(state, false, true)));
}

void test_a_sensor_given_up_on_stops_being_reported_as_live(void) {
  // The count freezes the moment readings stop — including across midnight, since
  // nothing calls updateSteps() any more. A frozen number on the face is not a
  // stale reading, it is a wrong one, and it looks exactly like a right one.
  StepState state = primedAt(1000);
  // An hour of walking, credited over an hour's worth of ticks — 3412 in a single
  // minute would (correctly) be rejected as implausible.
  uint32_t raw = 1000;
  for (int minute = 0; minute < 60; ++minute) {
    raw += 57;
    feed(state, raw, kNoon, true);
  }
  TEST_ASSERT_EQUAL_UINT32(3420, state.today);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, true)));
}

void test_a_sensor_absent_from_the_start_is_not_reported_as_zero_steps(void) {
  // A BMA423 that never answered never gives up — an unanswering chip is
  // deliberately not charged a configuration attempt — and never primes, so
  // `today` sits at its initial 0. "0 steps" is as much a lie as a frozen 3412.
  //
  // Note the narrowness of this case: it is the sensor that was dead on arrival.
  // One that dies in service stays primed for ever, which is what the staleness
  // tests below are for.
  const StepState untouched;
  TEST_ASSERT_FALSE(untouched.primed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(untouched, true, false)));
}

// ── a sensor that dies in service ────────────────────────────────────────────

void test_a_count_frozen_by_readings_stopping_is_not_reported_as_live(void) {
  // The gap the give-up check alone does not cover, and the likelier field
  // failure: a chip that worked, primed the state, and then stopped acknowledging
  // on a bus the RTC still answers on. probe() says Absent, no configure attempt
  // is spent (so a transient can still recover), gave_up is never set and `primed`
  // stays true. Nothing about the state says "frozen" except the clock.
  StepState state = primedAt(1000);
  feed(state, 1200, kNoon, true);
  core::noteStepWake(state, true, true, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));

  for (uint32_t minute = 0; minute < core::kStaleStepMinutes; ++minute) {
    core::noteStepWake(state, /*expected=*/true, /*obtained=*/false, 1);
  }
  TEST_ASSERT_TRUE(state.primed);  // still primed: nothing else noticed it stop
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_the_staleness_boundary_is_exact(void) {
  StepState state = primedAt(1000);

  core::noteStepWake(state, true, false, core::kStaleStepMinutes - 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));

  core::noteStepWake(state, true, false, 1);
  TEST_ASSERT_EQUAL_UINT32(core::kStaleStepMinutes, state.minutes_since_reading);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_one_missed_reading_does_not_condemn_the_count(void) {
  // A single garbled transfer on a shared bus is not a dead sensor.
  StepState state = primedAt(1000);
  core::noteStepWake(state, true, false, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_staleness_is_measured_in_minutes_not_wakes(void) {
  // Why the threshold is in minutes: the tick is 5 minutes on a low battery, so
  // two missed wakes there are worth ten on the normal tick. Counting wakes would
  // make the low-battery watch the one that lies longest.
  StepState fast = primedAt(1000);
  for (int i = 0; i < 2; ++i) {
    core::noteStepWake(fast, true, false, 1);  // normal tick
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(fast, true, false)));

  StepState slow = primedAt(1000);
  for (int i = 0; i < 2; ++i) {
    core::noteStepWake(slow, true, false, 5);  // low-battery tick
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(slow, true, false)));
}

void test_a_wake_that_never_meant_to_read_is_not_a_missed_reading(void) {
  // Safe mode, Recovery mode and a switched-off feature all skip the sensor
  // legitimately. Ageing the count on those would turn one unrelated fault into a
  // second, false symptom: a watch recovering from a crash would also claim its
  // step counter had died.
  StepState state = primedAt(1000);
  for (int i = 0; i < 1000; ++i) {
    core::noteStepWake(state, /*expected=*/false, /*obtained=*/false, 15);
  }
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_reading);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_a_skipped_wake_neither_starts_nor_restarts_the_clock(void) {
  // The other half of "evidence of nothing": a Safe-mode spell in the middle of a
  // real outage must not refresh the count either.
  StepState state = primedAt(1000);
  core::noteStepWake(state, true, false, core::kStaleStepMinutes - 1);
  for (int i = 0; i < 10; ++i) {
    core::noteStepWake(state, false, false, 15);
  }
  TEST_ASSERT_EQUAL_UINT32(core::kStaleStepMinutes - 1, state.minutes_since_reading);

  core::noteStepWake(state, true, false, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_one_reading_restores_the_display(void) {
  // Recovery is automatic and immediate: a reading is proof the sensor answers.
  StepState state = primedAt(1000);
  core::noteStepWake(state, true, false, 10000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));

  core::noteStepWake(state, true, true, 1);
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_reading);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_the_staleness_clock_saturates(void) {
  // It lives in RTC memory across an arbitrarily long outage; wrapping would make
  // a months-dead sensor look freshly read.
  StepState state = primedAt(1000);
  for (int i = 0; i < 5; ++i) {
    core::noteStepWake(state, true, false, UINT32_MAX);
  }
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, state.minutes_since_reading);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

void test_a_reading_counts_even_if_it_was_not_expected(void) {
  // Defensive: the caller should not report an unexpected reading, but a value in
  // hand is a value in hand and must never leave the count looking stale.
  StepState state = primedAt(1000);
  core::noteStepWake(state, true, false, core::kStaleStepMinutes);
  core::noteStepWake(state, /*expected=*/false, /*obtained=*/true, 1);
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_reading);
}

void test_a_stale_count_is_still_hidden_when_the_feature_is_off(void) {
  // Hidden outranks Stopped: with no step feature there is no line to be wrong.
  StepState state = primedAt(1000);
  core::noteStepWake(state, true, false, core::kStaleStepMinutes * 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Hidden),
                        static_cast<int>(core::stepsDisplayFor(state, false, false)));
}

void test_the_display_recovers_with_the_counter(void) {
  // After the escape hatch drops the baseline the count is briefly un-primed; the
  // face says so, and goes back to Live on the next successful reading rather
  // than latching.
  StepState state = primedAt(1000);
  for (uint8_t i = 0; i < core::kMaxConsecutiveRejections; ++i) {
    feed(state, UINT32_MAX, kNoon, true);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Stopped),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));

  feed(state, 1050, kNoon, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::StepsDisplay::Live),
                        static_cast<int>(core::stepsDisplayFor(state, true, false)));
}

// ── a whole realistic day ────────────────────────────────────────────────────

void test_a_days_walking(void) {
  StepState state = primedAt(0);
  uint32_t raw = 0;

  // 1440 minute ticks, walking during waking hours only.
  for (uint16_t minute = 0; minute < 1440; ++minute) {
    const uint8_t hour = static_cast<uint8_t>(minute / 60);
    if (hour >= 7 && hour < 22) {
      raw += 8;
    }
    feed(state, raw, DateTime{2026, 8, 12, hour, static_cast<uint8_t>(minute % 60), 0}, true);
  }
  TEST_ASSERT_EQUAL_UINT32(15 * 60 * 8, state.today);

  // Then midnight.
  const StepUpdate update = feed(state, raw, DateTime{2026, 8, 13, 0, 0, 0}, true);
  TEST_ASSERT_TRUE(update.day_rolled);
  TEST_ASSERT_EQUAL_UINT32(7200, state.yesterday);
  TEST_ASSERT_EQUAL_UINT32(0, state.today);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_limit_scales_with_the_interval);
  RUN_TEST(test_limit_treats_zero_minutes_as_one);
  RUN_TEST(test_limit_stops_growing_after_an_hour);

  RUN_TEST(test_first_reading_only_establishes_a_baseline);
  RUN_TEST(test_reset_clears_everything);

  RUN_TEST(test_delta_is_credited);
  RUN_TEST(test_deltas_accumulate);
  RUN_TEST(test_a_still_wrist_adds_nothing);
  RUN_TEST(test_dead_sensor_reading_zero_forever);

  RUN_TEST(test_counter_restart_is_detected);
  RUN_TEST(test_restart_to_zero_credits_nothing);
  RUN_TEST(test_counting_resumes_normally_after_a_restart);

  RUN_TEST(test_implausible_jump_is_rejected);
  RUN_TEST(test_a_jump_exactly_at_the_limit_is_accepted);
  RUN_TEST(test_the_same_jump_is_accepted_over_a_longer_interval);
  RUN_TEST(test_a_rejected_reading_does_not_move_the_baseline);
  RUN_TEST(test_garbage_is_rejected_even_over_a_long_gap);
  RUN_TEST(test_today_saturates_rather_than_wrapping);

  RUN_TEST(test_a_single_rejection_holds_the_baseline);
  RUN_TEST(test_the_baseline_is_dropped_on_the_exact_rejection_limit);
  RUN_TEST(test_an_accepted_reading_clears_the_rejection_streak);
  RUN_TEST(test_a_real_gap_past_the_ceiling_recovers_instead_of_dying);
  RUN_TEST(test_a_burst_of_garbage_does_not_leave_a_garbage_baseline);
  RUN_TEST(test_recovery_survives_a_reboot_mid_streak);

  RUN_TEST(test_first_call_learns_the_day_without_rolling);
  RUN_TEST(test_midnight_moves_today_to_yesterday);
  RUN_TEST(test_steps_across_the_midnight_tick_belong_to_the_old_day);
  RUN_TEST(test_rollover_across_a_month_boundary);
  RUN_TEST(test_rollover_across_a_year_boundary);
  RUN_TEST(test_rollover_across_a_leap_day);
  RUN_TEST(test_a_multi_day_gap_rolls_exactly_once);
  RUN_TEST(test_the_same_day_never_rolls);

  RUN_TEST(test_invalid_time_keeps_counting_but_does_not_roll);
  RUN_TEST(test_a_nonsense_datetime_flagged_valid_is_still_rejected);
  RUN_TEST(test_the_clock_coming_back_picks_the_day_up_again);

  RUN_TEST(test_a_working_counter_is_shown);
  RUN_TEST(test_the_feature_being_off_hides_the_line_entirely);
  RUN_TEST(test_a_sensor_given_up_on_stops_being_reported_as_live);
  RUN_TEST(test_a_sensor_absent_from_the_start_is_not_reported_as_zero_steps);
  RUN_TEST(test_the_display_recovers_with_the_counter);

  RUN_TEST(test_a_count_frozen_by_readings_stopping_is_not_reported_as_live);
  RUN_TEST(test_the_staleness_boundary_is_exact);
  RUN_TEST(test_one_missed_reading_does_not_condemn_the_count);
  RUN_TEST(test_staleness_is_measured_in_minutes_not_wakes);
  RUN_TEST(test_a_wake_that_never_meant_to_read_is_not_a_missed_reading);
  RUN_TEST(test_a_skipped_wake_neither_starts_nor_restarts_the_clock);
  RUN_TEST(test_one_reading_restores_the_display);
  RUN_TEST(test_the_staleness_clock_saturates);
  RUN_TEST(test_a_reading_counts_even_if_it_was_not_expected);
  RUN_TEST(test_a_stale_count_is_still_hidden_when_the_feature_is_off);

  RUN_TEST(test_a_days_walking);

  return UNITY_END();
}
