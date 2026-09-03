// The BLE sync window — the watch's half of PROTOCOL.md, on the air.
//
// **Effects only.** This module brings the radio up, advertises, waits for the
// phone and reports what happened. It never decides *whether* a window may open:
// core::sync_policy answered that before this file is reached, and duplicating
// any part of that gate here would put the most expensive decision in the
// firmware somewhere no host test can see it.
//
// ── Why this is an RAII guard, like i2c::Session and display::Session ────────
//
// The radio is the single largest consumer on this board — hundreds of
// milliamps against a ~20 uA sleep floor — so it is also the most expensive
// thing to leak. Constructing a Session powers the stack up and starts
// advertising; its destructor stops advertising on **every** exit path,
// including an early return, an error branch and an exception. A manual
// teardown at the end of a function is skipped by exactly the path that matters,
// which is why the other two peripherals are guards and why this one has to be.
// Note that a partly-built session still tears down: the destructor asks the
// stack whether it came up, not whether the constructor finished.
//
// ── **This class requires the wake to end in deep sleep. Read this before
//    using it anywhere new.** ─────────────────────────────────────────────────
//
// The destructor stops advertising. It does **not** power the radio down —
// `board::power::deepSleep()` does, when esp_deep_sleep_start() drops the RF and
// digital domains at the hardware level. So the "radio is off" guarantee lives at
// that choke point rather than at this one, and it holds only because Law 1 makes
// deepSleep() the terminus of every path out of setup().
//
// The reason is not a preference. NimBLEDevice::deinit() panics core 0 on every
// teardown: ble_hs_stop() moves the host out of its enabled state, after which
// ble_hs_timer_reset() (ble_hs.c:464) deletes and frees the host timer instead of
// re-arming it, while GAP is still scheduling it — and the FreeRTOS timer daemon
// then services a queued command against the freed block. It needs no connection
// and reproduces on the plain advertise timeout. Disabling the controller by hand
// instead cannot be shown to be sound; ble.cpp's destructor has the evidence.
//
// **What this costs, stated plainly:** between this destructor returning and
// deepSleep() being reached, the BLE controller is still enabled. Today that is
// microseconds and the cost is nil. A change that opens a window and then does
// anything before sleeping — a confirmation screen, a retry, a second window —
// holds the controller up for that whole stretch, tens of milliamps against a
// ~20 uA floor, and **nothing in the code will say so**. Neither the compiler nor
// a host test can hold that property: it is statement ordering inside a void
// function in main.cpp, which the native environment's build_src_filter excludes
// on purpose. What the compiler does hold is the half that makes deepSleep() a
// terminus rather than a step — see the -Wreturn-type probe in board/power.cpp.
// What the bench holds is the gap between "ble: teardown 4/4" and "sleep:" in the
// WORKADAY_DIAG log, which is exactly this interval.
//
// ── The watchdog, which is the part most easily got wrong ────────────────────
//
// A BLE window looks precisely like the wait loop Law 2 forbids feeding the
// watchdog from. PROTOCOL.md §5.1 resolves that by bounding every un-fed
// interval below the 10 s task watchdog instead of feeding from inside the wait:
// advertising with nobody there is 6 s and then over, connected but idle is 4 s
// and then over, and the whole session is capped at 12 s regardless.
//
// Two consequences shape this interface:
//
//   * The wait is **event-driven** — a FreeRTOS event group with a timeout, woken
//     by the NimBLE callbacks. Not a poll, not delay(), not `while (!flag) {}`.
//   * wait() returns to the caller at the two progress points it can observe —
//     a central connected, and a Time payload arrived — because those are where
//     the caller feeds the watchdog. (§5.1 names a third, the stack coming up and
//     the controller syncing; that one is the constructor returning, so the caller
//     reaches it without a wait().) Nothing in this module touches the watchdog
//     itself: esp_task_wdt_reset() only feeds the calling task's entry, and the
//     task subscribed to the watchdog is the one that owns the wake, not the
//     NimBLE host task the callbacks run on.
//
// Every timeout comes from core/protocol.h, which PROTOCOL.md §8 makes the only
// file allowed to hold one. There are no UUID literals and no §5 durations here,
// and no timeout arithmetic either — which phase is running and how long it has
// left is core::SyncWindow's, where a host test can drive it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/find_session.h"
#include "core/sync_window.h"

namespace board {
namespace ble {

class Session {
 public:
  // Brings the stack up, publishes the service and starts advertising.
  //
  // `status` is the already-encoded §3.2 payload that a plain read reports before
  // anything has been written in this connection — core::statusFromSyncState()
  // built it out of RTC-backed state, costing neither an ADC nor a clock read.
  // This module stores those bytes and never interprets them.
  //
  // Check ok() before doing anything else. A failure here is PROTOCOL.md §6.1's
  // "BLE stack fails to init: log, skip the window, sleep normally" — a radio
  // that will not start must never cost the watch a tick.
  Session(const uint8_t* status, size_t status_length);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;

  bool ok() const { return ok_; }

  // Block until the next event, or until the §5.1 timeout that applies to the
  // current phase expires — whichever comes first, and never past the session
  // cap. Blocks on an event group; it does not poll.
  //
  // `Connected` and `TimeWritten` are the two points at which the caller feeds
  // the watchdog (§5.1) and are the reason this hands control back rather than
  // looping internally. `Disconnected`, `TimedOut` and `Capped` are terminal: the
  // window is over and the caller must stop. Asking again after one of them
  // returns the same event immediately and without blocking — it is not a fresh
  // wait and it will never become one, so a loop that ignores a terminal event is
  // a busy loop of the caller's own making rather than a stalled radio.
  //
  // `StillWaiting` is the sixth answer and the one a caller is most likely to get
  // wrong: the wait came back with nothing to report and the current §5.1 phase
  // has not expired. Call again — and **do not feed the watchdog**, because
  // nothing has progressed and a feed here is a feed from inside a wait. Doing so
  // cannot spin: core::SyncWindow only returns it while there is real time left
  // on the phase, so the next call blocks, and the 12 s cap still ends the window.
  //
  // A session that never opened (see ok()) reports `Capped` for the same reason:
  // the window is over before it started.
  core::SyncWindowEvent wait();

  // Tell the window how the write it just reported turned out, once the payload
  // has been through core::decodeTimeWrite() and, if it validated, the RTC.
  //
  // Must be called before the next wait(). §4's completion rule is what this
  // feeds: `Ok` means the exchange is done and the window ends at the next thing
  // that happens, anything else leaves the retry §4 licenses available. The rule
  // itself lives in core::SyncWindow, where a host test can drive it; this is the
  // pass-through, because the window is a member of this class and nothing else
  // can reach it.
  void noteWriteResult(core::SyncResult result);

  // Copy the bytes of the most recent TimeWritten event into `out`, at most
  // `cap` of them, and return the length the central actually wrote.
  //
  // The returned length is what arrived, which may be shorter or longer than a
  // Time payload. Hand both to core::decodeTimeWrite(): it rejects any length but
  // 12 as BadLength (§7.3) *before* dereferencing the buffer, so a truncated or
  // oversized write is reported rather than parsed.
  size_t copyTimeWrite(uint8_t* out, size_t cap) const;

  // Publish `status` as the characteristic's value and notify the central.
  //
  // Returns whether a subscribed central was there to receive it. §4 makes the
  // notification — not the connect — the definition of a successful exchange, so
  // this answer is worth recording; but the clock has already been set either
  // way, and the watch does not retry inside the window.
  bool notify(const uint8_t* status, size_t length);

  // ── The find-phone session (PROTOCOL.md §4.1) ──────────────────────────────
  //
  // The same radio, the same service and the same constructor, driven by
  // core::FindSession instead of core::SyncWindow. What differs is the wait:
  // which signals it listens for, and whose arithmetic bounds it. Everything
  // said above about the destructor and the watchdog still holds — a find
  // session is a sequence of waits each no longer than a sync window's, and the
  // caller feeds after each completed step exactly as it does in a window.
  //
  // The two waits listen on different bit sets, and that is load-bearing rather
  // than tidy: the Back press and the Find write are latched as event bits, and
  // a wait that woke on a bit its classifier never consumes would return
  // instantly on every call for the rest of the window — the spin ble.cpp's
  // wait() documents at length. So wait() blocks only on the three sync signals
  // and findWait() on all five.

  // Block until the next find-session event, or until the current round's wait
  // expires — whichever comes first. The signals handed to classify() include the
  // Back press requestAbort() latched, which is how the wearer ends a search
  // without the watch ever polling a pin. Same contract as wait(): non-terminal
  // events are consumed here, Ended is latched, StillWaiting means wait again
  // and do not feed.
  core::FindEvent findWait(core::FindSession& session);

  // Copy the bytes of the most recent FindWritten event into `out`, at most `cap`
  // of them, and return the length the central actually wrote — the same contract
  // as copyTimeWrite(), for the same reason: core::decodeFindWrite() rejects any
  // length but 4 before reading the buffer.
  size_t copyFindWrite(uint8_t* out, size_t cap) const;

  // Advertise again after a central dropped the link. advertiseOnDisconnect is
  // off for the sync window's sake (see the constructor), so a find session that
  // wants the phone back after a lost link has to ask. Returns false if the stack
  // refused, which the caller logs and the round clock rides over.
  bool restartAdvertising();

  // Terminate the link, if one is up, and wait — bounded, on the disconnect
  // event, never polling — for it to actually drop. §4.1: the phone stops ringing
  // when the link ends, and a link merely abandoned to deep sleep ends for the
  // phone as a supervision timeout seconds later, with the alarm still going.
  // Returns whether the drop was seen inside the bound.
  bool hangUp();

  // ISR-safe: latch the wearer's Back press into the signals findWait() blocks
  // on. The one way a GPIO interrupt reaches the event group; wired by
  // board::buttons::attachPressInterrupt() from main.cpp for the length of a
  // find session and detached after. Idempotent, so a bouncing contact costs
  // nothing, and harmless outside a session, where nothing waits on the bit and
  // the next constructor clears it.
  static void requestAbort();

  // Milliseconds since advertising started — the same reading findWait() hands
  // core::FindSession, exposed for the screen's elapsed counter so the two cannot
  // disagree.
  uint32_t elapsedMs() const;

 private:
  bool ok_ = false;
  // millis() when advertising started. The only clock reading this module makes;
  // core::SyncWindow is handed the difference, so the rollover is handled by the
  // unsigned subtraction here and nothing impure crosses the boundary.
  uint32_t opened_ms_ = 0;
  core::SyncWindow window_;
};

}  // namespace ble
}  // namespace board
