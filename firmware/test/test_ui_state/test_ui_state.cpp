#include <unity.h>

#include "core/ui_state.h"

using core::ButtonId;
using core::Screen;
using core::UiState;

void setUp(void) {}
void tearDown(void) {}

static int screenAsInt(Screen screen) { return static_cast<int>(screen); }

// ── from the watchface ───────────────────────────────────────────────────────

void test_starts_on_the_watchface(void) {
  const UiState state;
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(0, state.menu_index);
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);
}

void test_menu_button_opens_the_menu(void) {
  UiState state;
  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Menu));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(0, state.menu_index);
}

void test_other_buttons_on_the_watchface_do_nothing(void) {
  // Returning false means no redraw, which means the wake costs only the wake
  // itself — no panel refresh, the expensive part.
  const ButtonId inert[] = {ButtonId::Back, ButtonId::Up, ButtonId::Down};
  for (const ButtonId button : inert) {
    UiState state;
    TEST_ASSERT_FALSE(core::handleButton(state, button));
    TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
  }
}

void test_none_is_ignored(void) {
  UiState state;
  state.idle_minutes = 5;
  TEST_ASSERT_FALSE(core::handleButton(state, ButtonId::None));
  TEST_ASSERT_EQUAL_UINT16(5, state.idle_minutes);  // not treated as activity
}

// ── in the menu ──────────────────────────────────────────────────────────────

void test_menu_navigation_wraps_both_ways(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Down));
  TEST_ASSERT_EQUAL_UINT8(1, state.menu_index);

  // Down from the last item wraps to the first.
  for (uint8_t i = 1; i < core::kMenuItemCount; ++i) {
    core::handleButton(state, ButtonId::Down);
  }
  TEST_ASSERT_EQUAL_UINT8(0, state.menu_index);

  // Up from the first wraps to the last.
  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Up));
  TEST_ASSERT_EQUAL_UINT8(core::kMenuItemCount - 1, state.menu_index);
}

void test_menu_index_stays_in_range_under_abuse(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  for (int i = 0; i < 500; ++i) {
    core::handleButton(state, (i % 3 == 0) ? ButtonId::Up : ButtonId::Down);
    TEST_ASSERT_LESS_THAN_UINT8(core::kMenuItemCount, state.menu_index);
  }
}

void test_menu_button_enters_the_selected_app(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);

  // Walk to the last item, whatever the menu currently holds. What this pins is
  // that entering an app carries the selection across — not that the menu has any
  // particular number of entries. An earlier version pressed Down twice and
  // asserted index 2, which stopped meaning anything the day the menu lost its
  // three unimplemented items: two presses then wrapped back to 0 and the test
  // failed for a reason that had nothing to do with what it was named for.
  for (uint8_t i = 1; i < core::kMenuItemCount; ++i) {
    core::handleButton(state, ButtonId::Down);
  }
  const uint8_t last = static_cast<uint8_t>(core::kMenuItemCount - 1);
  TEST_ASSERT_EQUAL_UINT8(last, state.menu_index);

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Menu));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(last, state.app_index);
}

void test_back_leaves_the_menu(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::handleButton(state, ButtonId::Down);

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Back));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(0, state.menu_index);  // reset for next time
}

// ── which item a press activated (the BLE sync request's only input) ─────────

void test_the_sync_item_is_inside_the_menu(void) {
  TEST_ASSERT_LESS_THAN_UINT8(core::kMenuItemCount, core::kSyncMenuIndex);
  // "nothing" must not be mistakable for an item, now or after the menu grows.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT8(core::kMenuItemCount, core::kNoMenuItem);
}

void test_the_menu_press_that_enters_an_app_names_the_item(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);  // watchface -> menu
  core::handleButton(state, ButtonId::Down);  // select item 1

  TEST_ASSERT_EQUAL_UINT8(1, core::activatedMenuItem(state, ButtonId::Menu));
}

void test_the_activated_item_is_the_one_handle_button_enters(void) {
  // The two must agree, because main.cpp reads the first and the wearer sees the
  // second. Checked for every item, so an off-by-one cannot hide in one of them.
  for (uint8_t item = 0; item < core::kMenuItemCount; ++item) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    for (uint8_t step = 0; step < item; ++step) {
      core::handleButton(state, ButtonId::Down);
    }

    const uint8_t activated = core::activatedMenuItem(state, ButtonId::Menu);
    core::handleButton(state, ButtonId::Menu);

    TEST_ASSERT_EQUAL_UINT8(item, activated);
    TEST_ASSERT_EQUAL_UINT8(activated, state.app_index);
    TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
  }
}

void test_reaching_sync_and_activating_it(void) {
  // The wearer's actual route to a radio window: open the menu, walk down to
  // Sync, press Menu. Up from the first item is the short way round, and it must
  // land on the same item.
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Up));
  TEST_ASSERT_EQUAL_UINT8(core::kMenuItemCount - 1, state.menu_index);

  TEST_ASSERT_EQUAL_UINT8(core::kSyncMenuIndex, core::activatedMenuItem(state, ButtonId::Menu));
}

void test_nothing_is_activated_from_the_watchface(void) {
  const ButtonId all[] = {ButtonId::None, ButtonId::Menu, ButtonId::Back, ButtonId::Up,
                          ButtonId::Down};
  for (const ButtonId button : all) {
    UiState state;  // on the watchface
    TEST_ASSERT_EQUAL_UINT8(core::kNoMenuItem, core::activatedMenuItem(state, button));
  }
}

void test_navigating_the_menu_activates_nothing(void) {
  // Walking to the Sync item is not the same as asking for a sync, or every Down
  // press past it would power up the radio.
  const ButtonId navigation[] = {ButtonId::None, ButtonId::Back, ButtonId::Up, ButtonId::Down};
  for (const ButtonId button : navigation) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    state.menu_index = core::kSyncMenuIndex;
    TEST_ASSERT_EQUAL_UINT8(core::kNoMenuItem, core::activatedMenuItem(state, button));
  }
}

void test_a_screen_left_open_activates_nothing(void) {
  // The rule the whole function exists for. Once Sync has been entered, the app
  // screen sits there until the idle timeout — through every minute tick in
  // between. If simply *being* on that screen counted as a request, the watch
  // would open a window on each of those wakes: PROTOCOL.md §5.1's interval gate
  // would refuse them, but the request must not be there to refuse in the first
  // place. Any press at all while an app is up activates nothing.
  const ButtonId all[] = {ButtonId::None, ButtonId::Menu, ButtonId::Back, ButtonId::Up,
                          ButtonId::Down};
  for (const ButtonId button : all) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    state.menu_index = core::kSyncMenuIndex;
    core::handleButton(state, ButtonId::Menu);  // now inside Sync
    TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));

    TEST_ASSERT_EQUAL_UINT8(core::kNoMenuItem, core::activatedMenuItem(state, button));
  }
}

void test_a_corrupt_menu_index_activates_nothing(void) {
  // menu_index lives in RTC memory, which is not zeroed and survives reflashing.
  // The block's magic and version catch a stale layout; this catches a bad byte
  // inside a block that still looks valid. An out-of-range index that came back
  // as kSyncMenuIndex by arithmetic accident would power up the radio.
  const uint8_t corrupt[] = {core::kMenuItemCount, static_cast<uint8_t>(core::kMenuItemCount + 1),
                             0x7F, core::kNoMenuItem};
  for (const uint8_t index : corrupt) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    state.menu_index = index;
    TEST_ASSERT_EQUAL_UINT8(core::kNoMenuItem, core::activatedMenuItem(state, ButtonId::Menu));
  }
}

void test_activation_does_not_mutate_the_state(void) {
  // It is asked before handleButton() and must leave everything for it to do.
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::handleButton(state, ButtonId::Down);
  core::tickIdle(state, 1);

  const UiState before = state;
  static_cast<void>(core::activatedMenuItem(state, ButtonId::Menu));

  TEST_ASSERT_EQUAL_INT(screenAsInt(before.screen), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(before.menu_index, state.menu_index);
  TEST_ASSERT_EQUAL_UINT8(before.app_index, state.app_index);
  TEST_ASSERT_EQUAL_UINT16(before.idle_minutes, state.idle_minutes);
}

// ── in an app ────────────────────────────────────────────────────────────────

void test_back_returns_from_app_to_menu(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Back));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
}

void test_app_ignores_other_buttons(void) {
  // The running app handles them; the state machine must not steal them.
  const ButtonId passthrough[] = {ButtonId::Menu, ButtonId::Up, ButtonId::Down};
  for (const ButtonId button : passthrough) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    core::handleButton(state, ButtonId::Menu);
    TEST_ASSERT_FALSE(core::handleButton(state, button));
    TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
  }
}

void test_full_round_trip(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);  // watchface -> menu
  core::handleButton(state, ButtonId::Menu);  // menu -> app
  core::handleButton(state, ButtonId::Back);  // app -> menu
  core::handleButton(state, ButtonId::Back);  // menu -> watchface
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
}

// ── the idle timeout ─────────────────────────────────────────────────────────

void test_idle_returns_to_the_watchface(void) {
  // A menu left up would otherwise keep the UI wake path alive indefinitely.
  UiState state;
  core::handleButton(state, ButtonId::Menu);

  TEST_ASSERT_FALSE(core::tickIdle(state, 1));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));

  TEST_ASSERT_TRUE(core::tickIdle(state, 1));  // reaches the 2-minute timeout
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);
}

void test_idle_timeout_from_an_app(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_TRUE(core::tickIdle(state, core::kIdleTimeoutMinutes));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
}

void test_idle_on_the_watchface_is_a_no_op(void) {
  UiState state;
  TEST_ASSERT_FALSE(core::tickIdle(state, 100));
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
}

void test_a_button_press_resets_the_idle_timer(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::tickIdle(state, 1);
  TEST_ASSERT_EQUAL_UINT16(1, state.idle_minutes);

  core::handleButton(state, ButtonId::Down);
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);

  // So the timeout is a full window away again.
  TEST_ASSERT_FALSE(core::tickIdle(state, 1));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
}

void test_inert_button_still_resets_the_idle_timer(void) {
  // A press that draws nothing is still user activity, so it must postpone the
  // timeout. Up inside an app is handled by the app, not the state machine, so
  // handleButton returns false — but the idle timer must reset all the same.
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  core::handleButton(state, ButtonId::Menu);  // now in an app
  core::tickIdle(state, 1);
  TEST_ASSERT_EQUAL_UINT16(1, state.idle_minutes);

  TEST_ASSERT_FALSE(core::handleButton(state, ButtonId::Up));
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
}

void test_large_idle_jump_does_not_overflow(void) {
  // A long sleep or a bad elapsed calculation must not wrap the counter and skip
  // the timeout.
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_TRUE(core::tickIdle(state, UINT16_MAX));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);
}

void test_zero_minute_tick_does_not_time_out(void) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_FALSE(core::tickIdle(state, 0));
  }
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_starts_on_the_watchface);
  RUN_TEST(test_menu_button_opens_the_menu);
  RUN_TEST(test_other_buttons_on_the_watchface_do_nothing);
  RUN_TEST(test_none_is_ignored);

  RUN_TEST(test_menu_navigation_wraps_both_ways);
  RUN_TEST(test_menu_index_stays_in_range_under_abuse);
  RUN_TEST(test_menu_button_enters_the_selected_app);
  RUN_TEST(test_back_leaves_the_menu);

  RUN_TEST(test_the_sync_item_is_inside_the_menu);
  RUN_TEST(test_the_menu_press_that_enters_an_app_names_the_item);
  RUN_TEST(test_the_activated_item_is_the_one_handle_button_enters);
  RUN_TEST(test_reaching_sync_and_activating_it);
  RUN_TEST(test_nothing_is_activated_from_the_watchface);
  RUN_TEST(test_navigating_the_menu_activates_nothing);
  RUN_TEST(test_a_screen_left_open_activates_nothing);
  RUN_TEST(test_a_corrupt_menu_index_activates_nothing);
  RUN_TEST(test_activation_does_not_mutate_the_state);

  RUN_TEST(test_back_returns_from_app_to_menu);
  RUN_TEST(test_app_ignores_other_buttons);
  RUN_TEST(test_full_round_trip);

  RUN_TEST(test_idle_returns_to_the_watchface);
  RUN_TEST(test_idle_timeout_from_an_app);
  RUN_TEST(test_idle_on_the_watchface_is_a_no_op);
  RUN_TEST(test_a_button_press_resets_the_idle_timer);
  RUN_TEST(test_inert_button_still_resets_the_idle_timer);
  RUN_TEST(test_large_idle_jump_does_not_overflow);
  RUN_TEST(test_zero_minute_tick_does_not_time_out);

  return UNITY_END();
}
