// Daily step accounting on top of the BMA423's own hardware counter. Pure logic.
//
// The chip counts steps autonomously and hands us a free-running 32-bit total. Our
// job is everything that total is not:
//
//   * It never resets on its own, so "steps today" is a difference we accumulate.
//   * It DOES reset to zero whenever the sensor is reconfigured or loses power,
//     which looks like the counter going backwards.
//   * It knows nothing about midnight, or about the watch rebooting mid-day.
//   * A corrupted I2C read produces a plausible-looking huge number.
//
// Handling all of that here rather than in board/ is what makes it testable: every
// one of those cases is a unit test below, and none of them needs a wrist.
//
// ── What the day does while readings are stale ───────────────────────────────
//
// Nothing. The rollover lives inside updateSteps(), which only runs when a reading
// arrives, so an outage spanning midnight leaves `day_epoch` on the old day until
// the sensor answers again. That is deliberate, not an oversight:
//
//   * Nothing is displayed during the outage — stepsDisplayFor() reports Stopped —
//     so maintaining `today`/`yesterday` through it would change nothing visible.
//   * The first reading after recovery still rolls the day before any new steps
//     are credited to it (the roll runs whether or not the delta was plausible),
//     so a recovered sensor does not resume counting into a day that ended hours
//     ago. `today` is correct from the moment readings return.
//   * The residue is that `yesterday` briefly shows the total from whatever day
//     the outage began — the same "a late rollover beats a wrong date" trade this
//     module already makes for an untrustworthy clock, in a field that only
//     appears on the steps detail screen.
#pragma once

#include <cstdint>

#include "core/time_model.h"

namespace core {

// Plausibility limit for one reading. A sprint is roughly 4 steps/s, so ~240 a
// minute; 250 leaves headroom without accepting nonsense.
//
// The limit scales with the interval rather than being a flat number, because the
// interval is not fixed: it is 1 minute normally, 5 on a low battery, and longer
// still after a crash. A flat cap sized for one minute would silently discard real
// steps on the longer intervals, and a flat cap sized for the longest would let a
// garbled reading through on the short one.
constexpr uint32_t kMaxStepsPerMinute = 250;

// Beyond an hour the interval stops being informative — the watch was off, or the
// clock jumped — so the cap stops growing rather than becoming no cap at all.
constexpr uint32_t kMaxCreditedGapMinutes = 60;

// How many readings in a row may be rejected before the baseline itself is
// treated as the thing that is wrong.
//
// Rejecting a reading holds the baseline, which is right for one garbled I2C
// transfer. But the chip's counter only ever increases, so if a gap was *real* —
// the sensor kept counting while we were not reading it, past the ceiling of
// 60 x 250 = 15 000 steps — then the difference only grows, every subsequent
// reading is rejected too, and `today` never advances again for the life of the
// device. `last_raw` lives in RTC memory, so not even a reboot clears it.
//
// Three in a row is far more than one bad transfer and is decisive evidence that
// the state, not the sensor, is stale. The recovery deliberately credits nothing:
// the steps in the gap are unknowable, and inventing them is worse than losing
// them.
constexpr uint8_t kMaxConsecutiveRejections = 3;

// How long the count may go without a fresh reading before it stops being
// presentable as current.
//
// Measured in **minutes, not wakes**, and that is a deliberate choice. The tick
// interval is not constant — 1 minute normally, 5 on a low battery — so a
// threshold of "N missed wakes" would mean N minutes on one path and 5N on
// another: one number with two meanings, and the low-battery watch would be the
// one that lies for longest. Minutes mean the same thing on every path.
//
// Ten is short enough that a wearer cannot build much of a wrong impression from
// a frozen number, and long enough to ride out a handful of garbled transfers on
// a shared bus — at the normal tick it takes ten consecutive failures.
constexpr uint32_t kStaleStepMinutes = 10;

uint32_t maxPlausibleDelta(uint32_t elapsed_minutes);

struct StepState {
  uint32_t last_raw = 0;     // previous value of the chip's counter
  uint32_t today = 0;        // steps accumulated for day_epoch
  uint32_t yesterday = 0;    // previous day's final total, kept for display
  int32_t day_epoch = 0;     // daysSinceEpoch of the day `today` belongs to
  // Minutes over wakes where a reading was expected and did not arrive. Zeroed by
  // any successful reading; never advanced by a wake that was not going to read
  // the sensor in the first place.
  uint32_t minutes_since_reading = 0;
  uint8_t rejected_streak = 0;  // consecutive implausible readings
  bool primed = false;       // last_raw is meaningful
  bool day_known = false;    // day_epoch is meaningful
};

// Record this wake's outcome against the count's freshness. Call once per wake,
// whether or not a reading arrived.
//
// `reading_expected` is the caller's WakePlan::need_accel: true only on a wake
// that was going to read the sensor at all. The distinction is the whole point.
// Safe mode, Recovery mode and a switched-off feature all skip the sensor
// legitimately, and counting those as missed readings would report "no step data"
// on a watch whose step counter is perfectly healthy and merely idle — turning an
// unrelated fault into a second, false symptom.
//
// `reading_obtained` is whether a raw counter value actually came back.
void noteStepWake(StepState& state, bool reading_expected, bool reading_obtained,
                  uint32_t elapsed_minutes);

// What the step count is worth showing.
//
// The distinction that matters is Live vs Stopped. A frozen count is the one
// failure here that puts *wrong* information on the wearer's wrist rather than
// none: once the sensor stops answering, `today` holds its last value and — since
// nothing calls updateSteps() any more — does not even roll over at midnight. The
// face would go on asserting "3412 steps" indefinitely, and "yesterday" would stop
// meaning yesterday.
enum class StepsDisplay : uint8_t {
  Hidden,   // the feature is off; the step line does not exist
  Live,     // the count is current
  Stopped,  // the sensor is not producing readings; the count is not current
};

// `sensor_gave_up` is core::AccelState::gave_up. Taken as a plain bool rather than
// the struct so this module keeps knowing nothing about the sensor's driver.
//
// There are three separate routes to a frozen count and all three must reach
// Stopped, because they are indistinguishable on the wearer's wrist:
//
//   * the policy gave up on configuring the sensor      -> sensor_gave_up
//   * no reading has ever arrived (sensor absent from   -> !primed
//     the start, so nothing ever primed the state)
//   * readings arrived and then stopped                 -> minutes_since_reading
//
// The third is the one that is easy to miss and the most likely in the field: a
// chip that fails, or loses a solder joint, after shipping. It never gives up —
// an unanswering sensor is deliberately not charged a configuration attempt, so
// that a transient can recover — and it stays primed from when it was working. It
// needs a staleness signal or nothing catches it.
StepsDisplay stepsDisplayFor(const StepState& state, bool feature_enabled,
                             bool sensor_gave_up);

struct StepUpdate {
  uint32_t added = 0;        // steps credited by this call
  bool day_rolled = false;   // a midnight boundary was crossed
  bool counter_restarted = false;  // the chip's counter went backwards
  bool rejected = false;     // the reading was implausible and credited nothing
  bool baseline_dropped = false;  // too many rejections; re-baselining from scratch
};

void resetStepState(StepState& state);

// Fold a raw counter reading into the state.
//
// `now`/`time_valid` drive the midnight rollover; when the clock is not
// trustworthy the day is left alone and steps keep accruing to the current bucket,
// because guessing a date would silently move a day's total to the wrong day.
//
// The first reading after a reset only establishes a baseline and credits nothing:
// the chip's total may include steps from before the reboot, and counting them as
// "today" would be worse than losing the handful taken while the watch was down.
//
// An implausible reading is rejected and the baseline held — but only up to
// `kMaxConsecutiveRejections` in a row, after which the baseline is dropped and
// re-established from the next reading. Rejection must be a pause, never a
// permanent stop.
StepUpdate updateSteps(StepState& state, uint32_t raw_counter, const DateTime& now,
                       bool time_valid, uint32_t elapsed_minutes);

}  // namespace core
