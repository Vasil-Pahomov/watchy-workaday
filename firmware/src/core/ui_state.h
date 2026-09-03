// Screen/menu state machine. Pure logic — no rendering, no input polling.
//
// Kept out of the render path so that "which screen am I on and what did that
// button do" is a host-testable pure function rather than something you learn by
// pressing buttons on a watch.
#pragma once

#include <cstdint>

namespace core {

enum class Screen : uint8_t { Watchface, Menu, App };

enum class ButtonId : uint8_t { None, Menu, Back, Up, Down };

constexpr uint8_t kMenuItemCount = 4;

// The Sync item (PROTOCOL.md §5.1: "the Sync menu item opens a window immediately
// and resets the hourly timer").
//
// The index lives here rather than beside the labels in app/screens.cpp because
// two places need it and only one of them draws: the render path needs it to know
// which banner to paint, and main.cpp needs it to know that this press asked for a
// radio window. A second copy of the number in the module that cannot be unit
// tested is how the two would drift.
//
// **Appended, never inserted.** UiState::menu_index and app_index survive deep
// sleep inside the caller's RTC-backed block, and that block's version is not
// bumped by adding a label — so inserting an item in the middle would silently
// change what an already-persisted index means. Appending leaves every existing
// index meaning exactly what it did.
constexpr uint8_t kSyncMenuIndex = 1;
static_assert(kSyncMenuIndex < kMenuItemCount, "the Sync item must be in the menu");

// The display-theme item. Appended, for the reason given directly above: the two
// indices in UiState come back out of RTC-backed memory meaning whatever they
// meant when they were written, so a new item goes on the end and nowhere else.
//
// It is the one item that does NOT open an app. Pressing Menu on it flips the
// theme and leaves the pointer where it is, so the label under the cursor changes
// and a second press changes it back. That rule lives here rather than in main.cpp
// because two places depend on it and only one of them can be unit tested:
// handleButton() must not move to Screen::App for this item, and the caller must
// flip its persisted flag for exactly the same press. themeAfterButton() below is
// the single implementation both of them go through.
constexpr uint8_t kThemeMenuIndex = 2;
static_assert(kThemeMenuIndex < kMenuItemCount, "the theme item must be in the menu");
static_assert(kThemeMenuIndex != kSyncMenuIndex, "the theme item must not be the Sync item");

// The Find phone item (PROTOCOL.md §4.1). Appended after the theme item, for the
// same reason as everything above it: an item goes on the end and nowhere else.
//
// Like Sync it opens an app screen AND asks main.cpp for the radio — a find
// session rather than a sync window, which is why main.cpp compares the activated
// item against both indices and does two different things. Unlike Sync, what the
// screen shows afterwards depends on how the session ended, and that outcome
// lives in the caller's persisted block (core::FindOutcome) rather than here, for
// the reason the theme lives there: UiState is navigation and is reset by Back
// and by the idle timeout, and the message has to outlive both of those but not
// the next search.
constexpr uint8_t kFindPhoneMenuIndex = 3;
static_assert(kFindPhoneMenuIndex < kMenuItemCount, "the Find phone item must be in the menu");
static_assert(kFindPhoneMenuIndex != kSyncMenuIndex && kFindPhoneMenuIndex != kThemeMenuIndex,
              "the Find phone item must be its own item");

// "no item was activated" — outside the menu's range on purpose, so it cannot be
// confused with item 255 on a corrupt index.
constexpr uint8_t kNoMenuItem = 0xFF;

// Anything other than the watchface reverts after this long idle. This is an
// energy rule as much as a UX one: a menu left up would otherwise keep redrawing
// its selection state and keep the UI wake path alive indefinitely.
constexpr uint16_t kIdleTimeoutMinutes = 2;

struct UiState {
  Screen screen = Screen::Watchface;
  uint8_t menu_index = 0;
  uint8_t app_index = 0;
  uint16_t idle_minutes = 0;
};

// Which menu item this press is about to activate, or kNoMenuItem.
//
// **Call it with the state as it is BEFORE handleButton() runs**, because the
// selected item is only knowable from the pre-press state — handleButton() moves
// the screen to App and the information is gone.
//
// Why this exists as a separate function rather than a richer handleButton()
// return: main.cpp needs to know that *this* press asked for a BLE sync window,
// and it needs to know it on the wake the press arrived on. core::routeWake()
// runs at the top of the wake, long before the UI is dispatched, so a menu
// selection made on this wake cannot reach the plan it produced. The answer is a
// second evaluation of core::evaluateSyncWindow() after the dispatch — and this
// is the input it needs.
//
// The "activated on this press" wording is load-bearing. Sitting on the Sync app
// screen when the next tick arrives must not ask for another window: only the
// Menu press that *enters* an app counts, so a screen left open is inert. That is
// also why the request is never persisted — see core::SyncContext::user_requested.
//
// Returns kNoMenuItem for a menu_index outside the menu, which RTC-backed state
// can produce even inside a block whose magic and version still check out.
//
// kThemeMenuIndex is returned like any other item: that press activates it. What
// differs is only what activation *means* — the theme item acts in place instead
// of opening a screen, so the caller sees the item named here and handleButton()
// leaves the screen on Menu.
uint8_t activatedMenuItem(const UiState& state, ButtonId button);

// The display theme after this press, and whether this press is what changed it.
struct ThemeChange {
  // White ink on black paper is the watch's default, so false is the shipped
  // face and the flag names the departure from it.
  bool inverted = false;
  // Only the press that flipped it. The caller needs this separately from the
  // value because a flip is not an ordinary content change: it repaints all
  // 40 000 pixels at once, and every one of them the opposite way round. A
  // partial refresh of that leaves the whole previous frame as ghosting, which
  // is the one thing core::refresh_policy's full refresh exists to clear — so
  // this is the caller's `force_full` for that wake.
  bool changed = false;
};

// **Call it with the state as it is BEFORE handleButton() runs**, for the same
// reason activatedMenuItem() must be: the selected item is only knowable from the
// pre-press state.
//
// `inverted` is the theme as persisted; the return value is what the caller must
// store back. Returning the new value rather than taking a reference keeps the
// decision here and leaves the caller a plain assignment with no branch of its
// own to get wrong.
ThemeChange themeAfterButton(const UiState& state, ButtonId button, bool inverted);

// Applies a button press. Returns true when the visible content may have changed
// and the caller should re-render. Any press clears the idle timer.
//
// The theme item is the one Menu press that does not enter an app: it returns
// true (the label under the cursor has changed) with the screen and the pointer
// untouched. Flipping the flag itself is themeAfterButton()'s job, because the
// flag outlives this struct — it is a persisted preference, not navigation state.
bool handleButton(UiState& state, ButtonId button);

// Advances the idle timer. Returns true if this call bounced the UI back to the
// watchface (and therefore needs a redraw).
bool tickIdle(UiState& state, uint16_t minutes);

}  // namespace core
