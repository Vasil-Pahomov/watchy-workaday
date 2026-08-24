#include <unity.h>

#include "core/battery_model.h"

using core::BatteryFilter;
using core::BatteryLevel;
using core::BatteryLevelTracker;

void setUp(void) {}
void tearDown(void) {}

static int levelAsInt(BatteryLevel level) { return static_cast<int>(level); }

// ── the discharge curve ──────────────────────────────────────────────────────

void test_percent_endpoints(void) {
  TEST_ASSERT_EQUAL_UINT8(0, core::percentFromMillivolts(core::kBatteryEmptyMv));
  TEST_ASSERT_EQUAL_UINT8(100, core::percentFromMillivolts(core::kBatteryFullMv));
}

void test_percent_clamps_outside_the_curve(void) {
  TEST_ASSERT_EQUAL_UINT8(0, core::percentFromMillivolts(0));
  TEST_ASSERT_EQUAL_UINT8(0, core::percentFromMillivolts(2000));
  TEST_ASSERT_EQUAL_UINT8(0, core::percentFromMillivolts(3299));
  TEST_ASSERT_EQUAL_UINT8(100, core::percentFromMillivolts(4201));
  TEST_ASSERT_EQUAL_UINT8(100, core::percentFromMillivolts(65535));
}

void test_percent_is_monotonic_over_the_whole_range(void) {
  // A non-monotonic curve would make the displayed percentage climb while the
  // battery drains, and could make the level tracker oscillate.
  uint8_t previous = 0;
  for (uint32_t mv = 0; mv <= 5000; ++mv) {
    const uint8_t percent = core::percentFromMillivolts(static_cast<uint16_t>(mv));
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(100, percent);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(previous, percent);
    previous = percent;
  }
  TEST_ASSERT_EQUAL_UINT8(100, previous);
}

void test_percent_hits_curve_knots(void) {
  TEST_ASSERT_EQUAL_UINT8(10, core::percentFromMillivolts(3600));
  TEST_ASSERT_EQUAL_UINT8(25, core::percentFromMillivolts(3700));
  TEST_ASSERT_EQUAL_UINT8(55, core::percentFromMillivolts(3800));
  TEST_ASSERT_EQUAL_UINT8(85, core::percentFromMillivolts(4000));
}

void test_percent_interpolates_between_knots(void) {
  // Halfway between (3600, 10) and (3700, 25) is 17.5, rounded to 18.
  TEST_ASSERT_EQUAL_UINT8(18, core::percentFromMillivolts(3650));
  // The curve is deliberately non-linear: a straight line from empty to full
  // would read ~39 % here rather than 18 %.
  TEST_ASSERT_TRUE(core::percentFromMillivolts(3650) < 39);
}

// ── smoothing ────────────────────────────────────────────────────────────────

void test_filter_primes_on_first_sample(void) {
  // Ramping up from zero would show a flat battery for several minutes after
  // every reset.
  BatteryFilter filter;
  TEST_ASSERT_FALSE(filter.primed());
  TEST_ASSERT_EQUAL_UINT16(3900, filter.update(3900));
  TEST_ASSERT_TRUE(filter.primed());
  TEST_ASSERT_EQUAL_UINT16(3900, filter.value());
}

void test_filter_converges_and_damps(void) {
  BatteryFilter filter;
  filter.update(4000);

  // A single outlier moves the output by only a quarter of the step.
  const uint16_t after_one = filter.update(3600);
  TEST_ASSERT_EQUAL_UINT16(3900, after_one);

  // Repeated samples converge on the new value.
  for (int i = 0; i < 40; ++i) {
    filter.update(3600);
  }
  TEST_ASSERT_UINT16_WITHIN(2, 3600, filter.value());
}

void test_filter_is_stable_on_a_constant_input(void) {
  BatteryFilter filter;
  for (int i = 0; i < 50; ++i) {
    filter.update(3750);
  }
  // Rounding must not let a constant input drift.
  TEST_ASSERT_EQUAL_UINT16(3750, filter.value());
}

void test_filter_reset(void) {
  BatteryFilter filter;
  filter.update(4100);
  filter.reset();
  TEST_ASSERT_FALSE(filter.primed());
  TEST_ASSERT_EQUAL_UINT16(3500, filter.update(3500));
}

// ── hysteresis ───────────────────────────────────────────────────────────────

void test_tracker_starts_normal(void) {
  BatteryLevelTracker tracker;
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.level()));
}

void test_tracker_enters_low_and_critical(void) {
  BatteryLevelTracker tracker;
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.update(21)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.update(20)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Critical), levelAsInt(tracker.update(5)));
}

void test_tracker_holds_low_until_well_clear(void) {
  BatteryLevelTracker tracker;
  tracker.update(20);
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.level()));

  // Still Low inside the dead band — this is the whole point: without it the
  // level flaps, the tick interval flaps with it, and the load change moves the
  // voltage back across the threshold.
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.update(21)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.update(27)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.update(28)));
}

void test_tracker_holds_critical_until_well_clear(void) {
  BatteryLevelTracker tracker;
  tracker.update(3);
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Critical), levelAsInt(tracker.level()));

  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Critical), levelAsInt(tracker.update(6)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Critical), levelAsInt(tracker.update(11)));
  // Leaves via Low, not straight to Normal.
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.update(12)));
}

void test_tracker_full_band(void) {
  BatteryLevelTracker tracker;
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.update(96)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Full), levelAsInt(tracker.update(97)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Full), levelAsInt(tracker.update(93)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.update(91)));
}

void test_tracker_does_not_oscillate_on_a_threshold(void) {
  // 20 % is exactly the Low entry point. Dithering around it must produce one
  // transition, not one per sample.
  BatteryLevelTracker tracker;
  tracker.update(20);
  const int settled = levelAsInt(tracker.level());
  for (int i = 0; i < 20; ++i) {
    tracker.update(i % 2 == 0 ? 20 : 21);
    TEST_ASSERT_EQUAL_INT(settled, levelAsInt(tracker.level()));
  }
}

void test_tracker_handles_extremes(void) {
  BatteryLevelTracker tracker;
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Critical), levelAsInt(tracker.update(0)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Low), levelAsInt(tracker.update(100)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Normal), levelAsInt(tracker.update(100)));
  TEST_ASSERT_EQUAL_INT(levelAsInt(BatteryLevel::Full), levelAsInt(tracker.update(100)));
}

// ── consequences ─────────────────────────────────────────────────────────────

void test_tick_interval_stretches_when_low(void) {
  TEST_ASSERT_EQUAL_UINT16(60, core::tickIntervalSeconds(BatteryLevel::Full));
  TEST_ASSERT_EQUAL_UINT16(60, core::tickIntervalSeconds(BatteryLevel::Normal));
  TEST_ASSERT_EQUAL_UINT16(300, core::tickIntervalSeconds(BatteryLevel::Low));
  TEST_ASSERT_EQUAL_UINT16(300, core::tickIntervalSeconds(BatteryLevel::Critical));
}

void test_radio_blocked_when_low(void) {
  TEST_ASSERT_TRUE(core::radioPermitted(BatteryLevel::Full));
  TEST_ASSERT_TRUE(core::radioPermitted(BatteryLevel::Normal));
  TEST_ASSERT_FALSE(core::radioPermitted(BatteryLevel::Low));
  TEST_ASSERT_FALSE(core::radioPermitted(BatteryLevel::Critical));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_percent_endpoints);
  RUN_TEST(test_percent_clamps_outside_the_curve);
  RUN_TEST(test_percent_is_monotonic_over_the_whole_range);
  RUN_TEST(test_percent_hits_curve_knots);
  RUN_TEST(test_percent_interpolates_between_knots);

  RUN_TEST(test_filter_primes_on_first_sample);
  RUN_TEST(test_filter_converges_and_damps);
  RUN_TEST(test_filter_is_stable_on_a_constant_input);
  RUN_TEST(test_filter_reset);

  RUN_TEST(test_tracker_starts_normal);
  RUN_TEST(test_tracker_enters_low_and_critical);
  RUN_TEST(test_tracker_holds_low_until_well_clear);
  RUN_TEST(test_tracker_holds_critical_until_well_clear);
  RUN_TEST(test_tracker_full_band);
  RUN_TEST(test_tracker_does_not_oscillate_on_a_threshold);
  RUN_TEST(test_tracker_handles_extremes);

  RUN_TEST(test_tick_interval_stretches_when_low);
  RUN_TEST(test_radio_blocked_when_low);

  return UNITY_END();
}
