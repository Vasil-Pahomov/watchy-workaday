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
//
// The Low pair is where saving mode begins, and it is deliberately late. It used
// to be 20/28, which on this curve is ~3.67 V — a cell with a third of its useful
// charge still in it, stretched to a five-minute face for hours before the wearer
// had any reason to want that. 10/15 is ~3.60 V and ~3.63 V: the knee of the
// discharge curve, where the remaining runtime is genuinely short enough that
// tripling it is worth a stale clock.
//
// Critical keeps 5/12 and is not a second saving mode — it has no behaviour of
// its own, only its own way out. Note that its exit sits *inside* the Low band
// (12 is above kLowEnterPercent): a cell climbing off the floor leaves Critical
// at 12 % and stays in saving mode until 15 %, which is the ladder working, not
// an inversion.
constexpr uint8_t kCriticalEnterPercent = 5;
constexpr uint8_t kCriticalExitPercent = 12;
constexpr uint8_t kLowEnterPercent = 10;
constexpr uint8_t kLowExitPercent = 15;
constexpr uint8_t kFullEnterPercent = 97;
constexpr uint8_t kFullExitPercent = 92;

// Tick intervals per level. Dropping to 5 minutes below 10 % cuts the wake count
// from 1440/day to 288/day, which roughly triples the remaining runtime — see
// docs/power-budget.md.
//
// The saving tick is also *aligned*: core::alignedTickMinutes() picks the count
// so the wake lands on a wall-clock minute divisible by five, which is why the
// face reads 14:35 and never 14:33. Both intervals divide 60, so the alignment
// holds across the hour boundary — and the hour boundary is where §5.1's sync
// window is due, so the tick that carries it exists in saving mode too.
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

// True when the watch may bring the radio up **on its own**.
//
// This used to read "regardless of what the user asked for", and that clause is
// gone on purpose. A scheduled window is the firmware spending the wearer's cell
// on a guess that a phone is nearby, and below 10 % that guess is not worth
// ~0.9 mAh/day. A Sync or Find phone press is not a guess: the wearer is standing
// there, and refusing them buys ~0.03 mAh at the cost of the one moment the
// feature exists for — a watch that cannot be found is not saving anybody
// anything. So this gates the schedule, and core::evaluateSyncWindow() applies it
// only when `user_requested` is false.
//
// Safe and Recovery are the opposite case and keep their unconditional refusal:
// those are faults, not a low cell, and a wearer cannot consent their way out of
// a watch that is already failing.
bool radioPermitted(BatteryLevel level);

// True when the two rules above have both bitten: the tick has stretched to five
// minutes and the watch has stopped syncing on its own. This is the watch behaving
// differently, and until now nothing on the panel said so — the wearer saw a watch
// whose clock had gone up to five minutes stale and which had quietly stopped
// meeting its phone, with no way to tell that from a fault.
//
// Note what it no longer means: the Sync and Find phone items still work here, so
// this mark is not "the radio is off". It is "the watch will not reach for the
// phone unless you tell it to".
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
