// Wake reason + state -> which peripherals to power up. Pure logic.
//
// This is where Law 1 is actually enforced. `board/` does not decide whether to
// bring up the I2C bus or read the ADC; it is told. That keeps the expensive
// decisions — the ones that cost microamp-hours — inside a pure function where
// "does a button wake spin up the accelerometer?" is a unit test instead of a
// bench measurement with a current probe.
//
// The design bias is subtractive: start from "do nothing" and switch things on
// only where there is a reason. A minute tick does not touch the accelerometer.
// It does not read the battery unless a sample is due. Neither does a button wake.
#pragma once

#include <cstdint>

#include "core/battery_model.h"
#include "core/health.h"

namespace core {

enum class WakeSource : uint8_t {
  PowerOn,        // cold boot or reset — nothing on screen can be trusted
  RtcAlarm,       // the PCF8563 minute tick, the overwhelmingly common case
  Button,         // ext1, any of the four active-high buttons
  Accelerometer,  // BMA423 interrupt; only armed when a motion feature is on
  Timer,          // ESP32 timer backstop — the RTC alarm did not arrive
  Unknown,
};

// The battery moves slowly; sampling it every minute buys nothing and costs an
// ADC power-up each time.
constexpr uint32_t kBatterySampleIntervalMinutes = 10;

struct WakeContext {
  RunMode mode = RunMode::Normal;
  BatteryLevel battery = BatteryLevel::Normal;
  uint32_t minutes_since_battery_sample = kBatterySampleIntervalMinutes;
  bool accel_features_enabled = false;
  bool ui_active = false;  // a screen other than the watchface is up

  // core::SyncState::minutes_since_window, straight through. Kept flat here rather
  // than as an embedded SyncContext because mode and battery would then exist
  // twice in this struct, and a caller that set one copy and not the other would
  // be deciding the radio's fate from stale state.
  uint16_t minutes_since_sync_window = 0;
  // The user picked the Sync menu item on this wake (PROTOCOL.md §5.1). Defaults
  // to false, so the radio is never granted a window by an omission.
  bool sync_requested = false;
};

struct WakePlan {
  // A device on the bus must be READ this wake. Always == (need_rtc || need_accel).
  // Note that main.cpp opens the bus on every wake regardless, because the RTC tick
  // has to be re-armed and its interrupt flag cleared each time; this flag governs
  // reads, not the bus itself.
  bool need_i2c = false;
  bool need_rtc = false;
  bool need_accel = false;
  bool need_battery = false;  // power up the ADC
  bool need_display = false;  // bring the panel out of hibernation
  bool run_ui = false;        // dispatch a button press through ui_state
  bool force_full_refresh = false;

  // A BLE sync window is GRANTED on this wake (PROTOCOL.md §5.1).
  //
  // Read the note on need_i2c above and then read this one, because the same trap
  // is here and it is more expensive: the name is narrower than it looks. It does
  // not mean "the radio is on", it does not mean a phone is there, and it does not
  // survive being ignored. It means every §5.1 gate passed and the caller may open
  // **one** window.
  //
  // What comes with the grant: the caller must call core::noteSyncWindowOpened()
  // *before* the radio comes up. The hourly timer is spent when the window opens,
  // never when it succeeds — a window that reached nobody, or one that died to a
  // watchdog reset, must still cost the full hour, or a watch whose phone is out
  // of range advertises on every wake for the rest of the charge. See
  // core/sync_policy.h.
  bool need_ble = false;

  uint16_t next_tick_seconds = kNormalTickSeconds;
};

WakePlan routeWake(WakeSource source, const WakeContext& context);

// The tick interval this context implies, independent of wake source: Recovery
// mode stretches furthest, then a low battery, then normal operation.
uint16_t tickIntervalFor(const WakeContext& context);

}  // namespace core
