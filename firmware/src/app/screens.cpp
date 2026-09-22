#include "app/screens.h"

#include <stdio.h>

#include "board/display.h"
#include "core/battery_model.h"
#include "core/refresh_policy.h"

namespace app {
namespace {

// File-static because board::display::DrawFn is a plain function pointer: no
// std::function, so no heap allocation on a path that runs 1440 times a day.
struct Composed {
  char time[8];
  char date[16];
  // The mode tag alone now. The percentage that used to sit in front of it is a
  // gauge in the opposite corner, drawn from `battery_percent` below.
  char status[8];
  // Sized for the widest value the type allows, not the widest a wrist produces:
  // core::updateSteps saturates `today` at UINT32_MAX, which is 10 digits, plus
  // " steps" and the terminator = 17. snprintf would have truncated safely, but a
  // buffer that disagrees with the tested range is a trap for the next change.
  char steps[17];
  // The same count with the word taken off, for the watchface corner. Two
  // buffers rather than one because the two places disagree about what the
  // number needs beside it: the corner has room for digits, and the Steps screen
  // has room to say what they are.
  char steps_face[11];    // 10 digits + NUL
  char steps_detail[24];  // "yesterday " + 10 digits + NUL = 21
  // The Sync screen's second line. drawBanner() starts at x=10 with text wrap
  // off, and WorkadaySmall is sized so that every label below still clears the
  // right edge — see tools/make_time_font.py, where those strings are the input.
  char sync[24];
  // The Find phone screen's second and third lines: what the search is doing or
  // how it ended, and "m:ss  try N" while it runs. Every string either can hold
  // is in tools/make_time_font.py's list; the widest, "no phone found", inks
  // 154 px against the 180 px a banner is budgeted.
  char find_status[24];
  char find_progress[24];
  core::Screen screen;
  uint8_t menu_index;
  uint8_t battery_percent;
  // Whether the gauge carries the low-battery mark. Stored as the answer rather
  // than as the level behind it, because the answer is what is drawn and what is
  // hashed: Low and Critical paint the same mark, and a hash keyed on the level
  // would pay for a panel refresh every time the cell crossed between them.
  bool battery_warning;
  bool inverted;
};

Composed g_composed = {"--:--", "", "", "", "", "", "", "", "",
                       core::Screen::Watchface, 0, 0, false, false};

constexpr uint8_t kStepsMenuIndex = 0;  // must track kMenuLabels below

// Only items that do something. "Battery", "Set Time" and "About" used to sit
// here and drew "not implemented" when opened, which is a menu that wastes the
// wearer's time to tell them so. They come back when they do something — see
// docs/backlog.md items 1 and 11.
//
// One slot per core index, so the table cannot drift from the indices main.cpp
// compares a press against: the Sync item is core::kSyncMenuIndex and the Find
// phone item is core::kFindPhoneMenuIndex, both read from core rather than
// retyped here. The theme item's slot is empty on purpose: its label is a
// function of the theme rather than a constant, so menuLabel() supplies it.
constexpr const char* const kMenuLabels[core::kMenuItemCount] = {
    "Steps",     // kStepsMenuIndex
    "Sync",      // core::kSyncMenuIndex
    nullptr,     // core::kThemeMenuIndex — themeLabel()
    "Find phone",  // core::kFindPhoneMenuIndex
};
static_assert(kMenuLabels[kStepsMenuIndex] != nullptr, "the Steps item needs a label");
static_assert(kMenuLabels[core::kSyncMenuIndex] != nullptr, "the Sync item needs a label");
static_assert(kMenuLabels[core::kThemeMenuIndex] == nullptr,
              "the theme item's label is a function of the theme; menuLabel() supplies it");
static_assert(kMenuLabels[core::kFindPhoneMenuIndex] != nullptr, "the Find phone item needs a label");

// The theme item's label states what the watch is doing NOW, not what the press
// will do. "White on black" is today's default face, and pressing it leaves the
// pointer where it is with the label reading "Black on white" — so the wearer
// reads the current state straight off the menu instead of having to work out
// which way round a verb was meant.
const char* themeLabel(bool inverted) { return inverted ? "Black on white" : "White on black"; }

// The label for `index`, whatever it depends on. Out-of-range gives the empty
// string rather than reading past the array: menu_index has been through RTC
// memory and a reflash, and drawMenu() prints an empty row without complaint.
const char* menuLabel(uint8_t index, bool inverted) {
  if (index == core::kThemeMenuIndex) {
    return themeLabel(inverted);
  }
  if (index >= core::kMenuItemCount || kMenuLabels[index] == nullptr) {
    return "";
  }
  return kMenuLabels[index];
}

// What the search is doing, while it runs. Presentation, like syncLabel(). The
// ringing line names the mode the wearer chose, because Menu flips it (§4.1) and
// the line changing is how they see the press land.
const char* findPhaseLabel(core::FindPhase phase, bool sound) {
  switch (phase) {
    case core::FindPhase::Searching:
      return "searching";
    case core::FindPhase::Connected:
      return "connected";
    case core::FindPhase::Ringing:
      return sound ? "phone ringing" : "phone vibrating";
  }
  return "";
}

// How the last search ended, for the wakes after it. TimedOut and BackPressed
// leave the screen (core::applyFindOutcome), so their words are only ever shown
// if that rule is broken; they get honest ones rather than blanks.
const char* findOutcomeLabel(core::FindOutcome outcome) {
  switch (outcome) {
    case core::FindOutcome::InProgress:
      // Stored while a session runs and never shown then (find_live is true).
      // Seen off a live session only after a wake died inside a search.
      return "interrupted";
    case core::FindOutcome::BackPressed:
      return "stopped";
    case core::FindOutcome::TimedOut:
      return "no phone found";
    case core::FindOutcome::DismissedByPhone:
      return "phone found";
    case core::FindOutcome::RadioFailed:
      return "radio failed";
    case core::FindOutcome::BatteryTooLow:
      return "battery too low";
    case core::FindOutcome::NotAvailable:
      return "not available";
  }
  return "";
}

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

// Standalone now that it is not glued to the end of a percentage, so no leading
// space. Normal returns the empty string and draws nothing at all — the ordinary
// state of the watch should not spend pixels saying it is ordinary.
const char* modeTag(core::RunMode mode) {
  switch (mode) {
    case core::RunMode::Safe:
      return "SAFE";
    case core::RunMode::Recovery:
      return "RECOV";
    case core::RunMode::Normal:
      break;
  }
  return "";
}

}  // namespace

uint32_t compose(const Snapshot& snapshot) {
  g_composed.screen = snapshot.screen;
  g_composed.menu_index = snapshot.menu_index;
  g_composed.inverted = snapshot.inverted;

  if (snapshot.time_valid) {
    core::formatTime(g_composed.time, sizeof(g_composed.time), snapshot.time, snapshot.use_24h);
    core::formatDate(g_composed.date, sizeof(g_composed.date), snapshot.time);
  } else {
    // An invalid clock is shown as such rather than as a plausible wrong time.
    snprintf(g_composed.time, sizeof(g_composed.time), "--:--");
    snprintf(g_composed.date, sizeof(g_composed.date), "set time");
  }

  snprintf(g_composed.status, sizeof(g_composed.status), "%s", modeTag(snapshot.mode));
  g_composed.battery_percent = snapshot.battery_percent;
  g_composed.battery_warning = core::batterySaving(snapshot.battery_level);

  switch (snapshot.steps_display) {
    case core::StepsDisplay::Live:
      snprintf(g_composed.steps, sizeof(g_composed.steps), "%lu steps",
               static_cast<unsigned long>(snapshot.steps_today));
      snprintf(g_composed.steps_face, sizeof(g_composed.steps_face), "%lu",
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
      // The corner holds a number and nothing else, so it says there what the
      // clock says when the RTC is unset: dashes. A wrong number and a missing
      // one have to look different at arm's length.
      snprintf(g_composed.steps_face, sizeof(g_composed.steps_face), "--");
      snprintf(g_composed.steps_detail, sizeof(g_composed.steps_detail), "sensor stopped");
      break;

    case core::StepsDisplay::Hidden:
      g_composed.steps[0] = '\0';
      g_composed.steps_face[0] = '\0';
      g_composed.steps_detail[0] = '\0';
      break;
  }

  snprintf(g_composed.sync, sizeof(g_composed.sync), "%s",
           syncLabel(snapshot.sync_result, snapshot.sync_applied));

  if (snapshot.find_live) {
    snprintf(g_composed.find_status, sizeof(g_composed.find_status), "%s",
             findPhaseLabel(snapshot.find_phase, snapshot.find_sound));
    // Elapsed as m:ss and the attempt counter, the two numbers the wearer asked
    // to see while the search runs. Widest reachable: "2:00  try 255", 13 chars.
    snprintf(g_composed.find_progress, sizeof(g_composed.find_progress), "%u:%02u  try %u",
             static_cast<unsigned>(snapshot.find_elapsed_s / 60u),
             static_cast<unsigned>(snapshot.find_elapsed_s % 60u),
             static_cast<unsigned>(snapshot.find_attempts));
  } else {
    snprintf(g_composed.find_status, sizeof(g_composed.find_status), "%s",
             findOutcomeLabel(snapshot.find_outcome));
    g_composed.find_progress[0] = '\0';
  }

  // Hash exactly what will be visible — nothing more, or the screen redraws for
  // invisible changes; nothing less, or a real change is missed and the panel shows
  // stale pixels.
  uint32_t hash = core::contentHash(g_composed.time);
  hash = core::hashCombine(hash, g_composed.date);
  hash = core::hashCombine(hash, g_composed.status);
  // The gauge, not the percentage behind it: 34 pixels of track carry 35
  // pictures where the percentage carries 101, and a refresh spent on a change
  // that does not move a pixel is a refresh spent on nothing.
  const uint16_t battery_fill =
      core::gaugeFillPixels(g_composed.battery_percent, board::display::kBatteryTrackPixels);
  hash = core::hashCombine(hash, &battery_fill, sizeof(battery_fill));
  // Separately from the fill, and it has to be: the tracker's hysteresis puts the
  // same percentage — so the same fill — on either side of the threshold, so a
  // hash keyed on the fill alone would compose an identical screen for a watch
  // that had just entered saving mode and leave the mark unpainted.
  const uint8_t battery_warning_byte = g_composed.battery_warning ? 1u : 0u;
  hash = core::hashCombine(hash, &battery_warning_byte, sizeof(battery_warning_byte));
  hash = core::hashCombine(hash, g_composed.steps_face);
  // Outside every screen-specific branch below, because this one is not specific
  // to a screen: it swaps all 40 000 pixels of whichever screen is up, and of the
  // menu it also rewrites a label. Hashing the theme rather than the labels is
  // what covers that second effect — the only label that varies is the theme
  // item's, and it varies with exactly this byte.
  const uint8_t inverted_byte = g_composed.inverted ? 1u : 0u;
  hash = core::hashCombine(hash, &inverted_byte, sizeof(inverted_byte));
  hash = core::hashCombine(hash, &g_composed.screen, sizeof(g_composed.screen));
  if (g_composed.screen == core::Screen::Menu) {
    hash = core::hashCombine(hash, &g_composed.menu_index, sizeof(g_composed.menu_index));
  }
  if (g_composed.screen == core::Screen::App) {
    hash = core::hashCombine(hash, &g_composed.menu_index, sizeof(g_composed.menu_index));
    // The spelled-out count and its second line, which only this screen shows.
    // The watchface hashes steps_face above instead: the two always move
    // together, but each screen hashing the string it actually draws is what
    // keeps that from being something the next change has to remember.
    hash = core::hashCombine(hash, g_composed.steps);
    hash = core::hashCombine(hash, g_composed.steps_detail);
    // Only here. A sync result changing must repaint the Sync screen if it is up,
    // and must never repaint the watchface — that would be a ~0.003 mAh refresh
    // for something not on screen.
    hash = core::hashCombine(hash, g_composed.sync);
    // The Find phone screen's two lines, for the same reason. While a search runs
    // the elapsed time changes every round, so every round repaints — which is
    // what the wearer is watching for; when nothing else moved, it is the one
    // thing on the panel that did.
    hash = core::hashCombine(hash, g_composed.find_status);
    hash = core::hashCombine(hash, g_composed.find_progress);
  }
  return hash;
}

void draw() {
  switch (g_composed.screen) {
    case core::Screen::Menu: {
      // Assembled per draw rather than held in a table, because one of the
      // labels is a function of the theme. kMenuItemCount pointers of stack, no
      // allocation, on a path that only a button wake reaches.
      const char* items[core::kMenuItemCount];
      for (uint8_t i = 0; i < core::kMenuItemCount; ++i) {
        items[i] = menuLabel(i, g_composed.inverted);
      }
      board::display::drawMenu(items, core::kMenuItemCount, g_composed.menu_index);
      return;
    }

    case core::Screen::App: {
      board::display::drawStatusLine(g_composed.status);
      board::display::drawBatteryGauge(g_composed.battery_percent, g_composed.battery_warning);
      const uint8_t index =
          g_composed.menu_index < core::kMenuItemCount ? g_composed.menu_index : 0;
      if (index == kStepsMenuIndex && g_composed.steps[0] != '\0') {
        board::display::drawBanner(g_composed.steps, g_composed.steps_detail);
      } else if (index == core::kFindPhoneMenuIndex) {
        // Redrawn every few seconds while a search runs (main.cpp's find session
        // paints it directly), then left showing how it ended. The third line is
        // empty once the search is over, and drawBanner() leaves the row blank.
        board::display::drawBanner(menuLabel(index, g_composed.inverted), g_composed.find_status,
                                   g_composed.find_progress[0] != '\0' ? g_composed.find_progress
                                                                       : nullptr);
      } else if (index == core::kSyncMenuIndex) {
        // What the *previous* window achieved. This screen is painted before the
        // window this press opened has run — the radio comes up after the panel
        // is done with, so the wearer sees something immediately instead of
        // waiting six seconds for a refresh. The new outcome is on the screen the
        // next time it is opened; re-rendering it here would cost a second panel
        // refresh per sync, which is ~0.26 mAh/day at the hourly cadence and is
        // not in the budget.
        board::display::drawBanner(menuLabel(index, g_composed.inverted), g_composed.sync);
      } else {
        // kStepsMenuIndex, the only other item that opens a screen. An empty
        // string means the counter is not running at all, which is a different
        // thing from "no step data" (the sensor answered and the reading was
        // stale) and needs its own words.
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
  board::display::drawBatteryGauge(g_composed.battery_percent, g_composed.battery_warning);
  board::display::drawTimeLarge(g_composed.time);
  board::display::drawDateLine(g_composed.date);
  board::display::drawStepsLine(g_composed.steps_face);
}

}  // namespace app
