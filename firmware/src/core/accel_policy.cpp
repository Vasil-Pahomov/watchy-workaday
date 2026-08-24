#include "core/accel_policy.h"

namespace core {
namespace {

// Ask for a park if one is owed and the budget allows. Spending the budget clears
// `suspend_pending` rather than leaving it set, so the "give up" state settles at
// "touch nothing, ever" instead of two transactions a minute for the rest of the
// charge.
AccelAction requestSuspend(AccelState& state) {
  if (!state.suspend_pending || state.suspend_attempts >= kMaxAccelSuspendAttempts) {
    state.suspend_pending = false;
    return AccelAction::Skip;
  }
  // Counted before the caller runs it, for the same reason as the config attempt:
  // a transaction that wedges the bus and takes the wake down with it must not
  // come back with a full budget.
  ++state.suspend_attempts;
  return AccelAction::Suspend;
}

// The one place `gave_up` is set. Parking the sensor is part of giving up, not an
// afterthought: a BMA423 left enabled but unread costs ~14 uA for nothing.
AccelAction giveUp(AccelState& state) {
  state.gave_up = true;
  state.suspend_pending = true;
  return requestSuspend(state);
}

// Common tail: we have decided the sensor needs configuring. Either spend an
// attempt or give up.
AccelAction requestConfigure(AccelState& state) {
  if (state.config_attempts >= kMaxAccelConfigAttempts) {
    return giveUp(state);
  }
  ++state.config_attempts;
  // Set before the caller runs the upload, not after. If the upload wedges the
  // bus and the wake dies on the watchdog, this is what the next boot reads.
  state.config_incomplete = true;
  return AccelAction::Configure;
}

}  // namespace

void resetAccelState(AccelState& state) { state = AccelState{}; }

AccelAction accelBeginWake(AccelState& state) {
  if (!state.gave_up) {
    return AccelAction::Probe;
  }
  return requestSuspend(state);
}

AccelAction accelPlan(AccelState& state, AccelProbe probe) {
  if (state.gave_up) {
    return AccelAction::Skip;
  }

  switch (probe) {
    case AccelProbe::Absent:
      // Nothing to configure — the chip is not answering at all. Uploading into
      // a device that will not acknowledge cannot help, and the probe reads that
      // got us here are cheap enough to repeat, so a sensor that comes back after
      // a transient is picked up without ever spending an attempt.
      return AccelAction::Skip;

    case AccelProbe::Ready:
      if (!state.config_incomplete) {
        // A configuration that is still present at the *start* of a wake has
        // survived a deep-sleep cycle. That, and only that, is evidence an upload
        // actually took — so it is the only thing that returns the attempt budget.
        state.config_attempts = 0;
        return AccelAction::Read;
      }
      // "Initialised" is set by the first half of the upload and says nothing
      // about the half that failed: the chip can hold that bit while the
      // accelerometer is disabled or the step feature was never enabled, and it
      // would then read a constant zero for ever. We know our last attempt did
      // not finish, so the bit is not trusted.
      break;

    case AccelProbe::Idle:
      // Configured but not running. Whatever put it here, it is not counting.
      break;

    case AccelProbe::Unconfigured:
      break;
  }

  return requestConfigure(state);
}

AccelAction accelAfterConfigure(AccelState& state, bool ok) {
  if (ok) {
    state.config_incomplete = false;
    // Note what is *not* done here: the attempt count is not cleared. A
    // configure() that reports success but does not stick — the marginal-bus
    // case — would otherwise refund its own attempt and retry for ever.
    return AccelAction::Read;
  }

  if (state.config_attempts >= kMaxAccelConfigAttempts) {
    return giveUp(state);
  }
  return AccelAction::Skip;
}

void accelAfterSuspend(AccelState& state, bool ok) {
  if (ok) {
    state.suspend_pending = false;
  }
  // A failed park leaves the flag set: the sensor is still burning current, so
  // the next wake tries again until the budget in requestSuspend() runs out.
}

}  // namespace core
