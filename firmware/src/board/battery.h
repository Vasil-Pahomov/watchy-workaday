// Battery voltage via the ADC on GPIO 34.
#pragma once

#include <stdint.h>

namespace board {
namespace battery {

// Cell voltage in millivolts, or 0 if the reading is implausible.
//
// Uses analogReadMilliVolts(), which applies the per-chip eFuse calibration — raw
// analogRead() on the ESP32 is materially non-linear and would skew the whole
// discharge curve. The board's 2.0x divider is applied here.
//
// There is deliberately no RAII power guard: the SAR ADC is not left biased between
// one-shot conversions, the driver powers it per conversion, so there is nothing to
// release. A guard here would imply protection that does not exist.
uint16_t readMillivolts();

}  // namespace battery
}  // namespace board
