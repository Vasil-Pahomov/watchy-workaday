// Battery interpretation: millivolts -> percentage -> level -> tick rate.
// Pure logic — no ADC here, that lives in board/battery.
//
// Two jobs beyond the obvious conversion:
//
//  * Smoothing. A single ADC sample moves several tens of millivolts with load
//    (the panel refresh alone sags the rail), which on a raw reading makes the
//    displayed percentage jitter and, worse, makes the level cross thresholds at
//    random.
//  * Hysteresis. Without it a battery sitting on a threshold flaps between
//    Normal and Low every minute, and each flap changes the tick interval, which
//    changes the load, which moves the voltage. Hysteresis breaks that loop.
#pragma once

#include <cstdint>

namespace core {

enum class BatteryLevel : uint8_t { Critical, Low, Normal, Full };

constexpr uint16_t kBatteryEmptyMv = 3300;
constexpr uint16_t kBatteryFullMv = 4200;

// Falling thresholds and the (higher) percentages needed to climb back out.
constexpr uint8_t kCriticalEnterPercent = 5;
constexpr uint8_t kCriticalExitPercent = 12;
constexpr uint8_t kLowEnterPercent = 20;
constexpr uint8_t kLowExitPercent = 28;
constexpr uint8_t kFullEnterPercent = 97;
constexpr uint8_t kFullExitPercent = 92;

// Tick intervals per level. Dropping to 5 minutes below 20 % cuts the wake count
// from 1440/day to 288/day, which roughly triples the remaining runtime — see
// docs/power-budget.md.
constexpr uint16_t kNormalTickSeconds = 60;
constexpr uint16_t kSavingTickSeconds = 300;

// Piecewise-linear LiPo discharge curve. Monotonic non-decreasing, clamped to
// 0..100 outside the table.
uint8_t percentFromMillivolts(uint16_t millivolts);

// Integer exponential moving average, alpha = 1/4. The first sample primes the
// filter directly instead of ramping up from zero, which would otherwise show a
// flat battery for the first few minutes after a reset.
class BatteryFilter {
 public:
  void reset();
  uint16_t update(uint16_t millivolts);
  uint16_t value() const { return value_; }
  bool primed() const { return primed_; }

 private:
  uint16_t value_ = 0;
  bool primed_ = false;
};

class BatteryLevelTracker {
 public:
  BatteryLevel update(uint8_t percent);
  BatteryLevel level() const { return level_; }

 private:
  BatteryLevel level_ = BatteryLevel::Normal;
};

uint16_t tickIntervalSeconds(BatteryLevel level);

// True when the radio must stay off regardless of what the user asked for.
bool radioPermitted(BatteryLevel level);

}  // namespace core
