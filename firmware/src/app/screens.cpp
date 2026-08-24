#include "app/screens.h"

#include <stdio.h>

#include "board/display.h"
#include "core/refresh_policy.h"

namespace app {
namespace {

// File-static because board::display::DrawFn is a plain function pointer: no
// std::function, so no heap allocation on a path that runs 1440 times a day.
struct Composed {
  char time[8];
  char date[16];
  char status[24];
  // Sized for the widest value the type allows, not the widest a wrist produces:
  // core::updateSteps saturates `today` at UINT32_MAX, which is 10 digits, plus
  // " steps" and the terminator = 17. snprintf would have truncated safely, but a
  // buffer that disagrees with the tested range is a trap for the next change.
  char steps[17];
  char steps_detail[24];  // "yesterday " + 10 digits + NUL = 21
  // The Sync screen's second line. drawBanner() starts at x=10 with text wrap
  // off, so about 17 characters of FreeMonoBold9pt reach the right edge — every
  // label below is inside that.
  char sync[24];
  core::Screen screen;
  uint8_t menu_index;
};

Composed g_composed = {"--:--", "", "", "", "", "", core::Screen::Watchface, 0};

constexpr uint8_t kStepsMenuIndex = 0;  // must track kMenuItems below

// Only items that do something. "Battery", "Set Time" and "About" used to sit
// here and drew "not implemented" when opened, which is a menu that wastes the
// wearer's time to tell them so. They come back when they do something — see
// docs/backlog.md items 1 and 11.
//
// The Sync item's index is core::kSyncMenuIndex, not a local constant: main.cpp
// reads the same number to tell that a press asked for a radio window, and one of
// the two places would drift.
const char* const kMenuItems[core::kMenuItemCount] = {"Steps", "Sync"};
static_assert(core::kMenuItemCount == 2, "kMenuItems and kMenuItemCount must agree");

// PROTOCOL.md §3.2's result codes, in the wearer's words rather than the wire's.
// Presentation, so it lives here beside modeTag() rather than in core/ — nothing
// downstream branches on the string.
const char* syncLabel(core::SyncResult result, bool applied) {
  switch (result) {
    case core::SyncResult::Ok:
      return "last sync ok";
    case core::SyncResult::BadLength:
    case core::SyncResult::BadType:
      return "bad message";
    case core::SyncResult::BadVersion:
      // §6.1: BadVersion means the two sides have drifted and retrying will not
      // help. Naming the app rather than the protocol is the actionable half.
      return "app mismatch";
    case core::SyncResult::OutOfRange:
      return "bad time sent";
    case core::SyncResult::RtcWriteFailed:
      return "clock write fail";
    case core::SyncResult::Busy:
      // The starting value as well as a real code, and the two need different
      // words. A watch that has committed an epoch and then reports Busy had a
      // window end early; one that never has, never synced.
      break;
  }
  return applied ? "sync unfinished" : "never synced";
}

const char* modeTag(core::RunMode mode) {
  switch (mode) {
    case core::RunMode::Safe:
      return " SAFE";
    case core::RunMode::Recovery:
      return " RECOV";
    case core::RunMode::Normal:
      break;
  }
  return "";
}

}  // namespace

uint32_t compose(const Snapshot& snapshot) {
  g_composed.screen = snapshot.screen;
  g_composed.menu_index = snapshot.menu_index;

  if (snapshot.time_valid) {
    core::formatTime(g_composed.time, sizeof(g_composed.time), snapshot.time, snapshot.use_24h);
    core::formatDate(g_composed.date, sizeof(g_composed.date), snapshot.time);
  } else {
    // An invalid clock is shown as such rather than as a plausible wrong time.
    snprintf(g_composed.time, sizeof(g_composed.time), "--:--");
    snprintf(g_composed.date, sizeof(g_composed.date), "set time");
  }

  const char* low = snapshot.battery_level == core::BatteryLevel::Critical ? "!" : "";
  snprintf(g_composed.status, sizeof(g_composed.status), "%u%%%s%s", snapshot.battery_percent,
           low, modeTag(snapshot.mode));

  switch (snapshot.steps_display) {
    case core::StepsDisplay::Live:
      snprintf(g_composed.steps, sizeof(g_composed.steps), "%lu steps",
               static_cast<unsigned long>(snapshot.steps_today));
      snprintf(g_composed.steps_detail, sizeof(g_composed.steps_detail), "yesterday %lu",
               static_cast<unsigned long>(snapshot.steps_yesterday));
      break;

    case core::StepsDisplay::Stopped:
      // Deliberately not "%lu steps" with the last known total. That number stops
      // advancing and stops rolling over at midnight, so within a day it is not a
      // stale reading — it is a wrong one, and it looks exactly like a right one.
      // The count is still in RTC state for diagnostics; what the wearer must not
      // be given is a dead number presented as a live one.
      snprintf(g_composed.steps, sizeof(g_composed.steps), "no step data");
      snprintf(g_composed.steps_detail, sizeof(g_composed.steps_detail), "sensor stopped");
      break;

    case core::StepsDisplay::Hidden:
      g_composed.steps[0] = '\0';
      g_composed.steps_detail[0] = '\0';
      break;
  }

  snprintf(g_composed.sync, sizeof(g_composed.sync), "%s",
           syncLabel(snapshot.sync_result, snapshot.sync_applied));

  // Hash exactly what will be visible — nothing more, or the screen redraws for
  // invisible changes; nothing less, or a real change is missed and the panel shows
  // stale pixels.
  uint32_t hash = core::contentHash(g_composed.time);
  hash = core::hashCombine(hash, g_composed.date);
  hash = core::hashCombine(hash, g_composed.status);
  hash = core::hashCombine(hash, g_composed.steps);
  hash = core::hashCombine(hash, &g_composed.screen, sizeof(g_composed.screen));
  if (g_composed.screen == core::Screen::Menu) {
    hash = core::hashCombine(hash, &g_composed.menu_index, sizeof(g_composed.menu_index));
  }
  if (g_composed.screen == core::Screen::App) {
    hash = core::hashCombine(hash, &g_composed.menu_index, sizeof(g_composed.menu_index));
    hash = core::hashCombine(hash, g_composed.steps_detail);
    // Only here. A sync result changing must repaint the Sync screen if it is up,
    // and must never repaint the watchface — that would be a ~0.003 mAh refresh
    // for something not on screen.
    hash = core::hashCombine(hash, g_composed.sync);
  }
  return hash;
}

void draw() {
  switch (g_composed.screen) {
    case core::Screen::Menu:
      board::display::drawMenu(kMenuItems, core::kMenuItemCount, g_composed.menu_index);
      return;

    case core::Screen::App: {
      board::display::drawStatusLine(g_composed.status);
      const uint8_t index =
          g_composed.menu_index < core::kMenuItemCount ? g_composed.menu_index : 0;
      if (index == kStepsMenuIndex && g_composed.steps[0] != '\0') {
        board::display::drawBanner(g_composed.steps, g_composed.steps_detail);
      } else if (index == core::kSyncMenuIndex) {
        // What the *previous* window achieved. This screen is painted before the
        // window this press opened has run — the radio comes up after the panel
        // is done with, so the wearer sees something immediately instead of
        // waiting six seconds for a refresh. The new outcome is on the screen the
        // next time it is opened; re-rendering it here would cost a second panel
        // refresh per sync, which is ~0.26 mAh/day at the hourly cadence and is
        // not in the budget.
        board::display::drawBanner(kMenuItems[index], g_composed.sync);
      } else {
        // kStepsMenuIndex, the only other item. An empty string means the counter
        // is not running at all, which is a different thing from "no step data"
        // (the sensor answered and the reading was stale) and needs its own words.
        board::display::drawBanner(
            g_composed.steps[0] != '\0' ? g_composed.steps : "steps off",
            g_composed.steps_detail);
      }
      return;
    }

    case core::Screen::Watchface:
      break;
  }

  board::display::drawStatusLine(g_composed.status);
  board::display::drawTimeLarge(g_composed.time);
  board::display::drawDateLine(g_composed.date);
  board::display::drawStepsLine(g_composed.steps);
}

}  // namespace app
