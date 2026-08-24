#include "core/step_counter.h"

namespace core {
namespace {

uint32_t saturatingAdd(uint32_t base, uint32_t addend) {
  const uint32_t sum = base + addend;
  return sum < base ? UINT32_MAX : sum;
}

}  // namespace

void resetStepState(StepState& state) { state = StepState{}; }

void noteStepWake(StepState& state, bool reading_expected, bool reading_obtained,
                  uint32_t elapsed_minutes) {
  if (reading_obtained) {
    // Recovery is automatic and immediate: one reading is proof the sensor is
    // answering again, whatever it says.
    state.minutes_since_reading = 0;
    return;
  }
  if (!reading_expected) {
    // Held, not advanced and not cleared. A wake that was never going to read the
    // sensor is evidence of nothing, so it neither ages the count nor refreshes
    // it — a Safe-mode spell in the middle of a real outage must not restart the
    // clock any more than it should start one.
    return;
  }
  state.minutes_since_reading = saturatingAdd(state.minutes_since_reading, elapsed_minutes);
}

StepsDisplay stepsDisplayFor(const StepState& state, bool feature_enabled,
                             bool sensor_gave_up) {
  if (!feature_enabled) {
    return StepsDisplay::Hidden;
  }
  if (sensor_gave_up || !state.primed) {
    return StepsDisplay::Stopped;
  }
  if (state.minutes_since_reading >= kStaleStepMinutes) {
    return StepsDisplay::Stopped;
  }
  return StepsDisplay::Live;
}

uint32_t maxPlausibleDelta(uint32_t elapsed_minutes) {
  uint32_t minutes = elapsed_minutes == 0 ? 1u : elapsed_minutes;
  if (minutes > kMaxCreditedGapMinutes) {
    minutes = kMaxCreditedGapMinutes;
  }
  return minutes * kMaxStepsPerMinute;
}

StepUpdate updateSteps(StepState& state, uint32_t raw_counter, const DateTime& now,
                       bool time_valid, uint32_t elapsed_minutes) {
  StepUpdate result;

  // ── credit steps to the current day ───────────────────────────────────────
  if (!state.primed) {
    // First reading after a reboot. The chip's total may include steps taken
    // before we were running, and there is no way to tell how many, so this
    // reading only establishes a baseline.
    state.primed = true;
    state.last_raw = raw_counter;
  } else {
    const bool restarted = raw_counter < state.last_raw;
    // A counter that went backwards means the sensor restarted its count, so the
    // whole current value is new steps rather than a difference.
    const uint32_t delta = restarted ? raw_counter : (raw_counter - state.last_raw);

    if (delta <= maxPlausibleDelta(elapsed_minutes)) {
      state.last_raw = raw_counter;
      state.today = saturatingAdd(state.today, delta);
      state.rejected_streak = 0;
      result.added = delta;
      result.counter_restarted = restarted;
    } else {
      // The reading is rejected and — importantly — the baseline is left alone.
      // Adopting a garbled value as the baseline would make the next healthy read
      // look like a counter restart and credit it in full, turning one bad I2C
      // transfer into thousands of phantom steps.
      result.rejected = true;
      if (state.rejected_streak < UINT8_MAX) {
        ++state.rejected_streak;
      }

      if (state.rejected_streak >= kMaxConsecutiveRejections) {
        // ...but holding it for ever is its own failure. The chip's counter only
        // increases, so a gap that was genuinely too large to credit makes every
        // later reading worse, not better, and the counter would be dead until
        // the battery came out. Drop the baseline instead.
        //
        // Note what this does NOT do: adopt `raw_counter` as the new baseline.
        // Un-priming makes the *next* reading the baseline, exactly as after a
        // reboot, so a run of garbage cannot leave a garbage baseline behind that
        // the following healthy read would mistake for a counter restart.
        state.primed = false;
        state.last_raw = 0;
        state.rejected_streak = 0;
        result.baseline_dropped = true;
      }
    }
  }

  // ── roll the day over ─────────────────────────────────────────────────────
  // Deliberately after crediting: the steps measured across a midnight tick belong
  // mostly to the day that just ended, so they land in `yesterday` rather than
  // being the first entry of the new day.
  if (time_valid && isValid(now)) {
    const int32_t current_epoch = daysSinceEpoch(now);
    if (!state.day_known) {
      state.day_epoch = current_epoch;
      state.day_known = true;
    } else if (current_epoch != state.day_epoch) {
      state.yesterday = state.today;
      state.today = 0;
      state.day_epoch = current_epoch;
      result.day_rolled = true;
    }
  }
  // With an untrustworthy clock the day is left as it is. Guessing would silently
  // file a day's steps under the wrong date, which is worse than a late rollover.

  return result;
}

}  // namespace core
