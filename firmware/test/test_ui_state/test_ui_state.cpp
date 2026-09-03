#include <unity.h>

#include "core/ui_state.h"

using core::ButtonId;
using core::Screen;
using core::UiState;

void setUp(void) {}
void tearDown(void) {}

static int screenAsInt(Screen screen) { return static_cast<int>(screen); }

// The last item that opens an app screen. Not simply kMenuItemCount - 1: the
// theme item acts in place rather than entering an app, and whether it happens to
// be last depends on what has been appended since, so a test about entering an
// app has to walk to something that enters one. Computed rather than written down
// for the reason the comment in test_menu_button_enters_the_selected_app gives —
// a literal here stops meaning anything the next time the menu changes shape.
static uint8_t lastAppItem(void) {
  for (uint8_t i = core::kMenuItemCount; i-- > 0;) {
    if (i != core::kThemeMenuIndex) {
      return i;
    }
  }
  return 0;
}

// Open the menu and walk the pointer to `item`.
static UiState menuAt(uint8_t item) {
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  for (uint8_t step = 0; step < item; ++step) {
    core::handleButton(state, ButtonId::Down);
  }
  return state;
}

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
  // Walk to the last item that opens an app, whatever the menu currently holds.
  // What this pins is that entering an app carries the selection across — not
  // that the menu has any particular number of entries. An earlier version
  // pressed Down twice and asserted index 2, which stopped meaning anything the
  // day the menu lost its three unimplemented items: two presses then wrapped
  // back to 0 and the test failed for a reason that had nothing to do with what
  // it was named for.
  const uint8_t last = lastAppItem();
  UiState state = menuAt(last);
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

void test_the_activated_item_is_the_one_handle_button_acts_on(void) {
  // The two must agree, because main.cpp reads the first and the wearer sees the
  // second. Checked for every item, so an off-by-one cannot hide in one of them.
  //
  // "Acts on" rather than "enters": the theme item is activated like any other —
  // main.cpp has to be told which item a press landed on — but activating it is a
  // toggle in place, not an app. Both halves are asserted here rather than the
  // theme item being skipped, because "which items open a screen" is exactly the
  // thing that must not drift.
  for (uint8_t item = 0; item < core::kMenuItemCount; ++item) {
    UiState state = menuAt(item);

    const uint8_t activated = core::activatedMenuItem(state, ButtonId::Menu);
    core::handleButton(state, ButtonId::Menu);

    TEST_ASSERT_EQUAL_UINT8(item, activated);
    if (item == core::kThemeMenuIndex) {
      TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
      TEST_ASSERT_EQUAL_UINT8(item, state.menu_index);  // the pointer has not moved
    } else {
      TEST_ASSERT_EQUAL_UINT8(activated, state.app_index);
      TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
    }
  }
}

void test_reaching_sync_and_activating_it(void) {
  // The wearer's actual route to a radio window: open the menu, walk down to
  // Sync, press Menu.
  UiState state = menuAt(core::kSyncMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(core::kSyncMenuIndex, state.menu_index);
  TEST_ASSERT_EQUAL_UINT8(core::kSyncMenuIndex, core::activatedMenuItem(state, ButtonId::Menu));
}

void test_the_short_way_round_no_longer_reaches_sync(void) {
  // Up from the first item wraps to the last, which used to be Sync, was then the
  // theme item, and is now Find phone. Worth pinning rather than deleting with
  // each append: it is the one press whose meaning an append silently changes,
  // and what it must NOT do is open a *sync* window — the Find phone item asks
  // for the radio too, but for a different session, and main.cpp tells the two
  // apart by exactly the comparison below.
  UiState state;
  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Up));
  TEST_ASSERT_EQUAL_UINT8(core::kMenuItemCount - 1, state.menu_index);
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, state.menu_index);

  TEST_ASSERT_TRUE(core::activatedMenuItem(state, ButtonId::Menu) != core::kSyncMenuIndex);
}

// ── the Find phone item ──────────────────────────────────────────────────────

void test_reaching_find_phone_and_activating_it(void) {
  // The wearer's route to a search: open the menu, walk to Find phone, press Menu.
  UiState state = menuAt(core::kFindPhoneMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, state.menu_index);
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, core::activatedMenuItem(state, ButtonId::Menu));
}

void test_the_find_phone_item_opens_a_screen(void) {
  // Unlike the theme item it enters an app: the search has a screen — elapsed
  // time, attempt counter, how it ended — and Back from it goes to the menu.
  UiState state = menuAt(core::kFindPhoneMenuIndex);
  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Menu));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::App), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, state.app_index);

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Back));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, state.menu_index);
}

void test_the_find_phone_item_is_neither_sync_nor_theme(void) {
  // main.cpp compares one activated index against two constants and does two
  // different things with the radio. The three items must stay three numbers.
  const UiState state = menuAt(core::kFindPhoneMenuIndex);
  const uint8_t activated = core::activatedMenuItem(state, ButtonId::Menu);
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, activated);
  TEST_ASSERT_TRUE(activated != core::kSyncMenuIndex);
  TEST_ASSERT_TRUE(activated != core::kThemeMenuIndex);
  TEST_ASSERT_FALSE(core::themeAfterButton(state, ButtonId::Menu, false).changed);
}

void test_a_find_screen_left_open_asks_for_nothing(void) {
  // Same rule as the Sync screen: once entered, no press — and no tick — may ask
  // for another search. Only the Menu press that enters it counts, which is what
  // keeps a "phone found" screen from re-running the radio on every wake.
  const ButtonId all[] = {ButtonId::None, ButtonId::Menu, ButtonId::Back, ButtonId::Up,
                          ButtonId::Down};
  for (const ButtonId button : all) {
    UiState state = menuAt(core::kFindPhoneMenuIndex);
    core::handleButton(state, ButtonId::Menu);  // now on the Find phone screen
    TEST_ASSERT_EQUAL_UINT8(core::kNoMenuItem, core::activatedMenuItem(state, button));
  }
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

// ── the display theme (the item that acts in place) ──────────────────────────

void test_items_are_appended_never_inserted(void) {
  // Appended, never inserted: an item added in the middle would change what an
  // already-persisted menu_index means (core/ui_state.h). So every index is
  // pinned where it was first written, and the newest item is the last one.
  TEST_ASSERT_EQUAL_UINT8(1, core::kSyncMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(2, core::kThemeMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(3, core::kFindPhoneMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(core::kMenuItemCount - 1, core::kFindPhoneMenuIndex);
  TEST_ASSERT_TRUE(core::kThemeMenuIndex != core::kSyncMenuIndex);
  TEST_ASSERT_LESS_THAN_UINT8(core::kMenuItemCount, core::kThemeMenuIndex);
}

void test_the_theme_item_does_not_leave_the_menu(void) {
  // The whole point of the item: the wearer stays where they are, watches the
  // label flip, and can press again. Moving to Screen::App would show an empty
  // app screen and cost a second press to get back.
  UiState state = menuAt(core::kThemeMenuIndex);
  const uint8_t app_before = state.app_index;

  TEST_ASSERT_TRUE(core::handleButton(state, ButtonId::Menu));  // the label changed
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(core::kThemeMenuIndex, state.menu_index);
  // Not entering an app must also not pretend one was entered: a Back press from
  // here goes to the watchface, and app_index has to still mean what it did.
  TEST_ASSERT_EQUAL_UINT8(app_before, state.app_index);
}

void test_the_theme_item_flips_the_theme(void) {
  const UiState state = menuAt(core::kThemeMenuIndex);

  const core::ThemeChange on = core::themeAfterButton(state, ButtonId::Menu, false);
  TEST_ASSERT_TRUE(on.inverted);
  TEST_ASSERT_TRUE(on.changed);

  const core::ThemeChange off = core::themeAfterButton(state, ButtonId::Menu, true);
  TEST_ASSERT_FALSE(off.inverted);
  TEST_ASSERT_TRUE(off.changed);
}

void test_pressing_it_twice_returns_to_where_it_started(void) {
  // What the wearer does when they change their mind. The state machine has to
  // leave the pointer on the item for the second press to land on it at all.
  UiState state = menuAt(core::kThemeMenuIndex);
  bool inverted = false;

  for (int press = 0; press < 4; ++press) {
    const core::ThemeChange change = core::themeAfterButton(state, ButtonId::Menu, inverted);
    inverted = change.inverted;
    core::handleButton(state, ButtonId::Menu);
    TEST_ASSERT_EQUAL_UINT8(core::kThemeMenuIndex, state.menu_index);
    TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Menu), screenAsInt(state.screen));
    // Odd presses inverted, even presses back to the shipped face.
    TEST_ASSERT_EQUAL_INT(press % 2 == 0 ? 1 : 0, inverted ? 1 : 0);
  }
  TEST_ASSERT_FALSE(inverted);  // four presses, back where it started
}

void test_a_flip_is_reported_only_on_the_press_that_flipped_it(void) {
  // `changed` is the caller's force-full-refresh input. Reporting it on a press
  // that changed nothing costs a 0.011 mAh full refresh for no reason; failing to
  // report it on the press that did costs a partial refresh of all 40 000 pixels,
  // which is the worst ghosting the panel can produce.
  const UiState menu_on_theme = menuAt(core::kThemeMenuIndex);
  TEST_ASSERT_TRUE(core::themeAfterButton(menu_on_theme, ButtonId::Menu, false).changed);

  const ButtonId navigation[] = {ButtonId::None, ButtonId::Back, ButtonId::Up, ButtonId::Down};
  for (const ButtonId button : navigation) {
    const core::ThemeChange change = core::themeAfterButton(menu_on_theme, button, true);
    TEST_ASSERT_FALSE(change.changed);
    TEST_ASSERT_TRUE(change.inverted);  // and the value is carried through unchanged
  }
}

void test_no_other_menu_item_flips_the_theme(void) {
  for (uint8_t item = 0; item < core::kMenuItemCount; ++item) {
    if (item == core::kThemeMenuIndex) {
      continue;
    }
    const UiState state = menuAt(item);
    const core::ThemeChange change = core::themeAfterButton(state, ButtonId::Menu, false);
    TEST_ASSERT_FALSE(change.changed);
    TEST_ASSERT_FALSE(change.inverted);
  }
}

void test_the_theme_is_not_flipped_outside_the_menu(void) {
  const ButtonId all[] = {ButtonId::None, ButtonId::Menu, ButtonId::Back, ButtonId::Up,
                          ButtonId::Down};
  for (const ButtonId button : all) {
    // On the watchface, where menu_index is 0 anyway.
    UiState watchface;
    TEST_ASSERT_FALSE(core::themeAfterButton(watchface, button, false).changed);

    // And inside an app with the pointer sitting on the theme item, which is the
    // state a corrupt index or a future in-app press could produce. Every wake
    // that arrives while a screen is open must leave the theme alone.
    UiState app = menuAt(core::kThemeMenuIndex);
    app.screen = Screen::App;
    TEST_ASSERT_FALSE(core::themeAfterButton(app, button, true).changed);
    TEST_ASSERT_TRUE(core::themeAfterButton(app, button, true).inverted);
  }
}

void test_a_corrupt_menu_index_does_not_flip_the_theme(void) {
  // Same reasoning as the Sync item's version of this test: menu_index comes out
  // of RTC memory, which is not zeroed and survives reflashing. A bad byte inside
  // an otherwise valid block must not invert the watch - and the values either
  // side of the item are where an inequality would let it.
  const uint8_t corrupt[] = {core::kMenuItemCount,
                             static_cast<uint8_t>(core::kThemeMenuIndex + 1),
                             static_cast<uint8_t>(core::kThemeMenuIndex + 2),
                             0x7F,
                             0x80,
                             core::kNoMenuItem};
  for (const uint8_t index : corrupt) {
    UiState state;
    core::handleButton(state, ButtonId::Menu);
    state.menu_index = index;
    TEST_ASSERT_FALSE(core::themeAfterButton(state, ButtonId::Menu, false).changed);
  }
}

void test_theme_after_button_does_not_mutate_the_state(void) {
  // It is asked before handleButton(), like activatedMenuItem(), and must leave
  // everything for it to do.
  UiState state = menuAt(core::kThemeMenuIndex);
  core::tickIdle(state, 1);

  const UiState before = state;
  static_cast<void>(core::themeAfterButton(state, ButtonId::Menu, false));

  TEST_ASSERT_EQUAL_INT(screenAsInt(before.screen), screenAsInt(state.screen));
  TEST_ASSERT_EQUAL_UINT8(before.menu_index, state.menu_index);
  TEST_ASSERT_EQUAL_UINT8(before.app_index, state.app_index);
  TEST_ASSERT_EQUAL_UINT16(before.idle_minutes, state.idle_minutes);
}

void test_the_theme_item_never_asks_for_a_radio_window(void) {
  // A press that inverts the screen must not also cost 0.031 mAh of BLE. main.cpp
  // reads activatedMenuItem() and compares it to kSyncMenuIndex, so this is the
  // comparison that keeps the two apart.
  const UiState state = menuAt(core::kThemeMenuIndex);
  TEST_ASSERT_EQUAL_UINT8(core::kThemeMenuIndex, core::activatedMenuItem(state, ButtonId::Menu));
  TEST_ASSERT_TRUE(core::activatedMenuItem(state, ButtonId::Menu) != core::kSyncMenuIndex);
}

void test_toggling_the_theme_still_resets_the_idle_timer(void) {
  // It is a press like any other: it postpones the timeout, and the menu it
  // leaves up still returns to the watchface on its own. A screen that stayed up
  // because of a toggle would keep the UI wake path alive indefinitely.
  UiState state = menuAt(core::kThemeMenuIndex);
  core::tickIdle(state, 1);
  TEST_ASSERT_EQUAL_UINT16(1, state.idle_minutes);

  core::handleButton(state, ButtonId::Menu);
  TEST_ASSERT_EQUAL_UINT16(0, state.idle_minutes);

  TEST_ASSERT_TRUE(core::tickIdle(state, core::kIdleTimeoutMinutes));
  TEST_ASSERT_EQUAL_INT(screenAsInt(Screen::Watchface), screenAsInt(state.screen));
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
  RUN_TEST(test_the_activated_item_is_the_one_handle_button_acts_on);
  RUN_TEST(test_reaching_sync_and_activating_it);
  RUN_TEST(test_the_short_way_round_no_longer_reaches_sync);
  RUN_TEST(test_nothing_is_activated_from_the_watchface);
  RUN_TEST(test_navigating_the_menu_activates_nothing);
  RUN_TEST(test_a_screen_left_open_activates_nothing);
  RUN_TEST(test_a_corrupt_menu_index_activates_nothing);
  RUN_TEST(test_activation_does_not_mutate_the_state);

  RUN_TEST(test_reaching_find_phone_and_activating_it);
  RUN_TEST(test_the_find_phone_item_opens_a_screen);
  RUN_TEST(test_the_find_phone_item_is_neither_sync_nor_theme);
  RUN_TEST(test_a_find_screen_left_open_asks_for_nothing);

  RUN_TEST(test_items_are_appended_never_inserted);
  RUN_TEST(test_the_theme_item_does_not_leave_the_menu);
  RUN_TEST(test_the_theme_item_flips_the_theme);
  RUN_TEST(test_pressing_it_twice_returns_to_where_it_started);
  RUN_TEST(test_a_flip_is_reported_only_on_the_press_that_flipped_it);
  RUN_TEST(test_no_other_menu_item_flips_the_theme);
  RUN_TEST(test_the_theme_is_not_flipped_outside_the_menu);
  RUN_TEST(test_a_corrupt_menu_index_does_not_flip_the_theme);
  RUN_TEST(test_theme_after_button_does_not_mutate_the_state);
  RUN_TEST(test_the_theme_item_never_asks_for_a_radio_window);
  RUN_TEST(test_toggling_the_theme_still_resets_the_idle_timer);

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
