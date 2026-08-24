#include "board/display.h"

#include <Arduino.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <GxEPD2_BW.h>

#include "board/board_v20.h"
#include "board/font_time.h"
#include "board/diag.h"

namespace board {
namespace display {
namespace {

// GDEH0154D67 / GDEY0154D67, 200x200. A full-height page buffer is 5000 bytes,
// which this chip has room for, so paged drawing collapses to a single pass.
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_display(
    GxEPD2_154_D67(kPinDisplayCs, kPinDisplayDc, kPinDisplayReset, kPinDisplayBusy));

// The face is white-on-black. Named because "ink" and "paper" are roles: every
// site below uses them rather than a colour literal, so inverting the watch is
// this one pair and not five scattered edits that can drift apart.
//
// Cost, checked rather than assumed: an e-paper refresh drives the whole window
// through a fixed waveform, so painting mostly black costs the same as mostly
// white — the panel's energy is in the transition, not the ink. What does change
// is ghosting: residue from the previous frame reads more strongly against black,
// so if the face starts looking smudged, the answer is core::refresh_policy's full
// -refresh cadence, not this pair.
constexpr uint16_t kPaper = GxEPD_BLACK;
constexpr uint16_t kInk = GxEPD_WHITE;

bool g_initialised = false;

}  // namespace

Session::Session(bool first_boot) {
  // Serial diag bitrate 0 keeps GxEPD2 from opening its own UART.
  g_display.init(0, first_boot, 2, false);
  g_display.setRotation(0);
  g_display.setTextColor(kInk);
  g_display.setTextWrap(false);
  g_initialised = true;
}

Session::~Session() {
  // The whole reason this is a destructor.
  g_display.hibernate();
  g_initialised = false;
}

void render(core::RefreshKind kind, DrawFn draw) {
  if (!g_initialised || draw == nullptr || kind == core::RefreshKind::Skip) {
    return;
  }

  if (kind == core::RefreshKind::Full) {
    g_display.setFullWindow();
  } else {
    g_display.setPartialWindow(0, 0, kDisplayWidth, kDisplayHeight);
  }

  g_display.firstPage();
  do {
    g_display.fillScreen(kPaper);
    draw();
  } while (g_display.nextPage());
}

void clear() { g_display.fillScreen(kPaper); }

void drawTimeLarge(const char* text) {
  if (text == nullptr) {
    return;
  }
  g_display.setFont(&WorkadayTime);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - static_cast<int16_t>(w)) / 2 - x1);
  g_display.setCursor(x, 128);
  g_display.print(text);
}

// The face reads date / time / steps top to bottom. Baselines are absolute rather
// than stacked, so the order these are called in does not matter — only these
// three numbers do, and they are here together so the layout can be read at once.
//
//   status   9pt   baseline  22   (~9..22)
//   date     label baseline  60   (~41..67 with the descender of "Aug")
//   time     clock baseline 128   (80..128, no descenders in digits)
//   steps    label baseline 174   (155..181)
void drawDateLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  g_display.setFont(&WorkadayLabel);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - static_cast<int16_t>(w)) / 2 - x1);
  g_display.setCursor(x, 60);
  g_display.print(text);
}

void drawStepsLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  g_display.setFont(&WorkadayLabel);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - static_cast<int16_t>(w)) / 2 - x1);
  g_display.setCursor(x, 174);
  g_display.print(text);
}

void drawStatusLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  g_display.setFont(&FreeMonoBold9pt7b);
  g_display.setCursor(8, 22);
  g_display.print(text);
}

void drawMenu(const char* const* items, uint8_t count, uint8_t selected) {
  if (items == nullptr) {
    return;
  }
  g_display.setFont(&FreeMonoBold9pt7b);
  constexpr int16_t kRowHeight = 30;
  constexpr int16_t kTop = 20;

  for (uint8_t i = 0; i < count; ++i) {
    if (items[i] == nullptr) {
      continue;
    }
    const int16_t y = static_cast<int16_t>(kTop + i * kRowHeight);
    if (i == selected) {
      g_display.fillRect(0, y, kDisplayWidth, kRowHeight, kInk);
      g_display.setTextColor(kPaper);
    } else {
      g_display.setTextColor(kInk);
    }
    g_display.setCursor(12, static_cast<int16_t>(y + 21));
    g_display.print(items[i]);
  }
  g_display.setTextColor(kInk);
}

void drawBanner(const char* line1, const char* line2) {
  g_display.setFont(&FreeMonoBold9pt7b);
  if (line1 != nullptr) {
    g_display.setCursor(10, 95);
    g_display.print(line1);
  }
  if (line2 != nullptr) {
    g_display.setCursor(10, 120);
    g_display.print(line2);
  }
}

}  // namespace display
}  // namespace board
