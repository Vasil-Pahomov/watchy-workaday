// Screen composition: turns state into the text that will be drawn, and into the
// content hash the refresh policy uses to decide whether drawing is needed at all.
//
// compose() is deliberately separate from draw(): compose() is cheap and always
// runs, draw() costs a panel refresh and runs only when the hash says the pixels
// would actually differ.
#pragma once

#include <stdint.h>

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
  // The gauge in the top-right corner is the whole battery display, so this is
  // the whole battery input: the level the tracker is in decides the tick rate
  // and whether the radio may come up, but nothing about the picture.
  uint8_t battery_percent = 0;
  core::RunMode mode = core::RunMode::Normal;
  core::Screen screen = core::Screen::Watchface;
  uint8_t menu_index = 0;
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
};

// Renders `snapshot` into internal buffers and returns a hash of everything that
// will be visible. Equal hashes mean an identical screen, so the panel can be left
// alone entirely.
uint32_t compose(const Snapshot& snapshot);

// Paints the most recently composed content. Matches board::display::DrawFn.
void draw();

}  // namespace app
