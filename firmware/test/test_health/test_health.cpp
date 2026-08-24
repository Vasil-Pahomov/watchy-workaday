#include <unity.h>

#include "core/health.h"

using core::HealthState;
using core::ResetReason;
using core::RunMode;

void setUp(void) {}
void tearDown(void) {}

static int modeAsInt(RunMode mode) { return static_cast<int>(mode); }

// A state that has already been through one clean boot-and-sleep cycle.
static HealthState settledState(void) {
  HealthState state;
  core::initHealthState(state);
  core::beginBoot(state, ResetReason::PowerOn);
  core::markRunComplete(state);
  return state;
}

// ── surviving garbage in RTC memory ──────────────────────────────────────────

void test_zeroed_state_is_invalid(void) {
  const HealthState state;
  TEST_ASSERT_FALSE(core::isPersistedStateValid(state));
}

void test_garbage_state_is_invalid(void) {
  // RTC_DATA_ATTR memory survives reflashing, so on the first boot of a new build
  // this is the previous build's bytes. Trusting them would mean booting straight
  // into Recovery mode with a fabricated fault count.
  HealthState state;
  state.magic = 0xDEADBEEFu;
  state.version = 99;
  state.consecutive_faults = 4321;
  state.boot_count = 999999;
  TEST_ASSERT_FALSE(core::isPersistedStateValid(state));

  core::beginBoot(state, ResetReason::PowerOn);
  TEST_ASSERT_TRUE(core::isPersistedStateValid(state));
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
  TEST_ASSERT_EQUAL_UINT32(1, state.boot_count);
}

void test_version_mismatch_is_invalid(void) {
  // A schema change must discard the old layout rather than misread it.
  HealthState state;
  core::initHealthState(state);
  TEST_ASSERT_TRUE(core::isPersistedStateValid(state));
  state.version = static_cast<uint8_t>(core::kHealthVersion + 1);
  TEST_ASSERT_FALSE(core::isPersistedStateValid(state));
}

void test_init_stamps_magic_and_version(void) {
  HealthState state;
  core::initHealthState(state);
  TEST_ASSERT_EQUAL_UINT32(core::kHealthMagic, state.magic);
  TEST_ASSERT_EQUAL_UINT8(core::kHealthVersion, state.version);
  TEST_ASSERT_EQUAL_UINT32(0, state.boot_count);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
  TEST_ASSERT_FALSE(state.run_completed);
}

void test_valid_state_is_preserved_across_boots(void) {
  HealthState state = settledState();
  const uint32_t boots_before = state.boot_count;
  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_EQUAL_UINT32(boots_before + 1, state.boot_count);
}

// ── which reasons count as faults ────────────────────────────────────────────

void test_fault_reason_classification(void) {
  TEST_ASSERT_TRUE(core::isFaultReason(ResetReason::Panic));
  TEST_ASSERT_TRUE(core::isFaultReason(ResetReason::TaskWatchdog));
  TEST_ASSERT_TRUE(core::isFaultReason(ResetReason::IntWatchdog));
  TEST_ASSERT_TRUE(core::isFaultReason(ResetReason::Brownout));
  TEST_ASSERT_TRUE(core::isFaultReason(ResetReason::Unknown));  // fails safe

  TEST_ASSERT_FALSE(core::isFaultReason(ResetReason::PowerOn));
  TEST_ASSERT_FALSE(core::isFaultReason(ResetReason::DeepSleepWake));
  TEST_ASSERT_FALSE(core::isFaultReason(ResetReason::SoftwareRestart));
  TEST_ASSERT_FALSE(core::isFaultReason(ResetReason::ExternalPin));
}

// ── counting ─────────────────────────────────────────────────────────────────

void test_clean_wake_keeps_counter_at_zero(void) {
  HealthState state = settledState();
  for (int i = 0; i < 10; ++i) {
    core::beginBoot(state, ResetReason::DeepSleepWake);
    core::markRunComplete(state);
  }
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal), modeAsInt(state.mode));
}

void test_panic_increments_both_counters(void) {
  HealthState state = settledState();
  core::beginBoot(state, ResetReason::Panic);
  TEST_ASSERT_EQUAL_UINT16(1, state.consecutive_faults);
  TEST_ASSERT_EQUAL_UINT16(1, state.total_faults);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResetReason::Panic), static_cast<int>(state.last_reason));
}

void test_clean_boot_clears_consecutive_but_not_total(void) {
  HealthState state = settledState();
  core::beginBoot(state, ResetReason::Panic);
  core::beginBoot(state, ResetReason::Panic);
  core::markRunComplete(state);
  TEST_ASSERT_EQUAL_UINT16(2, state.consecutive_faults);

  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
  TEST_ASSERT_EQUAL_UINT16(2, state.total_faults);  // history is kept
}

void test_silent_death_counts_as_a_fault(void) {
  // No panic, no watchdog: the previous run simply never reached deep sleep.
  // Without this rule the failure is invisible and never escalates.
  HealthState state = settledState();
  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);

  // run_completed was cleared by beginBoot and never set again.
  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_EQUAL_UINT16(1, state.consecutive_faults);
}

void test_mark_run_complete_is_what_clears_the_counter(void) {
  HealthState state = settledState();
  core::beginBoot(state, ResetReason::Panic);
  TEST_ASSERT_EQUAL_UINT16(1, state.consecutive_faults);

  // Surviving to sleep, then a clean wake, is the only thing that resets it.
  core::markRunComplete(state);
  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
}

void test_begin_boot_clears_run_completed(void) {
  HealthState state = settledState();
  TEST_ASSERT_TRUE(state.run_completed);
  core::beginBoot(state, ResetReason::DeepSleepWake);
  TEST_ASSERT_FALSE(state.run_completed);
}

void test_power_on_clears_the_counter(void) {
  // Battery reinserted or a deliberate reset: a fresh start.
  HealthState state = settledState();
  core::beginBoot(state, ResetReason::Panic);
  core::beginBoot(state, ResetReason::Panic);
  TEST_ASSERT_EQUAL_UINT16(2, state.consecutive_faults);

  core::beginBoot(state, ResetReason::PowerOn);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
}

// ── escalation ───────────────────────────────────────────────────────────────

void test_mode_thresholds(void) {
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal), modeAsInt(core::modeForFaultCount(0)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal), modeAsInt(core::modeForFaultCount(1)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal), modeAsInt(core::modeForFaultCount(2)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Safe), modeAsInt(core::modeForFaultCount(3)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Safe), modeAsInt(core::modeForFaultCount(5)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Recovery), modeAsInt(core::modeForFaultCount(6)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Recovery), modeAsInt(core::modeForFaultCount(100)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Recovery), modeAsInt(core::modeForFaultCount(UINT16_MAX)));
}

void test_crash_loop_escalates_then_recovers(void) {
  // The scenario this module exists for: without escalation, a fault that
  // reproduces every wake reboots forever at ~40 mA and flattens a 200 mAh cell
  // in about five hours.
  HealthState state = settledState();

  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal),
                        modeAsInt(core::beginBoot(state, ResetReason::TaskWatchdog)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal),
                        modeAsInt(core::beginBoot(state, ResetReason::TaskWatchdog)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Safe),
                        modeAsInt(core::beginBoot(state, ResetReason::TaskWatchdog)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Safe),
                        modeAsInt(core::beginBoot(state, ResetReason::Panic)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Safe),
                        modeAsInt(core::beginBoot(state, ResetReason::Panic)));
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Recovery),
                        modeAsInt(core::beginBoot(state, ResetReason::Panic)));

  // One surviving run is enough to climb all the way back out.
  core::markRunComplete(state);
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal),
                        modeAsInt(core::beginBoot(state, ResetReason::DeepSleepWake)));
}

void test_returned_mode_matches_stored_mode(void) {
  HealthState state = settledState();
  const RunMode returned = core::beginBoot(state, ResetReason::Panic);
  TEST_ASSERT_EQUAL_INT(modeAsInt(returned), modeAsInt(state.mode));
}

// ── overflow ─────────────────────────────────────────────────────────────────

void test_counters_saturate_rather_than_wrap(void) {
  // Wrapping would drop a watch in a permanent crash loop back to Normal mode and
  // restart the whole cycle.
  HealthState state = settledState();
  state.consecutive_faults = UINT16_MAX;
  state.total_faults = UINT16_MAX;
  state.boot_count = UINT32_MAX;

  core::beginBoot(state, ResetReason::Panic);

  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, state.consecutive_faults);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, state.total_faults);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, state.boot_count);
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Recovery), modeAsInt(state.mode));
}

void test_first_ever_boot(void) {
  // Uninitialised memory, PowerOn: must land in Normal mode, boot 1.
  HealthState state;
  const RunMode mode = core::beginBoot(state, ResetReason::PowerOn);
  TEST_ASSERT_EQUAL_INT(modeAsInt(RunMode::Normal), modeAsInt(mode));
  TEST_ASSERT_EQUAL_UINT32(1, state.boot_count);
  TEST_ASSERT_EQUAL_UINT16(0, state.consecutive_faults);
  TEST_ASSERT_EQUAL_UINT16(0, state.total_faults);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_zeroed_state_is_invalid);
  RUN_TEST(test_garbage_state_is_invalid);
  RUN_TEST(test_version_mismatch_is_invalid);
  RUN_TEST(test_init_stamps_magic_and_version);
  RUN_TEST(test_valid_state_is_preserved_across_boots);

  RUN_TEST(test_fault_reason_classification);

  RUN_TEST(test_clean_wake_keeps_counter_at_zero);
  RUN_TEST(test_panic_increments_both_counters);
  RUN_TEST(test_clean_boot_clears_consecutive_but_not_total);
  RUN_TEST(test_silent_death_counts_as_a_fault);
  RUN_TEST(test_mark_run_complete_is_what_clears_the_counter);
  RUN_TEST(test_begin_boot_clears_run_completed);
  RUN_TEST(test_power_on_clears_the_counter);

  RUN_TEST(test_mode_thresholds);
  RUN_TEST(test_crash_loop_escalates_then_recovers);
  RUN_TEST(test_returned_mode_matches_stored_mode);

  RUN_TEST(test_counters_saturate_rather_than_wrap);
  RUN_TEST(test_first_ever_boot);

  return UNITY_END();
}
