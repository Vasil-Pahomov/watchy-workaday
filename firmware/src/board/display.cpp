#include "board/display.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>

#include "board/board_v20.h"
#include "board/font_time.h"
#include "board/diag.h"
#include "core/battery_model.h"

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

// ── watchface layout ────────────────────────────────────────────────────────
// Baselines are absolute rather than stacked, so the order the helpers below are
// called in does not matter — only these numbers do, and they are here together
// so the whole face can be read at once.
//
//   mode tag  small  baseline  25   left, and only in Safe or Recovery
//   gauge            rows    8..27  right, a tenth of the screen tall
//   date      label  baseline  64   centred (42..71 with the descender of "Aug")
//   time      clock  baseline 140   centred (85..139; digits have no descenders)
//   steps     label  baseline 188   left corner (166..187), digits only
//
// The three gaps that fall out of those numbers are 15, 14 and 27 pixels. The
// last one is the odd one out on purpose: the step count is a corner label
// rather than the bottom of the stack, so it hangs off the edge instead of
// sitting an even distance below the clock.
//
// tools/preview_face.py parses these constants by name and paints the same
// pixels on the desktop, so a layout change can be looked at before it is
// flashed. Renaming one breaks the preview loudly instead of letting it drift.
constexpr int16_t kMargin = 8;
constexpr int16_t kStatusBaseline = 25;
constexpr int16_t kDateBaseline = 64;
constexpr int16_t kTimeBaseline = 140;
constexpr int16_t kStepsBaseline = 188;

// The gauge. Its height is the only number chosen outright — a tenth of the
// screen — and the rest is derived from it, so the shape survives that one
// number changing.
constexpr int16_t kGaugeHeight = kDisplayHeight / 10;
constexpr int16_t kGaugeBorder = 2;
constexpr int16_t kGaugeBodyWidth = 38;
constexpr int16_t kGaugeCapWidth = 4;
constexpr int16_t kGaugeCapHeight = kGaugeHeight / 2;
constexpr int16_t kGaugeTop = kMargin;
constexpr int16_t kGaugeLeft = kDisplayWidth - kMargin - kGaugeCapWidth - kGaugeBodyWidth;

// app/screens.cpp hashes the fill width against display.h's copy of this number.
// If the two ever disagreed the panel would skip a refresh the gauge needed,
// which on a wrist looks exactly like a frozen watch.
static_assert(kBatteryTrackPixels == kGaugeBodyWidth - 2 * kGaugeBorder,
              "kBatteryTrackPixels must be the gauge body less both borders");

// Centring needs the rendered width, which for a proportional face means asking
// GFX for it. x1 is the first glyph's left bearing and has to come back out, or
// every string sits that many pixels off centre.
void drawCentred(const GFXfont* font, const char* text, int16_t baseline) {
  g_display.setFont(font);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - static_cast<int16_t>(w)) / 2 - x1);
  g_display.setCursor(x, baseline);
  g_display.print(text);
}

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
  drawCentred(&WorkadayTime, text, kTimeBaseline);
}

void drawDateLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  drawCentred(&WorkadayLabel, text, kDateBaseline);
}

void drawStepsLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  // Left-aligned rather than centred: this is a corner label now, and a centred
  // number that slides sideways as it gains a digit reads as a wobble.
  g_display.setFont(&WorkadayLabel);
  g_display.setCursor(kMargin, kStepsBaseline);
  g_display.print(text);
}

void drawStatusLine(const char* text) {
  if (text == nullptr) {
    return;
  }
  g_display.setFont(&WorkadaySmall);
  g_display.setCursor(kMargin, kStatusBaseline);
  g_display.print(text);
}

void drawBatteryGauge(uint8_t percent) {
  // Body: a filled rectangle knocked back out, rather than drawRect in a loop.
  // The border is 2 px because a single pixel of white on black is the first
  // thing this panel loses to ghosting.
  g_display.fillRect(kGaugeLeft, kGaugeTop, kGaugeBodyWidth, kGaugeHeight, kInk);
  g_display.fillRect(kGaugeLeft + kGaugeBorder, kGaugeTop + kGaugeBorder, kBatteryTrackPixels,
                     kGaugeHeight - 2 * kGaugeBorder, kPaper);

  // The positive electrode: a shorter block hung off the right-hand end, which is
  // the half of the drawing that says "battery" rather than "progress bar".
  g_display.fillRect(kGaugeLeft + kGaugeBodyWidth,
                     kGaugeTop + (kGaugeHeight - kGaugeCapHeight) / 2, kGaugeCapWidth,
                     kGaugeCapHeight, kInk);

  // Charge fills from the left, so what shrinks as the cell drains is the ink on
  // the left and what grows is the empty stretch on the right.
  const uint16_t fill = core::gaugeFillPixels(percent, kBatteryTrackPixels);
  if (fill != 0) {
    g_display.fillRect(kGaugeLeft + kGaugeBorder, kGaugeTop + kGaugeBorder,
                       static_cast<int16_t>(fill), kGaugeHeight - 2 * kGaugeBorder, kInk);
  }
}

void drawMenu(const char* const* items, uint8_t count, uint8_t selected) {
  if (items == nullptr) {
    return;
  }
  g_display.setFont(&WorkadaySmall);
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
  g_display.setFont(&WorkadaySmall);
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
