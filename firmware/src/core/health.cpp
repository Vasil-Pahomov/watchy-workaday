#include "core/health.h"

namespace core {

bool isFaultReason(ResetReason reason) {
  switch (reason) {
    case ResetReason::Panic:
    case ResetReason::TaskWatchdog:
    case ResetReason::IntWatchdog:
    case ResetReason::Brownout:
    case ResetReason::Unknown:
      return true;

    case ResetReason::PowerOn:
    case ResetReason::DeepSleepWake:
    case ResetReason::SoftwareRestart:
    case ResetReason::ExternalPin:
      return false;
  }
  // An unrecognised value means the enum grew without this switch being updated;
  // treating it as a fault fails safe.
  return true;
}

bool isPersistedStateValid(const HealthState& state) {
  return state.magic == kHealthMagic && state.version == kHealthVersion;
}

void initHealthState(HealthState& state) {
  state = HealthState{};
  state.magic = kHealthMagic;
  state.version = kHealthVersion;
}

RunMode modeForFaultCount(uint16_t consecutive_faults) {
  if (consecutive_faults >= kRecoveryModeFaultThreshold) {
    return RunMode::Recovery;
  }
  if (consecutive_faults >= kSafeModeFaultThreshold) {
    return RunMode::Safe;
  }
  return RunMode::Normal;
}

RunMode beginBoot(HealthState& state, ResetReason reason) {
  if (!isPersistedStateValid(state)) {
    initHealthState(state);
  }

  if (state.boot_count < UINT32_MAX) {
    state.boot_count++;
  }

  // A deep-sleep wake whose predecessor never reached markRunComplete() died
  // silently — no panic, no watchdog, just a run that stopped. Counting it keeps
  // that failure mode inside the escalation ladder instead of invisible.
  const bool silent_death = (reason == ResetReason::DeepSleepWake) && !state.run_completed;
  const bool fault = isFaultReason(reason) || silent_death;

  if (fault) {
    if (state.consecutive_faults < UINT16_MAX) {
      state.consecutive_faults++;
    }
    if (state.total_faults < UINT16_MAX) {
      state.total_faults++;
    }
  } else {
    state.consecutive_faults = 0;
  }

  state.last_reason = reason;
  state.run_completed = false;
  state.mode = modeForFaultCount(state.consecutive_faults);
  return state.mode;
}

void markRunComplete(HealthState& state) { state.run_completed = true; }

}  // namespace core
