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

// An interrupt handler. Runs in ISR context: it must be IRAM_ATTR, must not
// block, and may only call the FromISR half of any FreeRTOS API.
using PressHandler = void (*)();

// Call `handler` on the press edge of `button`, for as long as this wake is
// awake. The only consumer is the find-phone session (PROTOCOL.md §4.1), which
// is the one path that stays awake long enough for a press to arrive as an edge
// rather than as an ext1 wake — and which must not poll the pin, because a poll
// loop is what Law 1 forbids and an event-group wait is what it has instead.
//
// Effects only: which button, and what the press means, are the caller's. The
// pin is re-routed to the digital GPIO matrix first, because ext1 wake leaves it
// muxed to the RTC domain where a digital interrupt would never fire.
void attachPressInterrupt(core::ButtonId button, PressHandler handler);

// The counterpart. Call before sleeping: deepSleep() re-arms the pin as an ext1
// wake source, and an interrupt left attached is a second reader of the same
// edge with nothing to deliver it to.
void detachPressInterrupt(core::ButtonId button);

}  // namespace buttons
}  // namespace board
