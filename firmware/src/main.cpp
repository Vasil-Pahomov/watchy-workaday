// Workaday — SQFMI Watchy v2.0 firmware.
//
// There is no run loop. Each wake is a fresh, short-lived program:
//
//     wake -> arm watchdog -> classify -> plan -> do the minimum -> deep sleep
//
// setup() always ends in board::power::deepSleep(), which does not return. loop()
// is a tripwire: reaching it means the sleep path was bypassed.
#include <Arduino.h>
#include <esp_system.h>

#include "app/screens.h"
#include "board/accel.h"
#include "board/battery.h"
#include "board/ble.h"
#include "board/board_v20.h"
#include "board/buttons.h"
#include "board/diag.h"
#include "board/display.h"
#include "board/i2c.h"
#include "board/power.h"
#include "board/rtc.h"
#include "core/accel_policy.h"
#include "core/battery_model.h"
#include "core/find_session.h"
#include "core/health.h"
#include "core/protocol.h"
#include "core/refresh_policy.h"
#include "core/step_counter.h"
#include "core/sync_policy.h"
#include "core/time_model.h"
#include "core/ui_state.h"
#include "core/wake_router.h"

namespace {

constexpr uint32_t kPersistMagic = 0x57444159u;  // 'WDAY'
// Bump on every layout change to this struct. Version 1 -> 2 added step state;
// 2 -> 3 added the accelerometer configuration budget and the step counter's
// rejection streak; 3 -> 4 added the sensor-park retry state; 4 -> 5 added the
// step-reading staleness clock; 5 -> 6 added the BLE sync window's hourly timer
// and the last sync result; 6 -> 7 added the display theme, and the menu it is
// chosen from grew a third item, which changes what an already-persisted
// menu_index of 2 would mean; 7 -> 8 added the find-phone outcome, and the menu
// grew a fourth item. An old block is discarded rather than reinterpreted, which
// is the whole point of carrying a version at all — including for a version that
// only ever existed on a bench.
constexpr uint8_t kPersistVersion = 8;

// Everything that must survive deep sleep **and every kind of reset**. It lives
// in `.rtc_noinit` (see the definition of g_persist below for why that is not the
// same thing as RTC_DATA_ATTR), which nothing ever writes on boot — so it also
// survives reflashing, and on the first boot of a new build it holds the previous
// build's bytes. Hence the magic and version, checked before any field is
// trusted.
struct PersistedState {
  uint32_t magic = 0;
  uint8_t version = 0;
  core::RefreshState refresh;
  core::UiState ui;
  core::DateTime last_time;
  core::BatteryFilter battery_filter;
  core::BatteryLevelTracker battery_level;
  core::StepState steps;
  // The sensor's configuration budget. It lives here, in RTC memory, precisely so
  // an upload that ends in a watchdog reset still counts against the limit — a
  // counter in RAM would be zeroed by that reset and the watch would retry the
  // 6 KB upload for ever.
  core::AccelState accel;
  // The BLE sync window's hourly timer, plus what the last window achieved so that
  // PROTOCOL.md §3.2's read path can be answered from here rather than from a
  // fresh clock or ADC read.
  core::SyncState sync;
  uint8_t battery_percent = 0;
  uint32_t minutes_since_battery_sample = core::kBatterySampleIntervalMinutes;
  bool use_24h = true;
  // Which way round ink and paper go (core::kThemeMenuIndex). Here rather than in
  // core::UiState because it is a preference and not navigation: UiState's screen
  // and menu_index are reset by the idle timeout and by Back, and the theme must
  // survive both of those as well as the sleep. False is white on black, the face
  // the watch ships with, and it is also what a first boot or a discarded block
  // lands on — an unreadable default is not something a version check should be
  // able to produce.
  bool inverted = false;
  // How the last find-phone search ended (core::kFindPhoneMenuIndex, PROTOCOL.md
  // §4.1). Here for the reason the theme is: the Find phone screen has to say
  // "phone found" on the wakes after the search, across the deep sleep in
  // between, and UiState is reset by exactly the two things — Back and the idle
  // timeout — that should clear it. Written before a session's first frame and
  // again when it ends; only the Find phone screen ever reads it.
  core::FindOutcome find_outcome = core::FindOutcome::InProgress;
};

// Step counting. Costs the sensor's own ~14 uA of sleep current — about a quarter
// of the sleep floor — and no extra wakes at all.
constexpr bool kStepCounterEnabled = true;

// Whether the BMA423 interrupt joins the ext1 wake mask. Deliberately independent
// of the flag above, and deliberately false: the step counter accumulates inside
// the sensor and is collected during the minute tick we already take, so waking the
// CPU on motion would buy nothing and cost a wake per gesture. This turns on only
// when a genuinely motion-triggered feature exists.
constexpr bool kAccelWakeEnabled = false;

// PROTOCOL.md §3.2's `fw_build`: a u16 the watch reports and the phone only ever
// shows on a diagnostic screen. Bump it when a released build changes; nothing on
// either side branches on it, which is the point — a diagnostic that acquired
// behaviour would become a second version number competing with PROTO_VERSION.
constexpr uint16_t kFirmwareBuild = 2;

bool persistValid(const PersistedState& state) {
  return state.magic == kPersistMagic && state.version == kPersistVersion;
}

void persistInit(PersistedState& state) {
  state = PersistedState{};
  state.magic = kPersistMagic;
  state.version = kPersistVersion;
}

uint8_t tickMinutes(uint16_t tick_seconds) {
  const uint32_t minutes = tick_seconds / 60u;
  if (minutes == 0) {
    return 1;
  }
  return minutes > 255u ? 255u : static_cast<uint8_t>(minutes);
}

// **A property held by the compiler, not by a comment.**
//
// board::ble::Session's destructor stops advertising but does not power the radio
// down — esp_deep_sleep_start() does. That makes board::power::deepSleep() the
// single choke point where "the radio is off" becomes true, and it is a choke point
// only because it is a *terminus*: [[noreturn]] is what stops it being one step
// among several that setup() might return from. Lose the attribute and setup()
// silently gains a path that ends with the controller still enabled.
//
// This function is never called. It sits in main.cpp rather than beside the
// definition on purpose — here only the declaration in board/power.h is visible,
// which is what every caller actually sees, so removing the attribute from the
// header fires it. The pragma is what makes it a gate rather than a grumble:
// -Werror is scoped to the native test environment, so in the firmware build
// -Wreturn-type would only warn — promoting that one diagnostic to an error here,
// and nowhere else, fails the build the moment control could reach the end of this
// non-void function. Unreferenced and in an anonymous namespace, so it costs
// nothing in the image.
//
// It holds one half of the guarantee. The other half — that nothing is inserted
// between the sync window closing and the sleep — is statement ordering inside
// setup(), which nothing available here can express. board/ble.h says so plainly
// rather than pretending otherwise, and names the log interval that shows it.
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wreturn-type"
[[maybe_unused]] int deepSleepIsATerminus() {
  board::power::deepSleep(0, /*accel_wake=*/false);
}
#pragma GCC diagnostic pop

// ── the screen, as one function ─────────────────────────────────────────────
//
// Every frame the watch paints goes through here: the one frame an ordinary wake
// draws, and the several a find session draws while it runs. Factored out so the
// session cannot grow its own copy of the compose/decide/render sequence and drift
// from it — the hash, the ghosting cadence and the hibernate-on-every-path guard
// are the same three things whichever caller asks.

// The persisted state as the screens see it, plus the wake's own readings.
// find_live is false: a live search sets it, and its counters, itself.
app::Snapshot snapshotFrom(const PersistedState& persist, const core::DateTime& now,
                           bool time_valid, core::RunMode mode) {
  app::Snapshot snapshot;
  snapshot.time = now;
  snapshot.time_valid = time_valid;
  snapshot.use_24h = persist.use_24h;
  snapshot.battery_percent = persist.battery_percent;
  snapshot.mode = mode;
  snapshot.screen = persist.ui.screen;
  snapshot.menu_index = persist.ui.menu_index;
  snapshot.steps_today = persist.steps.today;
  snapshot.steps_yesterday = persist.steps.yesterday;
  // Not the compile-time flag: a sensor the policy has given up on, or one that
  // never produced a first reading, leaves `today` frozen — and frozen includes
  // "does not roll over at midnight". core decides what that is worth showing.
  snapshot.steps_display =
      core::stepsDisplayFor(persist.steps, kStepCounterEnabled, persist.accel.gave_up);
  // What the previous window achieved, for the Sync screen. Read straight out
  // of the persisted block — the same answer PROTOCOL.md §3.2 gives the phone,
  // and it costs nothing to show.
  snapshot.sync_result = persist.sync.last_result;
  snapshot.sync_applied = persist.sync.last_applied_epoch_s != 0;
  snapshot.inverted = persist.inverted;
  snapshot.find_outcome = persist.find_outcome;
  return snapshot;
}

// Compose, decide, and — only if the pixels would differ — drive the panel.
// `first_boot` performs the panel's initial full reset and is true only on the
// power-on wake's frame; every later frame, in this wake or another, passes false.
void paint(PersistedState& persist, const app::Snapshot& snapshot, uint32_t elapsed_minutes,
           bool force_full, bool first_boot) {
  const uint32_t hash = app::compose(snapshot);
  const core::RefreshKind kind =
      core::decideRefresh(persist.refresh, hash, elapsed_minutes, force_full);
  if (kind == core::RefreshKind::Skip) {
    // Skip never gets near the panel, so an unchanged screen costs no panel
    // activity at all.
    return;
  }
  // The panel is initialised and hibernated by this scope.
  board::display::Session panel(first_boot, persist.inverted);
  board::display::render(kind, &app::draw);
}

// ── one Time write, answered ────────────────────────────────────────────────
//
// PROTOCOL.md §4, from the twelve bytes arriving to the Status leaving. Shared by
// the sync window and the find session, because §4.1 makes the find session's
// exchange §4 verbatim — the one difference is the `flags` the answer carries,
// and that is the argument. Everything decided here is decided in core; this
// function moves bytes between the radio, the decoder, the clock and the encoder.
//
// Returns the §3.2 result so the caller can apply its own rule to it: the sync
// window's §4 completion rule, or the find session's "nothing — the exchange is a
// side effect".
core::SyncResult answerTimeWrite(board::ble::Session& radio, PersistedState& persist,
                                 uint8_t status_flags) {
  // Twelve bytes arrived. Whether they mean anything is a decision, and it lives
  // in core where a test replays PROTOCOL.md §7's vectors against it.
  uint8_t payload[core::kTimePayloadLength] = {};
  const size_t length = radio.copyTimeWrite(payload, sizeof(payload));
  const core::TimeWrite decoded = core::decodeTimeWrite(payload, length);

  core::SyncResult result = decoded.result;
  uint32_t applied = 0;

  if (result == core::SyncResult::Ok) {
    // The I2C ordering problem, resolved by reopening the bus rather than by
    // holding it open across the window. board::rtc::write() needs a live
    // i2c::Session and the wake's own bus scope closed long ago — but the
    // alternative, moving the window inside that scope, would keep the bus
    // powered for the whole 6-12 s the radio is up, on every window, including
    // the great majority where no phone ever appears and nothing is written.
    // This way the two peripherals overlap for one register burst on the rare
    // path that actually sets the clock. Still RAII: the bus closes here whether
    // the write succeeded, failed or the session never opened.
    bool written = false;
    {
      board::i2c::Session bus;
      if (bus.ok()) {
        written = board::rtc::write(decoded.local);
      }
    }

    if (written) {
      applied = decoded.utc_epoch_s;
      // The clock has just moved, possibly by years. last_time is what the next
      // wake measures elapsed time against, so leaving it on the old reading
      // would hand core::elapsedMinutes() a jump it has to clamp — and every
      // counter driven by it, the sync timer included, would inherit that.
      persist.last_time = decoded.local;
    } else {
      // §6.1: the validated time did not reach the PCF8563. The clock keeps its
      // old value; nothing is half-written.
      result = core::SyncResult::RtcWriteFailed;
    }
  }

  // §5.1's second progress point: a Time write has been processed all the way
  // through to the clock.
  board::power::feedWatchdog();

  core::noteSyncResult(persist.sync, result, applied);

  core::Status outcome = core::statusFromSyncState(
      persist.sync, persist.battery_percent, persist.battery_filter.primed(), kFirmwareBuild);
  // The one field where the notification and the read disagree. §3.2's table
  // defines applied_utc_epoch_s as "what the watch actually committed; 0 if
  // nothing was", and it is only the *read* path — §3.2's last paragraph — that
  // reports the last successfully applied value instead. statusFromSyncState()
  // answers the read question, so this answers the other one. Everything else,
  // including the rule that an unsampled battery goes out as 0xFF rather than a
  // genuine-looking 0, is left to it rather than re-derived here.
  outcome.applied_utc_epoch_s = applied;
  // §3.2 `flags`: zero from a sync window, FIND_PHONE from a find session. An
  // assignment, not a decision — which session this is was settled by the press
  // that opened it.
  outcome.flags = status_flags;

  uint8_t outgoing[core::kStatusPayloadLength] = {};
  if (core::encodeStatus(outgoing, sizeof(outgoing), outcome)) {
    // §6.1: a bad payload is answered with its failure code, not with a hang-up.
    const bool notified = radio.notify(outgoing, sizeof(outgoing));
    WD_LOG("ble: result=%d applied=%lu flags=%02X notified=%d", static_cast<int>(result),
           static_cast<unsigned long>(applied), status_flags, notified ? 1 : 0);
    static_cast<void>(notified);
  } else {
    WD_LOG("ble: status did not encode");
  }
  return result;
}

// One BLE sync window — PROTOCOL.md §4, end to end.
//
// Called only after core::sync_policy has granted the window and
// core::noteSyncWindowOpened() has already spent the hour. Nothing in here asks
// whether a window may open; by this point that is settled.
//
// A function rather than another section of setup() because of the guard. The
// radio must be torn down before the wake goes anywhere near sleep, and the scope
// that bounds its lifetime is easier to see — and harder to accidentally extend
// with a later edit — as a function body than as a brace pair in the middle of a
// long one.
void runSyncWindow(PersistedState& persist) {
  // §3.2's read path, assembled from RTC-backed state alone: no ADC read, no
  // clock read. It is what a phone that reads Status before writing anything
  // gets, which is what makes the app's diagnostic screen work.
  uint8_t status_payload[core::kStatusPayloadLength] = {};
  const core::Status previous =
      core::statusFromSyncState(persist.sync, persist.battery_percent,
                                persist.battery_filter.primed(), kFirmwareBuild);
  if (!core::encodeStatus(status_payload, sizeof(status_payload), previous)) {
    return;
  }

  // From here the radio is up, and the destructor at the end of this scope is
  // what turns it off — on this path, on every early return below, and on a
  // throw. There is no manual teardown to forget.
  board::ble::Session radio(status_payload, sizeof(status_payload));
  if (!radio.ok()) {
    // §6.1: "BLE stack fails to init — log, skip the window, sleep normally. A
    // radio that will not start must never cost a tick." The hour is already
    // spent, so a stack that never starts costs one window an hour and not a
    // retry storm.
    WD_LOG("ble: window did not open");
    return;
  }

  // Bringing the stack up is itself forward progress, and Law 2 permits a feed
  // here for the reason it forbids one inside a wait: the host has come up, the
  // controller has synced and the watch is advertising — all observable, all
  // completed, none of it a loop the watchdog is being blinded to. Without this
  // the advertising phase would be the tail of an interval that also had to carry
  // NimBLE's init, and §5.1's 6 s would not be 6 s.
  //
  // It is here rather than inside the Session constructor on purpose:
  // esp_task_wdt_reset() feeds only the calling task and returns ESP_ERR_NOT_FOUND
  // for any other, so every feed has to be on the task that owns the wake. Keeping
  // all of them in this function is what makes that checkable by reading it.
  board::power::feedWatchdog();

  // One loop over every non-terminal event, rather than a connect step followed by
  // a write loop. §4 is why there is a loop at all: "The phone pushes the time
  // once per connection, on every connection. A second push in the same
  // connection happens only if the first one failed." So a write is not
  // necessarily the last thing that happens, and a watch that answered one and
  // then hung up would drop the retry of exactly the exchange that had just gone
  // wrong — but only a write that *failed*, which is the condition §4 attaches and
  // the loop originally did not apply. A successful write is answered once and the
  // window ends; core::SyncWindow::noteWriteResult() holds that rule, and without
  // it a phone that kept pushing after result=0 held the radio to the 12 s cap and
  // committed twelve RTC writes where §4 asked for one. StillWaiting is why it is
  // one loop rather than two steps: any wait can
  // come back with nothing to report, so every arm of this has to be able to go
  // round again, and a `while (event == TimeWritten)` would fall out of the window
  // at the first such wake.
  //
  // **It is bounded by time, not by a retry count, and that is the strong form.**
  // core::SyncWindow checks §5.1's 12 s cap before it looks at a single signal, so
  // nothing a peer does — writing forever, connecting late, saying nothing at all
  // — reaches this loop as anything but a terminal event by then; test_sync_window
  // drives that exhaustively, including from a caller that wakes early on every
  // pass. The two feeds inside are Law 2-legal for the same reason the one above
  // is: each follows a completed §5.1 progress point, never a wait. StillWaiting
  // is not one, gets no feed, and so is covered twice over — by the cap, and by
  // the watchdog behind it.
  uint8_t writes = 0;
  core::SyncWindowEvent event = radio.wait();

  for (;;) {
    if (event == core::SyncWindowEvent::StillWaiting) {
      // The wait came back early with nothing on it and the phase still has time.
      // Ask again — no feed, because nothing progressed, and core::SyncWindow only
      // says this while waitMs() is non-zero, so the next call blocks rather than
      // spins.
      event = radio.wait();
      continue;
    }

    if (event == core::SyncWindowEvent::Connected) {
      // §5.1's first progress point, and the reason wait() hands control back here
      // rather than looping internally: this feed is genuine forward progress — a
      // central is actually on the link — and it must happen on the task the
      // watchdog is subscribed to, which is this one and not NimBLE's host task.
      board::power::feedWatchdog();
      event = radio.wait();
      continue;
    }

    if (event != core::SyncWindowEvent::TimeWritten) {
      // Completed, Disconnected, TimedOut or Capped. §6.1 makes all of them
      // ordinary endings; Completed and Disconnected are the two that mean the
      // exchange actually worked, and which of the pair it is says whether the
      // phone let go or had to be cut off.
      break;
    }

    ++writes;

    // Decode, clock, record, answer — answerTimeWrite() feeds the watchdog at
    // §5.1's second progress point on the way through.
    const core::SyncResult result = answerTimeWrite(radio, persist, /*status_flags=*/0);

    // §4's completion rule, applied where a host test can reach it. `Ok` means the
    // exchange is finished and the window ends at the next thing that happens;
    // anything else leaves the retry available, which is what the loop is for.
    // Before the next wait, so that no path through this block can reach wait()
    // without having reported the outcome.
    radio.noteWriteResult(result);

    // And then the rest of §6.1's sentence: "then let the phone disconnect or time
    // out. Do not hang up mid-notification."
    //
    // This wait is not politeness, it is the difference between the feature
    // working and only appearing to. A notification is fire-and-forget — NimBLE
    // queues it and returns, and only an *indication* waits for an
    // acknowledgement — so leaving here would run ~Session() and tear the
    // controller down with the packet still in flight. §4 makes the notify with
    // result == 0 the one thing that resets the phone's backoff and health
    // counters, and nothing else does. The watch would set its clock, log
    // result=0, and the phone would learn nothing and back off: both ends looking
    // correct in isolation, which is the failure mode §4's ordering rules exist
    // to prevent.
    //
    // Bounded by the same §5.1 numbers as everything else — the phase clock
    // restarted at the write, so this is at most the 4 s idle timeout and never
    // past the 12 s cap. In practice the phone hangs up in ~100-300 ms and this
    // returns Disconnected well inside the exchange §5.3 already budgets.
    event = radio.wait();
  }

  // Every ending arrives here, and §6.1 makes all of them ordinary: nobody came,
  // nobody wrote, the central left, a phase timed out, the cap expired. None of
  // them touches core::health, and a window that reached no one records nothing —
  // §3.2's read path wants the last *sync*, and that was not one.
  WD_LOG("ble: window closed after %u write(s) (event %d)", writes, static_cast<int>(event));
  static_cast<void>(writes);
}

// ── the find-phone session ──────────────────────────────────────────────────
//
// PROTOCOL.md §4.1, end to end. The same radio guard as runSyncWindow(), driven
// by core::FindSession instead of core::SyncWindow, and the one path in this
// firmware that stays awake for longer than a panel refresh: up to two minutes,
// at radio current, because the wearer asked it to.
//
// Called only after core::sync_policy has granted the radio — the same gates as
// a sync window, evaluated with user_requested set — and after
// core::noteSyncWindowOpened() has spent the hour. Nothing here asks whether the
// radio may come up.
//
// ── The watchdog, which is where this could go wrong ────────────────────────
//
// Two minutes is twelve watchdog periods, and Law 2 forbids feeding from inside a
// wait. The resolution is the one PROTOCOL.md §5.1 already uses for the sync
// window, applied more times: core::FindSession never hands out a wait longer
// than kFindRoundMs (5 s, under §5.1's 6 s bound), and every wait ends in either
// a signal the loop acts on or a RoundElapsed the loop acts on — a panel redraw,
// which is a completed step. The feed comes *after* that step, never inside the
// wait, so a hang anywhere in the loop — the radio stack, the panel's BUSY line,
// the I2C write inside answerTimeWrite() — is still caught by the same 10 s
// watchdog that catches it on an ordinary wake. StillWaiting is the one event
// that feeds nothing, for the same reason it feeds nothing in the sync window.
//
// ── What it costs, plainly ──────────────────────────────────────────────────
//
// A search that finds nothing is ~120 s of advertising plus ~24 partial redraws:
// about 0.4 mAh. A search that finds the phone at once and is then left to run
// out holds a live link with the CPU awake for the whole cap: about 1.3 mAh, or
// ~14 % of a day's allowance, in one press. The realistic search — phone found,
// Back pressed or the alarm silenced inside half a minute — is a tenth of that.
// docs/power-budget.md carries the row; it is user-initiated, bounded by the cap,
// and refused on a low battery, which is what makes it affordable at all.
//
// `base` is the frame the wake would otherwise have drawn — the Find phone
// screen, since the press that opened it moved the UI there — and each redraw is
// that frame with the live counters filled in.

// The Menu pin's level, for core::FindSession to confirm a press against
// kFindButtonSettleMs after its edge. One digitalRead per wake of the find loop —
// the loop wakes on events and deadlines, never to look at this — and the reason
// it is a function pointer rather than a call inside board::ble is that the radio
// module has no business knowing which pin the wearer's thumb is on.
bool menuHeld() { return board::buttons::isPressed(core::ButtonId::Menu); }

// The watch's side of §3.3: the current alarm mode, for the phone that just
// subscribed or for the phone that is already ringing when the wearer flips it.
void notifyFindMode(board::ble::Session& radio, const core::FindSession& session) {
  uint8_t frame[core::kFindPayloadLength] = {};
  if (!core::encodeFindMode(frame, sizeof(frame), session.sound())) {
    return;
  }
  const bool heard = radio.notifyFind(frame, sizeof(frame));
  WD_LOG("find: mode sound=%d notified=%d", session.sound() ? 1 : 0, heard ? 1 : 0);
  static_cast<void>(heard);
}

core::FindOutcome runFindSession(PersistedState& persist, const app::Snapshot& base) {
  // §3.2's read path with the flag set, so even a phone that reads Status before
  // writing learns what kind of window it walked into.
  uint8_t status_payload[core::kStatusPayloadLength] = {};
  core::Status previous =
      core::statusFromSyncState(persist.sync, persist.battery_percent,
                                persist.battery_filter.primed(), kFirmwareBuild);
  previous.flags = core::kStatusFlagFindPhone;
  if (!core::encodeStatus(status_payload, sizeof(status_payload), previous)) {
    return core::FindOutcome::RadioFailed;
  }

  // The same guard as the sync window: advertising stops on every exit from this
  // scope, and deep sleep drops the radio after it.
  board::ble::Session radio(status_payload, sizeof(status_payload));
  if (!radio.ok()) {
    // §6.1: a radio that will not start must never cost a tick — and the screen
    // says so, because the wearer is standing there waiting for a phone to ring.
    WD_LOG("find: radio did not open");
    return core::FindOutcome::RadioFailed;
  }
  // The stack came up and the watch is advertising: §5.1's progress point (a),
  // and the same feed runSyncWindow() takes for the same reason.
  board::power::feedWatchdog();

  core::FindSession session;

  // The wearer's two buttons. Back is the way out; Menu flips the phone's alarm
  // between vibration and vibration-plus-tone (§4.1). Both are delivered as GPIO
  // edges into the same event group the wait blocks on, so the loop wakes on them
  // within a millisecond instead of at the next round — and without ever polling
  // a pin, which is what Law 1 would otherwise have to forbid here. Menu's edge
  // is confirmed against the pin kFindButtonSettleMs later (core::FindSession),
  // because the press that started this very search may still be releasing, and
  // a tactile switch bounces on the way up. Both are detached before the radio
  // scope ends, because deepSleep() re-arms the same pins as ext1 wake sources.
  board::buttons::attachPressInterrupt(core::ButtonId::Back, &board::ble::Session::requestAbort);
  board::buttons::attachPressInterrupt(core::ButtonId::Menu,
                                       &board::ble::Session::requestSoundToggle);

  for (;;) {
    const core::FindEvent event = radio.findWait(session, &menuHeld);

    if (event == core::FindEvent::StillWaiting) {
      // Nothing progressed. Wait again and, above all, do not feed.
      continue;
    }
    if (event == core::FindEvent::Ended) {
      break;
    }

    switch (event) {
      case core::FindEvent::Connected:
        // A central is on the link: §5.1's progress point (b). The feed is below,
        // after the redraw that shows it.
        break;

      case core::FindEvent::TimeWritten: {
        // §4 verbatim — decode, clock, record, answer — with FIND_PHONE, and
        // FIND_SOUND if the wearer has asked for the tone, in the answer. §4.1:
        // the result is recorded but decides nothing here; the phone rings
        // whatever it was.
        const core::SyncResult result = answerTimeWrite(radio, persist, session.statusFlags());
        static_cast<void>(result);
        session.noteStatusNotified();
        break;
      }

      case core::FindEvent::FindSubscribed:
        // §4.1: the phone can hear the mode from here on, so it is told the
        // current one — which delivers a Menu press that landed before the
        // subscription instead of losing it.
        notifyFindMode(radio, session);
        break;

      case core::FindEvent::SoundToggled:
        // A confirmed Menu press. The phone hears it now if it is subscribed, the
        // screen shows it below, and the next Status carries it if the link has
        // to be rebuilt.
        notifyFindMode(radio, session);
        break;

      case core::FindEvent::FindWritten: {
        // The phone's side of "found". Whether the bytes mean that is core's
        // decision; a rejected frame is ignored and the search goes on (§3.3).
        uint8_t payload[core::kFindPayloadLength] = {};
        const size_t length = radio.copyFindWrite(payload, sizeof(payload));
        const core::SyncResult result = core::decodeFindWrite(payload, length);
        session.noteFindWrite(result);
        WD_LOG("find: dismiss write result=%d", static_cast<int>(result));
        break;
      }

      case core::FindEvent::Disconnected:
        // The phone let go, or the link was lost. Either way the watch is looking
        // again, and advertising has to be asked for — see advertiseOnDisconnect
        // in board/ble.cpp for why it does not restart on its own.
        if (!radio.restartAdvertising()) {
          WD_LOG("find: advertising did not restart");
        }
        break;

      case core::FindEvent::RoundElapsed:
        // Nothing happened for a round. The redraw below is what the round is for.
        break;

      case core::FindEvent::Ended:
      case core::FindEvent::StillWaiting:
        break;  // handled above
    }

    // Every non-terminal event ends the same way: show the wearer where the search
    // is, then feed. The redraw is a completed panel refresh — the same step the
    // ordinary wake feeds after — and it is what makes the feed after it a feed
    // at a progress point rather than one from inside a wait. It also costs a
    // partial refresh only when the two lines actually changed: paint() hashes
    // the frame first, so a rejected Find write that changed nothing on screen
    // costs no panel time at all.
    //
    // The un-fed interval this loop can reach is the round (5 s) plus the redraw:
    // ~0.3 s for the partial refresh that almost every round is, ~2 s on the round
    // that core::refresh_policy's ghosting cadence turns into a full one. ~7 s
    // against the 10 s watchdog, and the margin is the same shape as §5.1's — a
    // longer kFindRoundMs spends it, which is why find_session.h pins the round
    // under kAdvertiseTimeoutMs rather than under the watchdog itself.
    app::Snapshot frame = base;
    frame.find_live = true;
    frame.find_phase = session.phase();
    frame.find_sound = session.sound();
    frame.find_outcome = core::FindOutcome::InProgress;
    frame.find_attempts = session.attempts();
    frame.find_elapsed_s = core::FindSession::elapsedSeconds(radio.elapsedMs());
    paint(persist, frame, /*elapsed_minutes=*/0, /*force_full=*/false, /*first_boot=*/false);
    board::power::feedWatchdog();
  }

  board::buttons::detachPressInterrupt(core::ButtonId::Menu);
  board::buttons::detachPressInterrupt(core::ButtonId::Back);

  // §4.1: the phone stops ringing when the link ends, and it must hear a
  // disconnect rather than a supervision timeout, or the alarm outlives the search
  // by several seconds. Bounded, event-driven, and it does not matter which
  // ending brought us here — a link that is already down returns at once.
  const bool dropped = radio.hangUp();
  WD_LOG("find: ended outcome=%d attempts=%u after %lu ms (link dropped %d)",
         static_cast<int>(session.outcome()), session.attempts(),
         static_cast<unsigned long>(radio.elapsedMs()), dropped ? 1 : 0);
  static_cast<void>(dropped);
  return session.outcome();
}

}  // namespace

// ── RTC_NOINIT_ATTR, not RTC_DATA_ATTR, and the difference is load-bearing ───
//
// Both attributes put a variable in RTC slow memory, which the RTC power domain
// keeps alive through deep sleep. Only one of them survives a *reset*:
//
//   .rtc.data    (RTC_DATA_ATTR)   CONTENTS, ALLOC, LOAD  <- in the image
//   .rtc_noinit  (RTC_NOINIT_ATTR) ALLOC                  <- never written
//
// `.rtc.data` is a loadable segment. The bootloader restores it from flash on
// every boot that runs the bootloader — which a deep-sleep wake skips, and a
// panic, a watchdog reset and a brownout do not. So with RTC_DATA_ATTR the minute
// tick worked perfectly and every fault path silently reset both blocks to their
// initialisers, which is precisely the case they exist for:
//
//   * core::health counts *consecutive* faults and escalates at three. Wiped on
//     each fault reset, the count never passed one, so Safe and Recovery mode
//     were unreachable by any reset-type fault — the escalation ladder was dead
//     code for its only purpose.
//   * SyncState::minutes_since_window is spent by noteSyncWindowOpened() *before*
//     the radio comes up, specifically so that a window dying to a watchdog reset
//     still costs the full hour. Wiped, it came back reading "due now", so a
//     fault inside the window reopened the window ~60 s later into the same
//     fault: ~10 s of watchdog timeout at radio current plus a reboot with a
//     forced full refresh, every ~70 s. That is ~130 mAh/day against a 200 mAh
//     cell, unbounded, with nothing counting to three to stop it.
//   * AccelState::config_attempts is incremented before the ~0.85 s upload for
//     the same reason (docs/power-budget.md item 7) and had the same hole.
//
// Both blocks move, because both carry state whose whole job is to survive the
// thing that was erasing it. Neither needs new validation: `persistValid()` and
// `core::isPersistedStateValid()` already magic- and version-check before any
// field is trusted, which is exactly the contract noinit storage requires — and
// those checks are only now doing real work, since `.rtc.data` used to hand them
// a freshly-zeroed block on every boot that was not a deep-sleep wake.
//
// Nothing here may acquire a runtime initialiser. Both types are aggregates whose
// implicit default constructors are constexpr, so they are statically
// initialised and the linker's NOLOAD on `.rtc_noinit` drops the bytes; no
// constructor runs at boot to overwrite what the last run left. Verified on the
// built ELF — `.rtc_noinit` is ALLOC-only and neither symbol appears in an
// `.init_array` entry. **If you give either struct a user-declared constructor,
// check that again**, because a dynamic initialiser here would restore the defect
// above in a form that no longer looks like a storage bug.
RTC_NOINIT_ATTR PersistedState g_persist;
RTC_NOINIT_ATTR core::HealthState g_health;

void setup() {
  // Before the diagnostics, and that ordering is the whole lesson of the defect
  // this call fixes. deepSleep() latches every pad on the way down and the latch
  // survives every reset but a power-on, so until it is released U0TXD is frozen —
  // Serial.begin() would succeed and emit nothing, and the firmware would look
  // hung with no way to see that it was not. Releasing first is what keeps the
  // channel that reports a fault from being part of the fault.
  board::power::releaseSleepHold();

  WD_DIAG_BEGIN();

  // Then, before anything that can wedge. A reset from here is a fault that
  // core::health counts and escalates.
  board::power::startWatchdog();

  const core::ResetReason reason = board::power::resetReason();
  const core::RunMode mode = core::beginBoot(g_health, reason);
  const core::WakeSource source = board::power::wakeSource();
  const core::ButtonId wake_button = board::power::wakeButton();

  if (!persistValid(g_persist)) {
    WD_LOG("persist: reinitialising (magic=%08X ver=%u)", g_persist.magic, g_persist.version);
    persistInit(g_persist);
  }

  WD_LOG("boot %u reason=%d wake=%d mode=%d faults=%u", g_health.boot_count,
         static_cast<int>(reason), static_cast<int>(source), static_cast<int>(mode),
         g_health.consecutive_faults);

  core::WakeContext context;
  context.mode = mode;
  context.battery = g_persist.battery_level.level();
  context.minutes_since_battery_sample = g_persist.minutes_since_battery_sample;
  context.accel_features_enabled = kStepCounterEnabled;
  context.ui_active = g_persist.ui.screen != core::Screen::Watchface;
  context.minutes_since_sync_window = g_persist.sync.minutes_since_window;
  // Always false here, and that is the ordering trap rather than an oversight.
  // board::power::wakeButton() is available this early, but *which menu item the
  // press activates* is not: it depends on g_persist.ui, and the press has not
  // been dispatched yet — core::handleButton() runs a long way below. So this
  // call decides the scheduled window only, and a request made on this wake is
  // put through core::evaluateSyncWindow() a second time after the dispatch. See
  // the radio section near the end of setup().
  context.sync_requested = false;

  const core::WakePlan plan = core::routeWake(source, context);

  // ── the bus ───────────────────────────────────────────────────────────────
  // Opened unconditionally, even when the plan calls for no device reads: the
  // PCF8563's tick has to be re-armed and its interrupt flag cleared on every
  // wake. Skipping that leaves INT asserted, and since deep-sleep ext0 triggers on
  // that level the watch would wake the instant it slept.
  core::DateTime now;
  bool time_valid = false;
  bool tick_armed = false;
  uint32_t raw_steps = 0;
  bool raw_steps_ok = false;
  uint16_t tick_seconds = plan.next_tick_seconds;

  {
    board::i2c::Session bus;
    if (bus.ok()) {
      if (plan.need_rtc) {
        const board::rtc::ReadResult reading = board::rtc::read();
        now = reading.time;
        time_valid = reading.valid;
      }

      // Probe, maybe configure, maybe park, maybe read. Every "whether" in that
      // sentence belongs to core::accel_policy — including the one that matters
      // most, which is when to stop paying for a ~0.85 s configuration upload that
      // is not working. Each step here is a straight-line response to an action
      // the policy returned; main.cpp chooses nothing.
      if (plan.need_accel) {
        core::AccelAction action = core::accelBeginWake(g_persist.accel);

        if (action == core::AccelAction::Probe) {
          action = core::accelPlan(g_persist.accel, board::accel::probe());
        }

        if (action == core::AccelAction::Configure) {
          // The attempt is already recorded, so a reset partway through this call
          // does not buy the sensor another go.
          const bool configured = board::accel::configure();
          action = core::accelAfterConfigure(g_persist.accel, configured);
          WD_LOG("accel: configure %s (attempt %u/%u)", configured ? "ok" : "FAILED",
                 g_persist.accel.config_attempts, core::kMaxAccelConfigAttempts);
        }

        if (action == core::AccelAction::Suspend) {
          // We have given up on the sensor, so it must stop costing its ~14 uA —
          // a fifth of the sleep floor — for a feature that is now off. This one
          // write can NACK, hence reporting the result back: a park that did not
          // happen is retried on a later wake rather than silently written off.
          const bool parked = board::accel::suspend();
          core::accelAfterSuspend(g_persist.accel, parked);
          WD_LOG("accel: gave up after %u config attempts — suspend %s (%u/%u)",
                 g_persist.accel.config_attempts, parked ? "ok" : "FAILED",
                 g_persist.accel.suspend_attempts, core::kMaxAccelSuspendAttempts);
        }

        if (action == core::AccelAction::Read) {
          raw_steps_ok = board::accel::readStepCount(raw_steps);
        }
      }

      tick_armed = board::rtc::armTick(tickMinutes(tick_seconds));
    }
    // The bus closes here, on every path out of this scope.
  }

  if (!tick_armed) {
    WD_LOG("rtc: tick not armed — relying on the timer backstop");
  }

  // ── battery ───────────────────────────────────────────────────────────────
  if (plan.need_battery) {
    const uint16_t millivolts = board::battery::readMillivolts();
    if (millivolts != 0) {
      const uint16_t filtered = g_persist.battery_filter.update(millivolts);
      g_persist.battery_percent = core::percentFromMillivolts(filtered);
      g_persist.battery_level.update(g_persist.battery_percent);
    }
    g_persist.minutes_since_battery_sample = 0;
  }

  // ── UI ────────────────────────────────────────────────────────────────────
  // What to believe about elapsed time when the PCF8563 cannot be trusted is a
  // decision, and it lives in core::time_model where a test can reach it. Every
  // accumulating counter below is driven by this one number.
  const uint32_t elapsed =
      core::elapsedMinutes(g_persist.last_time, now, time_valid, tick_seconds);
  g_persist.minutes_since_battery_sample += elapsed;

  // The sync window's hourly timer, advanced from the same measured-or-inferred
  // elapsed as the battery schedule above. Sharing that number is the point: an
  // untrustworthy clock degrades this counter exactly the way it degrades every
  // other one — it keeps advancing at the tick rate instead of freezing, so a
  // watch with a dead PCF8563 can still reach the window that would set it.
  // Saturating, so a month in a drawer cannot wrap it back to "just synced".
  core::advanceSyncTimer(g_persist.sync, elapsed);

  // Freshness first, and on every wake — including the ones that got nothing.
  // plan.need_accel is what makes "missed" mean missed: a wake that never intended
  // to read the sensor (Safe mode, Recovery, feature off) is evidence of nothing
  // and must not age the count, or an unrelated fault would show up as a second,
  // false symptom on the watchface.
  core::noteStepWake(g_persist.steps, plan.need_accel, raw_steps_ok, elapsed);

  // Interpreting the sensor's raw total — deltas, restarts, midnight, garbage — is
  // core's job. board/ only handed us a number.
  if (raw_steps_ok) {
    const core::StepUpdate step_update =
        core::updateSteps(g_persist.steps, raw_steps, now, time_valid, elapsed);
    if (step_update.baseline_dropped) {
      // Not an error path: the gap was too large to credit, so the counter
      // re-baselines from the next reading rather than staying stuck for ever.
      WD_LOG("steps: baseline dropped after %u implausible readings",
             core::kMaxConsecutiveRejections);
    }
  }

  bool ui_changed = false;
  bool sync_requested = false;
  bool find_requested = false;
  // Consumed under plan.need_display below, which is safe because every wake that
  // sets run_ui is a button wake and core::routeWake() sets need_display on all of
  // them. A router that ever separated the two would drop this flag and repaint
  // the swap as a partial — the ghosting case it exists to avoid.
  bool theme_changed = false;
  if (plan.run_ui && wake_button != core::ButtonId::None) {
    // Asked *before* the dispatch: which item a press activates is only knowable
    // from the state as it was when the button went down, and handleButton() is
    // about to move the screen to App and lose it. plan.run_ui is only ever set
    // for a button wake, so this is structurally confined to the one wake source
    // PROTOCOL.md §5.1 says a user request can arrive on. Two items ask for the
    // radio and they ask for different things — a sync window, or a find session
    // (§4.1) — so both are named here and the radio section below does two
    // different things with them.
    const uint8_t activated = core::activatedMenuItem(g_persist.ui, wake_button);
    sync_requested = activated == core::kSyncMenuIndex;
    find_requested = activated == core::kFindPhoneMenuIndex;
    // Before the dispatch for the same reason, and through the same function
    // core::handleButton() consults, so the item that does not open an app and
    // the item whose flag is flipped cannot become two different items. An
    // assignment rather than a branch: nothing here decides anything.
    const core::ThemeChange theme =
        core::themeAfterButton(g_persist.ui, wake_button, g_persist.inverted);
    g_persist.inverted = theme.inverted;
    theme_changed = theme.changed;
    ui_changed = core::handleButton(g_persist.ui, wake_button);
  } else {
    // Not a button wake: age the idle timer so a menu left open returns to the
    // watchface on its own instead of keeping the UI path alive indefinitely.
    ui_changed = core::tickIdle(g_persist.ui, static_cast<uint16_t>(elapsed > UINT16_MAX
                                                                       ? UINT16_MAX
                                                                       : elapsed));
  }
  static_cast<void>(ui_changed);  // the content hash, not this flag, gates the redraw

  // The find session's gate, decided *before* the first frame so that a refusal is
  // what the frame shows. It is the sync window's gate — core::evaluateSyncWindow()
  // with user_requested set, the same function the Sync item goes through — so no
  // rule is weakened for the more expensive of the two: Safe and Recovery refuse,
  // a Low or Critical battery refuses, and the interval is the one gate a request
  // overrides. Evaluated against the battery as it stands now, after any sample
  // this wake took.
  bool find_session = false;
  if (find_requested) {
    core::SyncContext request;
    request.mode = mode;
    request.battery = g_persist.battery_level.level();
    request.minutes_since_window = g_persist.sync.minutes_since_window;
    request.user_requested = true;

    const core::SyncDecision decision = core::evaluateSyncWindow(request);
    find_session = decision.open;
    // InProgress when the session is about to run, the refusal's name when it is
    // not. Written before the frame below reads it.
    g_persist.find_outcome = core::findOutcomeForGate(decision.gate);
    if (!find_session) {
      WD_LOG("find: refused (gate %d)", static_cast<int>(decision.gate));
    }
  }

  // ── draw, but only if the pixels would actually differ ─────────────────────
  if (plan.need_display) {
    app::Snapshot snapshot = snapshotFrom(g_persist, now, time_valid, mode);
    if (find_session) {
      // The first frame of a search — "searching, 0:00, try 1" — painted before
      // the radio comes up, so the wearer sees the press land at once rather
      // than after the stack has initialised.
      snapshot.find_live = true;
      snapshot.find_phase = core::FindPhase::Searching;
      snapshot.find_attempts = 1;
      snapshot.find_elapsed_s = 0;
    }
    // theme_changed joins plan.force_full_refresh rather than replacing it: both
    // are the "cases the policy cannot see" core::decideRefresh() documents, and
    // this one is a frame in which every pixel transitions. A partial refresh of
    // that is the worst ghosting case the panel has, so the flip costs one full
    // refresh: 0.011 mAh against the ~0.0030 mAh partial this press would have
    // paid anyway — app::compose() hashes the theme, so the alternative was a
    // partial repaint and never a skip. ~+0.008 mAh, on a press the wearer made
    // deliberately. See core::ThemeChange and docs/power-budget.md.
    paint(g_persist, snapshot, elapsed, plan.force_full_refresh || theme_changed,
          source == core::WakeSource::PowerOn);
  }

  if (time_valid) {
    g_persist.last_time = now;
  }

  // Real progress has been made; this is a legitimate place to feed the watchdog.
  // It is also what makes the radio window below a fresh un-fed interval, which
  // is the assumption PROTOCOL.md §5.1's arithmetic rests on: 6 s of advertising
  // measured from a watchdog that was just fed stays comfortably inside the 10 s
  // timeout, whereas 6 s measured from the top of the wake would have to carry a
  // full panel refresh with it.
  board::power::feedWatchdog();

  // ── the radio ──────────────────────────────────────────────────────────────
  // The single most expensive thing this firmware does — one window is ~0.031 mAh
  // against a 9.5 mAh/day allowance, and a find session up to forty times that —
  // and the only place the radio is ever touched.
  //
  // Three grants, because of the ordering trap noted at routeWake() above:
  //
  //   * plan.need_ble is the scheduled window. routeWake() evaluated §5.1's gates
  //     from the state at the top of the wake, which is the same staleness every
  //     other flag in the plan carries.
  //   * a Sync press is evaluated here, after the UI dispatch, because the press
  //     had not been dispatched when routeWake() ran and the item it selected was
  //     therefore unknowable. It goes through core::evaluateSyncWindow() — the
  //     same function routeWake() uses, not a copy of it — so no gate is
  //     bypassed, weakened or duplicated. It is also evaluated against the
  //     battery level as it stands *now*, after any sample this wake took.
  //   * a Find phone press was evaluated above, before the frame, through the
  //     same function again. It outranks a scheduled window that happens to be
  //     due on the same wake: a find session that reaches the phone performs the
  //     §4 exchange anyway (§4.1), so the window would be redundant.
  //
  // The alternative — latching the request into SyncState and honouring it on the
  // next wake — was rejected. §5.1 says the Sync item "opens a window
  // immediately", a wearer who pressed Sync and saw nothing happen for a minute
  // would reasonably press it again, and core::SyncContext::user_requested is
  // deliberately not persisted precisely so that a request cannot outlive the
  // wake it arrived on.
  if (find_session) {
    // Before the radio, never after — the same rule as the sync window's, for the
    // same reason. A find session spends the hour (§4.1): it performs the sync
    // exchange if a phone turns up, and a wearer pressing Find phone every minute
    // must not be able to run the radio more than the schedule would.
    core::noteSyncWindowOpened(g_persist.sync);

    const core::FindOutcome outcome =
        runFindSession(g_persist, snapshotFrom(g_persist, now, time_valid, mode));
    g_persist.find_outcome = outcome;
    // Where the wearer lands: the watchface after a timeout, the menu after Back,
    // and the Find phone screen — with its message — after everything else.
    core::applyFindOutcome(g_persist.ui, outcome);

    // The search may have outlived the minute. The PCF8563 fired its tick while
    // the radio was up and the flag is still set, so INT is still low — and
    // deep-sleep ext0 triggers on that level, which would wake the watch the
    // instant it slept. Re-arming clears the flag, exactly as the top of every
    // wake does. The clock is re-read on the same bus session so the frame below
    // shows the time as it is now rather than as it was two minutes ago.
    // g_persist.last_time keeps the top-of-wake reading on purpose: the next wake
    // then measures the whole session as elapsed, and every counter that rides on
    // elapsed — battery, idle, the sync timer — sees the minutes the search took.
    {
      board::i2c::Session bus;
      if (bus.ok()) {
        const board::rtc::ReadResult reading = board::rtc::read();
        if (reading.valid) {
          now = reading.time;
          time_valid = true;
        }
        if (!board::rtc::armTick(tickMinutes(tick_seconds))) {
          WD_LOG("rtc: tick not re-armed after the find session");
        }
      }
    }

    // The frame after the search: the menu, the watchface, or the Find phone
    // screen with how it ended. Painted with the radio idle but the controller
    // still enabled — board/ble.h says what that costs (one partial refresh's
    // worth of controller time, ~0.003 mAh) and docs/power-budget.md carries it
    // inside the session's row. Not a choice: the frame depends on how the
    // session ended, so it cannot be painted before the session.
    paint(g_persist, snapshotFrom(g_persist, now, time_valid, mode), /*elapsed_minutes=*/0,
          /*force_full=*/false, /*first_boot=*/false);
  } else {
    bool open_window = plan.need_ble;
    if (!open_window && sync_requested) {
      core::SyncContext request;
      request.mode = mode;
      request.battery = g_persist.battery_level.level();
      request.minutes_since_window = g_persist.sync.minutes_since_window;
      request.user_requested = true;

      const core::SyncDecision decision = core::evaluateSyncWindow(request);
      open_window = decision.open;
      if (!open_window) {
        WD_LOG("ble: sync refused (gate %d)", static_cast<int>(decision.gate));
      }
    }

    if (open_window) {
      // Before the radio, never after. The hour is spent when the window is
      // granted, so a window that reaches nobody — or one that dies to a watchdog
      // reset partway through — still costs the full hour. Deferring this to a
      // success path is the unbounded retry core::sync_policy exists to prevent:
      // 1440 windows a day instead of 24.
      core::noteSyncWindowOpened(g_persist.sync);
      runSyncWindow(g_persist);
    }
  }

  // Marks the run a success. Nothing else clears the fault counter, so a crash
  // before this point escalates toward Safe and then Recovery mode — including
  // one inside the window above, which is what turns a radio that wedges into a
  // watch that degrades into Safe mode and stops using it.
  core::markRunComplete(g_health);

  board::power::deepSleep(tick_seconds, kAccelWakeEnabled);
}

void loop() {
  // Unreachable by design: setup() ends in deep sleep. Arriving here means a sleep
  // path was bypassed, which is exactly the "watch stays awake and flattens the
  // battery" failure Law 1 exists to prevent. Reboot rather than spin.
  esp_restart();
}
