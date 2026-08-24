// Watchy v2.0 pin map — the single source of truth.
//
// Values verified against upstream sqfmi/Watchy src/config.h (ARDUINO_WATCHY_V20
// branch). See docs/hardware-v2.0.md for the traps; the short version:
//
//   * Buttons are ACTIVE HIGH, so ext1 wake is ANY_HIGH.
//   * The RTC interrupt is ACTIVE LOW, so it uses ext0 at trigger level 0.
//   * GPIO 34 (battery ADC) and 35 (Up button) are input-only with NO internal
//     pull resistors. INPUT_PULLUP on them silently does nothing.
//   * v1.5 puts Up on 32 and the ADC on 35. Do not copy v1.5 code.
#pragma once

#include <stdint.h>

#if !defined(ARDUINO_WATCHY_V20)
#error "Workaday targets SQFMI Watchy v2.0 only. Build with -D ARDUINO_WATCHY_V20 \
(the PlatformIO `watchy` board defines only ARDUINO_WATCHY, which is not \
revision-specific, so platformio.ini pins it). See CLAUDE.md Law 5."
#endif

namespace board {

// ── I2C: PCF8563 RTC + BMA423 accelerometer share the bus ───────────────────
constexpr int kPinSda = 21;
constexpr int kPinScl = 22;
constexpr uint32_t kI2cFrequencyHz = 100000;
constexpr uint16_t kI2cTimeoutMs = 50;

// ── Buttons: ACTIVE HIGH ────────────────────────────────────────────────────
constexpr int kPinButtonMenu = 26;
constexpr int kPinButtonBack = 25;
constexpr int kPinButtonUp = 35;  // input-only, external pull-up on the board
constexpr int kPinButtonDown = 4;

// ── Battery ─────────────────────────────────────────────────────────────────
// Input-only. The board halves the cell voltage before the ADC sees it, so a
// reading must be doubled — applied as integer math in board/battery.cpp, which is
// the single place that conversion lives.
constexpr int kPinBatteryAdc = 34;

// ── PCF8563: ACTIVE LOW interrupt ───────────────────────────────────────────
constexpr int kPinRtcInterrupt = 27;

// ── BMA423 ──────────────────────────────────────────────────────────────────
constexpr int kPinAccelInt1 = 14;
constexpr int kPinAccelInt2 = 12;

// ── Vibration motor ─────────────────────────────────────────────────────────
constexpr int kPinVibrationMotor = 13;

// ── Display: 200x200 e-paper ────────────────────────────────────────────────
constexpr int kPinDisplayCs = 5;
constexpr int kPinDisplayDc = 10;
constexpr int kPinDisplayReset = 9;
constexpr int kPinDisplayBusy = 19;
constexpr int16_t kDisplayWidth = 200;
constexpr int16_t kDisplayHeight = 200;

// ── Deep-sleep wake masks ───────────────────────────────────────────────────
// ext1 cannot mix trigger polarities, which is why the active-low RTC line gets
// ext0 to itself and the four active-high buttons share ext1.
constexpr uint64_t kButtonWakeMask = (1ULL << kPinButtonMenu) | (1ULL << kPinButtonBack) |
                                     (1ULL << kPinButtonUp) | (1ULL << kPinButtonDown);
constexpr uint64_t kAccelWakeMask = (1ULL << kPinAccelInt1);

}  // namespace board
