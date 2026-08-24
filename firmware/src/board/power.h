// Sleep, wake-source detection, the watchdog, and GPIO quiescing.
//
// Deep sleep is the control flow of this firmware, not an optimisation: deepSleep()
// does not return, and every path out of setup() must reach it.
#pragma once

#include <stdint.h>

#include "core/health.h"
#include "core/ui_state.h"  // core::ButtonId
#include "core/wake_router.h"

namespace board {
namespace power {

// Comfortably longer than a full e-paper refresh (~2 s) so a legitimate slow
// refresh is not mistaken for a hang, but short enough that a real lockup is
// resolved inside one tick.
constexpr uint32_t kWatchdogTimeoutSeconds = 10;

// Release the pad holds that deepSleep() applied on the way down. **Call this
// first in setup(), before startWatchdog() and before WD_DIAG_BEGIN().**
//
// deepSleep() latches every pad so nothing floats or stops driving while the chip
// is asleep. That latch is not scoped to the sleep: it survives a core reset, a
// watchdog reset and a brownout, and only a power-on reset clears it. Without this
// call the first sleep freezes the pads for good — including U0TXD and U0RXD, so
// there is no serial output and the ROM download loader cannot answer esptool. On
// a board with no power switch that means opening the case to recover.
//
// Before WD_DIAG_BEGIN() specifically: Serial.begin() on a held pad reports success
// and emits nothing, so a release after it would leave the diagnostics silent and
// the fault invisible.
void releaseSleepHold();

// Arm the task watchdog. Call this immediately after releaseSleepHold(), before
// anything that can wedge. A reset from here is a fault the health module counts
// and escalates.
void startWatchdog();

// Feed the watchdog. Only ever call this after real forward progress — never from
// inside a wait loop, which would defeat the watchdog entirely.
void feedWatchdog();

// Why the chip came up. Reads esp_reset_reason(), so it distinguishes a panic or a
// watchdog reset from an ordinary deep-sleep wake.
core::ResetReason resetReason();

// What woke us. Uses the ext1 status bitmask rather than assuming, so a button and
// an accelerometer interrupt sharing ext1 stay distinguishable.
core::WakeSource wakeSource();

// Which button caused an ext1 wake, or None if the wake was something else.
// Read early: the user may already have released the button.
core::ButtonId wakeButton();

// Enter deep sleep. Does not return.
//
// Configures ext0 (RTC alarm, active low), ext1 (buttons, active high), and a
// timer backstop at `tick_seconds` plus a margin. The backstop is what stops a
// missed or misconfigured RTC alarm from becoming a permanent freeze — without it,
// one bad alarm write bricks the watch until someone opens the case.
//
// `accel_wake` adds the BMA423 interrupt to the ext1 mask. It costs sleep current,
// so it stays off unless a motion feature is actually enabled.
[[noreturn]] void deepSleep(uint16_t tick_seconds, bool accel_wake);

}  // namespace power
}  // namespace board
