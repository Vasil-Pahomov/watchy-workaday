// The 200x200 e-paper panel, via GxEPD2.
//
// Panel lifetime is an RAII guard for the same reason the I2C bus is: the panel
// must be hibernated before sleep on EVERY path, including the one where the
// refresh was skipped and the error path where the draw failed. A missed
// hibernate() leaves the charge pump running for the whole sleep window, which
// dwarfs the ~60 uA sleep floor. A destructor cannot be skipped by an early return;
// a call at the end of a function can.
#pragma once

#include <stdint.h>

#include "core/refresh_policy.h"

namespace board {
namespace display {

class Session {
 public:
  // `first_boot` performs the panel's initial full reset; on a deep-sleep wake it
  // must be false or every wake pays for a full re-initialisation.
  explicit Session(bool first_boot);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;
};

// Draw callback: paints into the framebuffer. Called once per page; with a
// full-height buffer on this panel that means exactly once.
using DrawFn = void (*)();

// Push the framebuffer to the panel. `kind` must not be Skip — the caller decides
// that, and a Skip means this function is never entered at all, which is the point.
void render(core::RefreshKind kind, DrawFn draw);

// Pixels the battery gauge's fill can occupy — the body less both borders.
// Published because app/screens.cpp hashes core::gaugeFillPixels() against this
// number rather than the percentage behind it: what the refresh policy has to
// compare is the picture, and 34 pixels carry fewer states than 101 percentages.
// display.cpp static_asserts that its own geometry still agrees with this.
constexpr uint16_t kBatteryTrackPixels = 34;

// Framebuffer drawing helpers, so app code does not reach into GxEPD2 directly.
void clear();
void drawTimeLarge(const char* text);
void drawDateLine(const char* text);
// The step count, bottom-left. The digits only — the word "steps" is on the
// Steps screen, where there is room to say it.
void drawStepsLine(const char* text);
void drawStatusLine(const char* text);
// The charge, top-right: a horizontal cell with its electrode on the right,
// filled from the left in proportion to `percent`.
void drawBatteryGauge(uint8_t percent);
void drawMenu(const char* const* items, uint8_t count, uint8_t selected);
void drawBanner(const char* line1, const char* line2);

}  // namespace display
}  // namespace board
