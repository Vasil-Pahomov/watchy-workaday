// PCF8563 real-time clock (Watchy v2.0). Requires an active i2c::Session.
//
// The periodic tick uses the PCF8563's countdown timer, not its minute alarm. The
// timer auto-reloads, so one write gives a repeating interrupt, and with the
// 1/60 Hz source the count is simply the interval in minutes — which covers all
// three intervals this firmware uses (1, 5 and 15 minutes) with no special cases.
// The minute alarm would need re-arming on every wake and cannot express a
// 5-minute period at all.
#pragma once

#include <stdint.h>

#include "core/time_model.h"

namespace board {
namespace rtc {

constexpr uint8_t kI2cAddress = 0x51;

struct ReadResult {
  core::DateTime time;
  bool transport_ok = false;   // the I2C exchange itself succeeded
  bool clock_integrity = false;  // the PCF8563's VL flag says the oscillator held
  bool valid = false;          // transport_ok && clock_integrity && isValid(time)
};

// Read the wall clock. Never trusts the chip: the VL (voltage-low) flag is checked
// — it is set whenever the oscillator has stopped, which means the time is fiction
// — and the decoded fields are then run through core::isValid(). A bogus time that
// reaches the alarm logic produces an alarm that never fires, which is an
// unrecoverable freeze.
ReadResult read();

bool write(const core::DateTime& time);

// Arm the repeating tick, `minutes` apart (clamped to 1..255).
//
// Clears the timer flag first. This is not cosmetic: with level-mode interrupts a
// stale flag leaves INT asserted, and since deep-sleep ext0 triggers on that level
// the watch would wake the instant it slept and never rest.
bool armTick(uint8_t minutes);

// Clear the timer/alarm flags, releasing the INT line. Must run before sleeping.
bool clearInterruptFlags();

}  // namespace rtc
}  // namespace board
