#include "core/battery_model.h"

#include <cstddef>

namespace core {
namespace {

struct CurvePoint {
  uint16_t millivolts;
  uint8_t percent;
};

// Flat in the middle and steep at both ends, which is how a single LiPo cell
// actually behaves. Interpolating a straight line from 3300 to 4200 instead would
// read ~50 % when the cell is nearly empty.
constexpr CurvePoint kCurve[] = {
    {3300, 0},  {3500, 5},  {3600, 10}, {3700, 25}, {3750, 40},
    {3800, 55}, {3900, 70}, {4000, 85}, {4100, 95}, {4200, 100},
};

constexpr size_t kCurveSize = sizeof(kCurve) / sizeof(kCurve[0]);

}  // namespace

uint8_t percentFromMillivolts(uint16_t millivolts) {
  if (millivolts <= kCurve[0].millivolts) {
    return 0;
  }
  if (millivolts >= kCurve[kCurveSize - 1].millivolts) {
    return 100;
  }
  for (size_t i = 1; i < kCurveSize; ++i) {
    if (millivolts <= kCurve[i].millivolts) {
      const uint32_t mv_span =
          static_cast<uint32_t>(kCurve[i].millivolts - kCurve[i - 1].millivolts);
      const uint32_t pct_span = static_cast<uint32_t>(kCurve[i].percent - kCurve[i - 1].percent);
      const uint32_t into = static_cast<uint32_t>(millivolts - kCurve[i - 1].millivolts);
      // Rounded rather than truncated, so the curve is symmetric about each
      // segment's midpoint.
      const uint32_t offset = (into * pct_span + mv_span / 2) / mv_span;
      return static_cast<uint8_t>(kCurve[i - 1].percent + offset);
    }
  }
  return 100;
}

void BatteryFilter::reset() {
  value_ = 0;
  primed_ = false;
}

uint16_t BatteryFilter::update(uint16_t millivolts) {
  if (!primed_) {
    value_ = millivolts;
    primed_ = true;
    return value_;
  }
  // Truncating rather than rounding is deliberate. Rounding to nearest leaves a
  // sticky ±2 mV offset that the filter never settles out of; truncation
  // converges exactly on a constant input, and its small downward bias during a
  // transient errs toward under-reporting charge, which is the safe direction for
  // a battery gauge.
  const uint32_t blended = static_cast<uint32_t>(value_) * 3u + millivolts;
  value_ = static_cast<uint16_t>(blended / 4u);
  return value_;
}

BatteryLevel BatteryLevelTracker::update(uint8_t percent) {
  switch (level_) {
    case BatteryLevel::Full:
      if (percent < kFullExitPercent) {
        level_ = BatteryLevel::Normal;
      }
      break;

    case BatteryLevel::Normal:
      if (percent >= kFullEnterPercent) {
        level_ = BatteryLevel::Full;
      } else if (percent <= kCriticalEnterPercent) {
        level_ = BatteryLevel::Critical;
      } else if (percent <= kLowEnterPercent) {
        level_ = BatteryLevel::Low;
      }
      break;

    case BatteryLevel::Low:
      if (percent <= kCriticalEnterPercent) {
        level_ = BatteryLevel::Critical;
      } else if (percent >= kLowExitPercent) {
        level_ = BatteryLevel::Normal;
      }
      break;

    case BatteryLevel::Critical:
      // Only leaves Critical once well clear of the entry threshold — a charger
      // being plugged in is exactly when the reading is noisiest.
      if (percent >= kCriticalExitPercent) {
        level_ = BatteryLevel::Low;
      }
      break;
  }
  return level_;
}

uint16_t tickIntervalSeconds(BatteryLevel level) {
  switch (level) {
    case BatteryLevel::Low:
    case BatteryLevel::Critical:
      return kSavingTickSeconds;
    case BatteryLevel::Normal:
    case BatteryLevel::Full:
      break;
  }
  return kNormalTickSeconds;
}

bool radioPermitted(BatteryLevel level) {
  return level == BatteryLevel::Normal || level == BatteryLevel::Full;
}

}  // namespace core
