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

constexpr uint8_t kMenuItemCount = 2;

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
uint8_t activatedMenuItem(const UiState& state, ButtonId button);

// Applies a button press. Returns true when the visible content may have changed
// and the caller should re-render. Any press clears the idle timer.
bool handleButton(UiState& state, ButtonId button);

// Advances the idle timer. Returns true if this call bounced the UI back to the
// watchface (and therefore needs a redraw).
bool tickIdle(UiState& state, uint16_t minutes);

}  // namespace core
