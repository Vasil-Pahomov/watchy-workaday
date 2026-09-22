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

// Width in pixels of the filled part of a battery gauge whose track is
// `track_pixels` wide, at `percent` charge.
//
// Here rather than beside the drawing code because it is the only decision in
// painting the gauge — where the ink stops — and because it is what the
// watchface hashes. A 34 px track has 35 distinct pictures and the percentage
// has 101, so hashing the percentage would repaint the panel for a change no
// wearer can see, which Law 1 pays for in refreshes.
uint16_t gaugeFillPixels(uint8_t percent, uint16_t track_pixels);

// True when the radio must stay off regardless of what the user asked for.
bool radioPermitted(BatteryLevel level);

// True when the two rules above have both bitten: the tick has stretched to five
// minutes and the radio is refused. This is the watch behaving differently, and
// until now nothing on the panel said so — the wearer saw a watch that had gone
// quiet, took up to five minutes to show a button press, and turned down a sync,
// with no way to tell that from a fault.
//
// The gauge cannot carry it. The gauge draws a percentage, and which side of the
// hysteresis the tracker sits on is not a function of the percentage: 24 % is
// Normal on the way down and Low on the way back up, and both paint the same
// eight pixels of ink. So the mark beside it is a second reading, not a louder
// version of the first one.
//
// Here rather than in app/ because it is the same decision `tickIntervalSeconds`
// and `radioPermitted` make, phrased for the wearer — a test holds all three to
// the same answer, so a level that later stretches the tick without lighting the
// mark fails the gate instead of shipping.
bool batterySaving(BatteryLevel level);

}  // namespace core
