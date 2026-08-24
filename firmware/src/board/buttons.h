// The four buttons. ACTIVE HIGH on Watchy v2.0 — pressed reads HIGH.
//
// GPIO 35 (Up) is input-only with no internal pull resistors; it works only
// because the board provides an external one. Never configure these with
// INPUT_PULLUP: on 35 it silently does nothing, which is worse than failing.
#pragma once

#include "core/ui_state.h"

namespace board {
namespace buttons {

void configure();

bool isPressed(core::ButtonId button);

// Whichever button is held right now, or None. Used to re-read the button after an
// ext1 wake; prefer power::wakeButton() for the wake itself, since by the time
// setup() runs the user may already have let go.
core::ButtonId pressed();

}  // namespace buttons
}  // namespace board
