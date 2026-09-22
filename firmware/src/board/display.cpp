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

// The frame around the picture: the couple of millimetres of panel outside the
// 200x200 addressable pixels, and the one thing on the glass the framebuffer
// cannot reach. There is no RAM behind it. The controller drives it from the
// border waveform register (SSD1681 0x3C), which GxEPD2 hardcodes to 0x05 inside
// a private _InitDisplay() that cannot be reached or overridden.
//
// 0x05 means "follow LUT1", and this panel's OTP waveform groups its LUTs as
// LUT0 black->black, LUT1 black->white, LUT2 white->black, LUT3 white->white. So
// on every refresh the frame runs a transition that ENDS white and passes
// through black on the way — which is the black flash a full refresh shows, and
// why a white-on-black face has been sitting inside a white frame.
//
// The register is written before the update command, and refresh() runs between
// the two — so overriding refresh() is enough to have the last word without
// forking the library. refresh(bool) delegates to the rectangle overload for a
// partial, so one update can write the register twice; the write is idempotent
// and costs two bytes of SPI, which is cheaper than a flag saying it happened.
class Panel : public GxEPD2_154_D67 {
 public:
  using GxEPD2_154_D67::GxEPD2_154_D67;

  // The two LUTs whose end state is a colour worth ending on. Which of them is
  // in force belongs to the theme, not to this class — see Session.
  static constexpr uint8_t kBorderWhite = 0x05;  // follow LUT1, black->white
  static constexpr uint8_t kBorderBlack = 0x06;  // follow LUT2, white->black

  void setBorder(uint8_t waveform) { border_ = waveform; }

  void refresh(bool partial_update_mode = false) override {
    writeBorder();
    GxEPD2_154_D67::refresh(partial_update_mode);
  }

  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) override {
    writeBorder();
    GxEPD2_154_D67::refresh(x, y, w, h);
  }

 private:
  void writeBorder() {
    _writeCommand(0x3C);  // BorderWaveform
    _writeData(border_);
  }

  // The shipped default is the library's, so a Session that forgot to say leaves
  // the panel exactly as it behaved before this class existed.
  uint8_t border_ = kBorderWhite;
};

// GDEH0154D67 / GDEY0154D67, 200x200. A full-height page buffer is 5000 bytes,
// which this chip has room for, so paged drawing collapses to a single pass.
GxEPD2_BW<Panel, Panel::HEIGHT> g_display(
    Panel(kPinDisplayCs, kPinDisplayDc, kPinDisplayReset, kPinDisplayBusy));

// "Ink" and "paper" are roles, and every site below uses them rather than a
// colour literal — which is what made inverting the watch this one pair and not
// five scattered edits that could drift apart.
//
// Runtime rather than constexpr since the wearer chooses (core::kThemeMenuIndex).
// The defaults are the shipped face, white on black; Session's constructor sets
// the pair for the session before the panel is initialised, so no draw can run
// against a stale one. No policy: board/ is told, it does not decide.
//
// Cost, checked rather than assumed: an e-paper refresh drives the whole window
// through a fixed waveform, so painting mostly black costs the same as mostly
// white — the panel's energy is in the transition, not the ink. What does change
// is ghosting: residue from the previous frame reads more strongly against black,
// so if the face starts looking smudged, the answer is core::refresh_policy's full
// -refresh cadence, not this pair. The swap itself is the extreme case of that —
// every pixel transitions at once — which is why core::ThemeChange::changed makes
// the wake it happens on a full refresh.
uint16_t g_paper = GxEPD_BLACK;
uint16_t g_ink = GxEPD_WHITE;

bool g_initialised = false;

// ── watchface layout ────────────────────────────────────────────────────────
// Baselines are absolute rather than stacked, so the order the helpers below are
// called in does not matter — only these numbers do, and they are here together
// so the whole face can be read at once.
//
//   mode tag  small  baseline  25   left at kMargin, and only in Safe or Recovery
//   gauge            rows    0..19  hard into the top-right corner
//   low mark  small  measured       "!" beside the gauge, and only in saving mode
//   date      label  baseline  64   centred (42..69 with the descender of "Aug")
//   time      clock  baseline 140   centred (86..140 at its widest, "20:00")
//   steps     label  measured       into the bottom-left corner, with some padding from the botton, digits only
//
// Two of the five are pinned to the panel rather than placed on it: the gauge
// and the step count are corner readings, and a corner reading that floats a
// margin inside the corner reads as a mistake at arm's length. So they sit on
// column 0 and column 199, on row 0 and row 199, with nothing between them and
// the glass. kMargin survives for the mode tag alone, which is the one thing up
// there that is text rather than a reading.
//
// The step count has no baseline constant for the same reason: which baseline
// puts a digit on row 199 is a property of the glyphs, and the glyphs are
// regenerated by tools/make_time_font.py. drawStepsLine() measures it rather
// than carrying a number that a regeneration could silently move a pixel.
//
// tools/preview_face.py parses these constants by name and paints the same
// pixels on the desktop, so a layout change can be looked at before it is
// flashed. Renaming one breaks the preview loudly instead of letting it drift.
constexpr int16_t kMargin = 8;
constexpr int16_t kStatusBaseline = 25;
constexpr int16_t kDateBaseline = 64;
constexpr int16_t kTimeBaseline = 140;
constexpr int16_t kStepsLeft = 0;
constexpr int16_t kStepsBottom = kDisplayHeight - kMargin;

// The gauge. Its height is the only number chosen outright — a tenth of the
// screen — and the rest is derived from it, so the shape survives that one
// number changing.
constexpr int16_t kGaugeHeight = kDisplayHeight / 10;
constexpr int16_t kGaugeBorder = 2;
constexpr int16_t kGaugeBodyWidth = 38;
constexpr int16_t kGaugeCapWidth = 4;
constexpr int16_t kGaugeCapHeight = kGaugeHeight / 2;
constexpr int16_t kGaugeTop = 0;
constexpr int16_t kGaugeLeft = kDisplayWidth - kGaugeCapWidth - kGaugeBodyWidth;

// Clear columns between the low-battery mark and the cell's left wall. The mark
// is placed from this edge leftwards rather than from a left-hand column of its
// own, so it stays the same distance from the cell whatever width the glyph is
// regenerated at — the one thing the wearer would notice if it drifted is the
// mark crowding or leaving the gauge. Six columns is three times the gauge's own
// border, which is enough for the two to read as separate readings rather than
// as one wider drawing.
constexpr int16_t kGaugeWarnGap = 6;

// Measured and then drawn, so the two have to be the same string.
constexpr const char* kWarnMark = "!";

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

Session::Session(bool first_boot, bool inverted) {
  g_paper = inverted ? GxEPD_WHITE : GxEPD_BLACK;
  g_ink = inverted ? GxEPD_BLACK : GxEPD_WHITE;

  // The frame follows the paper, for the same reason and from the same argument:
  // it is the outermost thing the wearer sees, and a white frame around a black
  // face reads as a bezel that got left behind. Set here rather than inside
  // Panel so board/ still decides nothing — it is told which way round, once,
  // and the frame cannot end up disagreeing with the pixels it surrounds.
  g_display.epd2.setBorder(inverted ? Panel::kBorderWhite : Panel::kBorderBlack);

  // Serial diag bitrate 0 keeps GxEPD2 from opening its own UART.
  g_display.init(0, first_boot, 2, false);
  g_display.setRotation(0);
  g_display.setTextColor(g_ink);
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
    g_display.fillScreen(g_paper);
    draw();
  } while (g_display.nextPage());
}

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
  if (text == nullptr || text[0] == '\0') {
    // getTextBounds() on an empty string underflows its width, and the answer
    // would be thrown away anyway: there is nothing to place.
    return;
  }
  // Left-aligned rather than centred: this is a corner reading, and a centred
  // number that slides sideways as it gains a digit reads as a wobble.
  g_display.setFont(&WorkadayLabel);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;

  // Horizontally, the string's own left bearing comes out, so its first inked
  // column lands on column 0 whatever it starts with.
  g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>(kStepsLeft - x1);

  // Vertically, a digit's bearing rather than this string's. The baseline that
  // puts a digit's last inked row on row 199 is the one this slot keeps in every
  // state - so "--" hangs where a dash hangs instead of being dragged down to
  // the bottom row, and the count does not jump between the two.
  g_display.getTextBounds("0", 0, 0, &x1, &y1, &w, &h);
  const int16_t baseline =
      static_cast<int16_t>(kStepsBottom - (y1 + static_cast<int16_t>(h) - 1));

  g_display.setCursor(x, baseline);
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

void drawBatteryGauge(uint8_t percent, bool warning) {
  if (warning) {
    // A glyph rather than a pair of rectangles like the cell beside it. The mark
    // is the only thing up here that is a character, the small face already
    // carries it (0x21 is inside the generated 0x20..0x7A), and a hand-built
    // one would be the single piece of lettering on the panel not set in Gilroy.
    //
    // Both offsets are measured rather than constants, exactly as drawStepsLine()
    // measures its own: which rows a 15 px mark has to sit on to be centred in a
    // 20 px band is a property of the glyph, and the glyphs are regenerated by
    // tools/make_time_font.py. Measuring means a regeneration moves the mark with
    // the font instead of leaving a stale number here to fall out of centre.
    g_display.setFont(&WorkadaySmall);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    g_display.getTextBounds(kWarnMark, 0, 0, &x1, &y1, &w, &h);
    // Right-aligned against the cell, so the gap is the fixed thing; centred in
    // the cell's own band, so the two line up whatever height either becomes.
    const int16_t x = static_cast<int16_t>(kGaugeLeft - kGaugeWarnGap -
                                           (x1 + static_cast<int16_t>(w)));
    const int16_t baseline =
        static_cast<int16_t>(kGaugeTop + (kGaugeHeight - static_cast<int16_t>(h)) / 2 - y1);
    g_display.setCursor(x, baseline);
    g_display.print(kWarnMark);
  }

  // Body: a filled rectangle knocked back out, rather than drawRect in a loop.
  // The border is 2 px because a single pixel of white on black is the first
  // thing this panel loses to ghosting.
  g_display.fillRect(kGaugeLeft, kGaugeTop, kGaugeBodyWidth, kGaugeHeight, g_ink);
  g_display.fillRect(kGaugeLeft + kGaugeBorder, kGaugeTop + kGaugeBorder, kBatteryTrackPixels,
                     kGaugeHeight - 2 * kGaugeBorder, g_paper);

  // The positive electrode: a shorter block hung off the right-hand end, which is
  // the half of the drawing that says "battery" rather than "progress bar".
  g_display.fillRect(kGaugeLeft + kGaugeBodyWidth,
                     kGaugeTop + (kGaugeHeight - kGaugeCapHeight) / 2, kGaugeCapWidth,
                     kGaugeCapHeight, g_ink);

  // Charge fills from the left, so what shrinks as the cell drains is the ink on
  // the left and what grows is the empty stretch on the right.
  const uint16_t fill = core::gaugeFillPixels(percent, kBatteryTrackPixels);
  if (fill != 0) {
    g_display.fillRect(kGaugeLeft + kGaugeBorder, kGaugeTop + kGaugeBorder,
                       static_cast<int16_t>(fill), kGaugeHeight - 2 * kGaugeBorder, g_ink);
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
      g_display.fillRect(0, y, kDisplayWidth, kRowHeight, g_ink);
      g_display.setTextColor(g_paper);
    } else {
      g_display.setTextColor(g_ink);
    }
    g_display.setCursor(12, static_cast<int16_t>(y + 21));
    g_display.print(items[i]);
  }
  g_display.setTextColor(g_ink);
}

void drawBanner(const char* line1, const char* line2, const char* line3) {
  g_display.setFont(&WorkadaySmall);
  // Three baselines 25 px apart, with the small face's 22 px yAdvance: rows that
  // neither touch nor drift. tools/preview_face.py draws the same three numbers.
  if (line1 != nullptr) {
    g_display.setCursor(10, 95);
    g_display.print(line1);
  }
  if (line2 != nullptr) {
    g_display.setCursor(10, 120);
    g_display.print(line2);
  }
  if (line3 != nullptr) {
    g_display.setCursor(10, 145);
    g_display.print(line3);
  }
}

}  // namespace display
}  // namespace board
