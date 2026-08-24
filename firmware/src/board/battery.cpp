#include "board/battery.h"

#include <Arduino.h>

#include "board/board_v20.h"
#include "board/diag.h"

namespace board {
namespace battery {
namespace {

// A LiPo outside this window means a broken divider, a disconnected cell, or a
// mis-sampled ADC — not a real reading. Returning 0 lets the caller keep the last
// good filtered value instead of showing a nonsense percentage.
constexpr uint16_t kPlausibleMinMv = 2500;
constexpr uint16_t kPlausibleMaxMv = 4500;

constexpr uint8_t kSampleCount = 4;

}  // namespace

uint16_t readMillivolts() {
  // 11 dB attenuation covers the full ~0..2.5 V the divider can present.
  analogSetPinAttenuation(kPinBatteryAdc, ADC_11db);

  // A few conversions averaged: the panel refresh sags the rail by tens of
  // millivolts and a single sample lands wherever that happens to be. Four
  // one-shot reads cost microseconds, unlike a delay-based settle.
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSampleCount; ++i) {
    total += analogReadMilliVolts(kPinBatteryAdc);
  }
  const uint32_t at_pin = total / kSampleCount;
  const uint32_t cell = at_pin * 2u;  // board divider is 2.0x

  if (cell < kPlausibleMinMv || cell > kPlausibleMaxMv) {
    WD_LOG("battery: implausible reading %umV (pin %umV)", static_cast<unsigned>(cell),
           static_cast<unsigned>(at_pin));
    return 0;
  }
  return static_cast<uint16_t>(cell);
}

}  // namespace battery
}  // namespace board
