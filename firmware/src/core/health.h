// Cross-boot health tracking and fault escalation. Pure logic — the caller
// supplies the reset reason and owns the storage.
//
// This is the module that keeps a broken watch alive. Without it, a fault that
// reproduces every wake becomes a crash loop: the ESP32 reboots, crashes, reboots
// — never sleeping, drawing ~40 mA continuously, and flattening the 200 mAh cell
// in about five hours. With it, three consecutive faults drop the firmware into
// Safe mode and six into Recovery, where the watch does almost nothing but stays
// alive for weeks and still shows a face.
//
// The state lives in `RTC_NOINIT_ATTR` memory, and that attribute rather than
// `RTC_DATA_ATTR` is what makes this module work at all: `.rtc.data` is a
// loadable segment the bootloader refills on every boot that is not a deep-sleep
// wake, so a panic or a watchdog reset would restore the initialisers below and
// `consecutive_faults` would come back as 1 every single time — the escalation to
// Safe and then Recovery could never reach 3, for precisely the faults it exists
// to survive. `.rtc_noinit` is never written by anything.
//
// The cost of that is the block also survives reflashing, so on the first boot of
// a new build it contains the previous build's bytes. Hence magic- and
// version-checking before use rather than trusting it. The caller owns the
// storage and the attribute; see `docs/architecture.md`, "State across sleep".
#pragma once

#include <cstdint>

namespace core {

enum class RunMode : uint8_t { Normal, Safe, Recovery };

enum class ResetReason : uint8_t {
  PowerOn,
  DeepSleepWake,
  SoftwareRestart,
  Panic,
  TaskWatchdog,
  IntWatchdog,
  Brownout,
  ExternalPin,
  Unknown,
};

constexpr uint32_t kHealthMagic = 0x57414B31u;  // 'WAK1'
constexpr uint8_t kHealthVersion = 1;

constexpr uint16_t kSafeModeFaultThreshold = 3;
constexpr uint16_t kRecoveryModeFaultThreshold = 6;

struct HealthState {
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t boot_count = 0;
  uint16_t consecutive_faults = 0;
  uint16_t total_faults = 0;
  ResetReason last_reason = ResetReason::Unknown;
  RunMode mode = RunMode::Normal;
  // Set immediately before deep sleep. Its absence on the next boot means the
  // previous run died somewhere between waking and sleeping, which the watchdog
  // may not have caught.
  bool run_completed = false;
};

bool isFaultReason(ResetReason reason);

bool isPersistedStateValid(const HealthState& state);

// Resets to first-boot defaults and stamps magic/version.
void initHealthState(HealthState& state);

// Call once, early in setup(), before anything that can fault. Re-initialises
// invalid state, counts the boot, updates the fault counters and returns the mode
// this run should operate in.
RunMode beginBoot(HealthState& state, ResetReason reason);

// Call immediately before entering deep sleep. This is what marks the run a
// success; nothing else clears the fault counter.
void markRunComplete(HealthState& state);

RunMode modeForFaultCount(uint16_t consecutive_faults);

// Recovery mode stretches the tick out this far. Long enough to make a crash
// loop harmless, short enough that the watch visibly recovers on its own.
constexpr uint16_t kRecoveryTickSeconds = 900;

}  // namespace core
