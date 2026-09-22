// Screen composition: turns state into the text that will be drawn, and into the
// content hash the refresh policy uses to decide whether drawing is needed at all.
//
// compose() is deliberately separate from draw(): compose() is cheap and always
// runs, draw() costs a panel refresh and runs only when the hash says the pixels
// would actually differ.
#pragma once

#include <stdint.h>

#include "core/battery_model.h"
#include "core/find_session.h"
#include "core/health.h"
#include "core/protocol.h"
#include "core/step_counter.h"
#include "core/time_model.h"
#include "core/ui_state.h"

namespace app {

struct Snapshot {
  core::DateTime time;
  bool time_valid = false;
  bool use_24h = true;
  // The two halves of the battery display, and they are not the same reading.
  // `battery_percent` fills the gauge in the top-right corner. `battery_level` is
  // where the tracker's hysteresis has left the watch, which decides the tick
  // rate and whether the radio may come up — behaviour the wearer would otherwise
  // have no way to see, so core::batterySaving() turns it into the mark beside
  // the gauge. Deriving one from the other is exactly what cannot be done: the
  // same percentage sits on either side of the threshold depending on which way
  // the cell was going.
  uint8_t battery_percent = 0;
  core::BatteryLevel battery_level = core::BatteryLevel::Normal;
  core::RunMode mode = core::RunMode::Normal;
  core::Screen screen = core::Screen::Watchface;
  uint8_t menu_index = 0;
  // The wearer's choice of ink and paper (core::kThemeMenuIndex), carried from
  // the caller's persisted block. Two things read it: the theme item's label,
  // which states the current state rather than the action, and the content hash
  // — on EVERY screen, because inverting changes every pixel of every one of
  // them. Leave it out of the hash and a flip composes to an identical screen,
  // core::decideRefresh() answers Skip, and nothing is ever repainted.
  bool inverted = true;
  uint32_t steps_today = 0;
  uint32_t steps_yesterday = 0;
  // Runtime state, not a compile-time flag: the sensor can stop answering while
  // the feature is switched on, and the face must not keep presenting the last
  // number it saw as if it were current.
  core::StepsDisplay steps_display = core::StepsDisplay::Hidden;

  // What the last BLE sync window achieved (PROTOCOL.md §3.2), read out of
  // RTC-backed state — the same answer the phone gets, and it costs neither a
  // clock read nor an ADC read to show.
  //
  // Only the Sync screen renders it, and only the Sync screen hashes it, so a
  // sync result changing never redraws the watchface.
  core::SyncResult sync_result = core::SyncResult::Busy;
  // Whether an epoch has ever actually reached the PCF8563. Busy is both "never
  // synced" and "a window did not finish", and only this distinguishes them.
  bool sync_applied = false;

  // The Find phone screen (core::kFindPhoneMenuIndex, PROTOCOL.md §4.1). Only
  // that screen renders any of it and only that screen hashes it, so a search
  // never repaints anything else.
  //
  // `find_live` is true only for frames drawn while a session is running — the
  // first paint of the wake that starts one, and every redraw the session makes.
  // Then the phase, the attempt counter and the elapsed time are what is shown.
  // Off it, the screen shows `find_outcome` alone; a persisted InProgress with no
  // live session reads as "interrupted", because the one way that pair arises is
  // a wake that died inside a search.
  bool find_live = false;
  core::FindPhase find_phase = core::FindPhase::Searching;
  // Whether the phone has been asked for its alarm tone as well (§4.1's Menu
  // toggle). Read only in the Ringing phase, where it picks the status line.
  bool find_sound = false;
  core::FindOutcome find_outcome = core::FindOutcome::InProgress;
  uint8_t find_attempts = 0;
  uint16_t find_elapsed_s = 0;
};

// Renders `snapshot` into internal buffers and returns a hash of everything that
// will be visible. Equal hashes mean an identical screen, so the panel can be left
// alone entirely.
uint32_t compose(const Snapshot& snapshot);

// Paints the most recently composed content. Matches board::display::DrawFn.
void draw();

}  // namespace app
