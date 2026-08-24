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

// Framebuffer drawing helpers, so app code does not reach into GxEPD2 directly.
void clear();
void drawTimeLarge(const char* text);
void drawDateLine(const char* text);
void drawStepsLine(const char* text);
void drawStatusLine(const char* text);
void drawMenu(const char* const* items, uint8_t count, uint8_t selected);
void drawBanner(const char* line1, const char* line2);

}  // namespace display
}  // namespace board
