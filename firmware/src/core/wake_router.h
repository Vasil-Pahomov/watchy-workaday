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

  // This wake is the KIND a BLE sync window may ride on (PROTOCOL.md §5.3).
  //
  // Not a grant, and the difference is the whole contract. §5.1's gates — the run
  // mode, the battery, the hour boundary — are core::evaluateSyncWindow()'s answer
  // and the caller asks it separately. This field answers only the question this
  // router is in a position to answer: whether the §5.3 budget, which prices a
  // window as an extension of a wake the watch was already taking, covers *this*
  // wake at all.
  //
  // **It used to be the grant, and it could not stay one.** §5.1 now schedules the
  // window on the hour boundary, so the decision reads the wall clock — and this
  // router runs before the I2C bus is open, because it is what decides whether the
  // clock is worth reading. A grant made here would have had to be made without
  // the input it now depends on. Moving it out also retired a documented trap: the
  // grant was evaluated twice, once here from top-of-wake state and once after the
  // button dispatch, because which menu item a press activates is not knowable
  // until the press has been dispatched. There is one evaluation now, after the
  // clock read and after the dispatch, against state that is current.
  //
  // What has not moved: the caller must still call core::noteSyncWindowOpened()
  // *before* the radio comes up. The schedule is spent when the window opens,
  // never when it succeeds — a window that reached nobody, or one that died to a
  // watchdog reset, must still cost the full hour, or a watch whose phone is out
  // of range advertises on every wake for the rest of the charge. See
  // core/sync_policy.h.
  bool may_carry_sync_window = false;

  uint16_t next_tick_seconds = kNormalTickSeconds;
};

WakePlan routeWake(WakeSource source, const WakeContext& context);

// The tick interval this context implies, independent of wake source: Recovery
// mode stretches furthest, then a low battery, then normal operation.
uint16_t tickIntervalFor(const WakeContext& context);

}  // namespace core
