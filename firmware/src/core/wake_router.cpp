#include "core/wake_router.h"

#include "core/sync_policy.h"

namespace core {
namespace {

bool batterySampleDue(const WakeContext& context) {
  return context.minutes_since_battery_sample >= kBatterySampleIntervalMinutes;
}

// Which wakes may carry a sync window at all. Not "is a sync due" — that is
// core::sync_policy's answer, and it is the same on every wake — but "is this the
// kind of wake a window may ride on".
//
// The question exists because PROTOCOL.md §5.3 budgets the window as an extension
// of a wake the watch was already taking. A window that instead justified its own
// wake, or attached itself to wakes that arrive at unpredictable rates, would be
// spending a different budget from the one that was approved.
bool sourceMayCarrySyncWindow(WakeSource source) {
  switch (source) {
    case WakeSource::RtcAlarm:  // the scheduled tick the window is budgeted to extend
    case WakeSource::Timer:     // its backstop; the same wake by another route
    case WakeSource::PowerOn:   // a fresh watch's clock is unset, and this is the wake that can fix it
    case WakeSource::Button:    // the only way a user request can arrive
      return true;

    case WakeSource::Accelerometer:
      // Motion is not a reason to power up the radio. The interrupt is disarmed
      // today (kAccelWakeEnabled), so this is about the day a motion feature turns
      // it on: an accelerometer wake is deliberately the cheapest path in this
      // router — no clock, no panel — and a wrist flick must not be able to make
      // it the most expensive one.
      return false;

    case WakeSource::Unknown:
      // The previous run ended in a way board::power could not classify: a panic,
      // a brownout, an unexplained reset. core::health has not yet had the three
      // consecutive faults it needs to escalate into Safe mode, so this is exactly
      // the wake where the firmware knows something is wrong and has not yet
      // degraded. Bringing up the largest consumer on the board there is the wrong
      // default, and refusing costs nothing: the next ordinary tick carries the
      // window instead, minutes later.
      return false;
  }
  return false;
}

bool syncWindowGranted(WakeSource source, const WakeContext& context) {
  if (!sourceMayCarrySyncWindow(source)) {
    return false;
  }

  SyncContext sync;
  sync.mode = context.mode;
  sync.battery = context.battery;
  sync.minutes_since_window = context.minutes_since_sync_window;
  sync.user_requested = context.sync_requested;
  return evaluateSyncWindow(sync).open;
}

}  // namespace

uint16_t tickIntervalFor(const WakeContext& context) {
  if (context.mode == RunMode::Recovery) {
    return kRecoveryTickSeconds;
  }
  // Safe mode uses the battery-saving interval too: whatever is faulting, waking
  // less often makes it hurt less.
  if (context.mode == RunMode::Safe) {
    return kSavingTickSeconds;
  }
  return tickIntervalSeconds(context.battery);
}

WakePlan routeWake(WakeSource source, const WakeContext& context) {
  WakePlan plan;
  plan.next_tick_seconds = tickIntervalFor(context);

  // Recovery mode is deliberately almost inert: read the clock so the next alarm
  // can be armed, and leave the panel alone. A crash loop that reaches here stops
  // draining the battery.
  if (context.mode == RunMode::Recovery) {
    plan.need_rtc = true;
    plan.need_display = (source == WakeSource::PowerOn);
    plan.force_full_refresh = plan.need_display;
    plan.need_i2c = plan.need_rtc;
    return plan;
  }

  switch (source) {
    case WakeSource::PowerOn:
      // Nothing about the panel or the persisted state can be trusted, so take
      // the one expensive wake and establish a known baseline.
      plan.need_rtc = true;
      plan.need_battery = true;
      plan.need_display = true;
      plan.force_full_refresh = true;
      plan.need_accel = context.accel_features_enabled;
      break;

    case WakeSource::RtcAlarm:
      plan.need_rtc = true;
      plan.need_display = true;
      plan.need_battery = batterySampleDue(context);
      plan.need_accel = context.accel_features_enabled;
      break;

    case WakeSource::Timer:
      // The RTC alarm was missed. Behave like a tick, but re-read the RTC and
      // treat the clock as suspect; the caller re-arms the alarm from scratch.
      //
      // "Like a tick" has to include the accelerometer. A watch whose PCF8563 has
      // died runs on this path indefinitely, and the sensor's counter only ever
      // rises: stop reading it and the gap grows past what the step logic can
      // plausibly credit, so the count is stuck until it re-baselines. It is one
      // extra register read on a bus that is already open for the RTC.
      plan.need_rtc = true;
      plan.need_display = true;
      plan.need_battery = batterySampleDue(context);
      plan.need_accel = context.accel_features_enabled;
      break;

    case WakeSource::Button:
      // The time is needed to render, but nothing else is. No ADC, no
      // accelerometer — a button press should be close to free.
      plan.need_rtc = true;
      plan.need_display = true;
      plan.run_ui = true;
      plan.need_battery = batterySampleDue(context);
      break;

    case WakeSource::Accelerometer:
      // Motion alone does not justify a panel refresh; the step count is
      // accumulated and shown at the next tick. Unless a screen is already up,
      // in which case it is live and worth updating.
      plan.need_accel = true;
      plan.need_rtc = context.ui_active;
      plan.need_display = context.ui_active;
      break;

    case WakeSource::Unknown:
      // Provenance unknown, so the panel may not match the state. One full
      // refresh resynchronises rather than leaving a stale screen indefinitely,
      // and the sensor is collected for the same reason as on the timer backstop:
      // a wake that skips it widens the gap the step logic has to explain.
      plan.need_rtc = true;
      plan.need_display = true;
      plan.force_full_refresh = true;
      plan.need_accel = context.accel_features_enabled;
      break;
  }

  if (context.mode == RunMode::Safe) {
    // Strip everything optional: the display and the clock stay, so the watch is
    // still a watch, and every extra peripheral is dropped.
    plan.need_accel = false;
    plan.need_battery = false;
    plan.run_ui = false;
  }

  // Deliberately after the Safe-mode strip and reached only by the paths that fall
  // through the switch — the Recovery branch returns above, so a watch in Recovery
  // leaves this flag at its default. sync_policy refuses both modes as well; the
  // radio is the one peripheral worth gating twice.
  plan.need_ble = syncWindowGranted(source, context);

  plan.need_i2c = plan.need_rtc || plan.need_accel;
  return plan;
}

}  // namespace core
