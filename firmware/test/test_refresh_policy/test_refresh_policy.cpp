#include <unity.h>

#include "core/refresh_policy.h"

using core::RefreshKind;
using core::RefreshLimits;
using core::RefreshState;

void setUp(void) {}
void tearDown(void) {}

static int kindAsInt(RefreshKind kind) { return static_cast<int>(kind); }

static RefreshKind decide(RefreshState& state, uint32_t hash, uint32_t minutes = 1,
                          bool force_full = false) {
  return core::decideRefresh(state, hash, minutes, force_full);
}

// ── hashing ──────────────────────────────────────────────────────────────────

void test_hash_is_stable_and_discriminating(void) {
  TEST_ASSERT_EQUAL_UINT32(core::contentHash("14:30"), core::contentHash("14:30"));
  TEST_ASSERT_NOT_EQUAL_UINT32(core::contentHash("14:30"), core::contentHash("14:31"));
  TEST_ASSERT_NOT_EQUAL_UINT32(core::contentHash("14:30"), core::contentHash("14:03"));
}

void test_hash_handles_empty_and_null(void) {
  // Must not crash, and an empty screen must not collide with a drawn one.
  TEST_ASSERT_EQUAL_UINT32(core::contentHash(""), core::contentHash(""));
  TEST_ASSERT_NOT_EQUAL_UINT32(core::contentHash(""), core::contentHash("0"));

  const uint32_t null_string = core::contentHash(static_cast<const char*>(nullptr));
  TEST_ASSERT_EQUAL_UINT32(core::contentHash(""), null_string);

  const uint32_t null_buffer = core::contentHash(nullptr, 16);
  TEST_ASSERT_EQUAL_UINT32(core::contentHash(""), null_buffer);
}

void test_hash_combine_is_order_sensitive(void) {
  // Fields must not be interchangeable: "12:00 / 45%" and "45% / 12:00" are
  // different screens.
  const uint32_t a = core::hashCombine(core::contentHash("12:00"), "45");
  const uint32_t b = core::hashCombine(core::contentHash("45"), "12:00");
  TEST_ASSERT_NOT_EQUAL_UINT32(a, b);
}

void test_hash_combine_matches_a_concatenated_hash(void) {
  const uint32_t combined = core::hashCombine(core::contentHash("ab"), "cd");
  TEST_ASSERT_EQUAL_UINT32(core::contentHash("abcd"), combined);
}

void test_hash_over_binary_data(void) {
  const unsigned char one[] = {0x00, 0x01, 0x02};
  const unsigned char two[] = {0x00, 0x01, 0x03};
  TEST_ASSERT_EQUAL_UINT32(core::contentHash(one, sizeof(one)), core::contentHash(one, sizeof(one)));
  TEST_ASSERT_NOT_EQUAL_UINT32(core::contentHash(one, sizeof(one)),
                              core::contentHash(two, sizeof(two)));
  // Embedded NULs are honoured, unlike the string overload.
  TEST_ASSERT_NOT_EQUAL_UINT32(core::contentHash(one, sizeof(one)), core::contentHash(one, 1));
}

// ── the first draw ───────────────────────────────────────────────────────────

void test_first_draw_is_full(void) {
  // Nothing is known about what the panel is showing, so establish a baseline.
  RefreshState state;
  TEST_ASSERT_FALSE(state.drawn);
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Full), kindAsInt(decide(state, 111, 0)));
  TEST_ASSERT_TRUE(state.drawn);
  TEST_ASSERT_EQUAL_UINT32(111, state.content_hash);
  TEST_ASSERT_EQUAL_UINT16(0, state.partials_since_full);
}

// ── the energy win: skipping ─────────────────────────────────────────────────

void test_unchanged_content_is_skipped(void) {
  // This is the cheapest possible wake and ~85 % of a tick's energy.
  RefreshState state;
  decide(state, 111, 0);
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Skip), kindAsInt(decide(state, 111)));
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Skip), kindAsInt(decide(state, 111)));
}

void test_skip_does_not_accumulate_ghosting(void) {
  // No refresh means no new artefacts, so a skip must not consume the partial
  // budget — otherwise a static screen would trigger pointless full refreshes.
  RefreshState state;
  decide(state, 111, 0);
  for (int i = 0; i < 50; ++i) {
    TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Skip), kindAsInt(decide(state, 111, 0)));
  }
  TEST_ASSERT_EQUAL_UINT16(0, state.partials_since_full);
}

void test_changed_content_is_partial(void) {
  RefreshState state;
  decide(state, 111, 0);
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Partial), kindAsInt(decide(state, 222)));
  TEST_ASSERT_EQUAL_UINT32(222, state.content_hash);
  TEST_ASSERT_EQUAL_UINT16(1, state.partials_since_full);
}

void test_partials_accumulate(void) {
  RefreshState state;
  decide(state, 0, 0);
  for (uint32_t i = 1; i <= 10; ++i) {
    TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Partial), kindAsInt(decide(state, i)));
  }
  TEST_ASSERT_EQUAL_UINT16(10, state.partials_since_full);
}

// ── ghosting cleanup ─────────────────────────────────────────────────────────

void test_full_refresh_after_the_partial_budget(void) {
  RefreshState state;
  const RefreshLimits limits;
  decide(state, 0, 0);

  for (uint32_t i = 1; i <= limits.max_partials_before_full; ++i) {
    TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Partial), kindAsInt(decide(state, i)));
  }
  TEST_ASSERT_EQUAL_UINT16(limits.max_partials_before_full, state.partials_since_full);

  // Budget exhausted -> clear the ghosting.
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Full), kindAsInt(decide(state, 9999)));
  TEST_ASSERT_EQUAL_UINT16(0, state.partials_since_full);
}

void test_full_refresh_after_the_time_budget(void) {
  RefreshState state;
  const RefreshLimits limits;
  decide(state, 111, 0);

  // A minute short of the limit, content unchanged: still a skip.
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Skip),
                        kindAsInt(decide(state, 111, limits.max_minutes_before_full - 1)));
  // Crossing it forces the cleanup even though nothing changed.
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Full), kindAsInt(decide(state, 111, 1)));
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_full);
}

void test_time_budget_accumulates_across_skips(void) {
  RefreshState state;
  decide(state, 111, 0);
  for (int i = 0; i < 100; ++i) {
    decide(state, 111, 5);
  }
  // 500 minutes of skips must still be counted toward the 12 h cleanup.
  TEST_ASSERT_EQUAL_UINT32(500, state.minutes_since_full);
}

void test_minutes_counter_saturates(void) {
  // A wildly wrong elapsed value (a bad RTC read) must not wrap the counter and
  // silently reset the cleanup schedule.
  RefreshState state;
  decide(state, 111, 0);
  decide(state, 111, UINT32_MAX);
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Full), kindAsInt(decide(state, 111, UINT32_MAX)));
}

// ── forcing ──────────────────────────────────────────────────────────────────

void test_force_full_overrides_everything(void) {
  RefreshState state;
  decide(state, 111, 0);
  decide(state, 222);
  TEST_ASSERT_EQUAL_UINT16(1, state.partials_since_full);

  // Same content, would otherwise be a skip.
  TEST_ASSERT_EQUAL_INT(kindAsInt(RefreshKind::Full), kindAsInt(decide(state, 222, 1, true)));
  TEST_ASSERT_EQUAL_UINT16(0, state.partials_since_full);
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_full);
}

void test_full_resets_both_budgets(void) {
  RefreshState state;
  decide(state, 111, 500);
  TEST_ASSERT_EQUAL_UINT32(0, state.minutes_since_full);
  TEST_ASSERT_EQUAL_UINT16(0, state.partials_since_full);
}

// ── a realistic day ──────────────────────────────────────────────────────────

void test_a_days_worth_of_minute_ticks(void) {
  // 1440 ticks with an always-changing clock. Counts how many full refreshes the
  // policy actually spends, which is a direct battery cost.
  RefreshState state;
  int fulls = 0;
  int partials = 0;
  int skips = 0;

  for (uint32_t minute = 0; minute < 1440; ++minute) {
    switch (decide(state, minute, 1)) {
      case RefreshKind::Full:
        fulls++;
        break;
      case RefreshKind::Partial:
        partials++;
        break;
      case RefreshKind::Skip:
        skips++;
        break;
    }
  }

  TEST_ASSERT_EQUAL_INT(1440, fulls + partials + skips);
  TEST_ASSERT_EQUAL_INT(0, skips);  // HH:MM changes every minute
  // 1 initial + one per 60 partials. Budgeted at ~24/day in docs/power-budget.md;
  // if this number grows, the budget is wrong.
  TEST_ASSERT_EQUAL_INT(24, fulls);
  TEST_ASSERT_EQUAL_INT(1416, partials);
}

void test_a_static_screen_costs_almost_nothing(void) {
  // The opposite extreme: a screen that never changes. Only the 12 h ghosting
  // cleanups should cost anything at all.
  RefreshState state;
  int fulls = 0;
  int touched = 0;

  for (uint32_t minute = 0; minute < 1440; ++minute) {
    const RefreshKind kind = decide(state, 777, 1);
    if (kind == RefreshKind::Full) {
      fulls++;
    }
    if (kind != RefreshKind::Skip) {
      touched++;
    }
  }

  // Initial baseline + a single 12 h cleanup at minute 720. Against 1440 wakes,
  // the panel is driven twice: this is the whole point of the Skip path.
  TEST_ASSERT_EQUAL_INT(2, fulls);
  TEST_ASSERT_EQUAL_INT(2, touched);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_hash_is_stable_and_discriminating);
  RUN_TEST(test_hash_handles_empty_and_null);
  RUN_TEST(test_hash_combine_is_order_sensitive);
  RUN_TEST(test_hash_combine_matches_a_concatenated_hash);
  RUN_TEST(test_hash_over_binary_data);

  RUN_TEST(test_first_draw_is_full);

  RUN_TEST(test_unchanged_content_is_skipped);
  RUN_TEST(test_skip_does_not_accumulate_ghosting);
  RUN_TEST(test_changed_content_is_partial);
  RUN_TEST(test_partials_accumulate);

  RUN_TEST(test_full_refresh_after_the_partial_budget);
  RUN_TEST(test_full_refresh_after_the_time_budget);
  RUN_TEST(test_time_budget_accumulates_across_skips);
  RUN_TEST(test_minutes_counter_saturates);

  RUN_TEST(test_force_full_overrides_everything);
  RUN_TEST(test_full_resets_both_budgets);

  RUN_TEST(test_a_days_worth_of_minute_ticks);
  RUN_TEST(test_a_static_screen_costs_almost_nothing);

  return UNITY_END();
}
