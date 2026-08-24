// How one BLE sync window runs: which PROTOCOL.md §5.1 timeout applies right now,
// and what the radio's signals mean. Pure logic — there is no radio in this file.
//
// `core::sync_policy` decides *whether* a window opens. This decides *how long*
// the watch may wait at each step of one that has, which is the other half of
// §5.1 and the half that is arithmetic rather than gating.
//
// ── Why this is not in board/ ────────────────────────────────────────────────
//
// It was, and it was the one part of `board::ble` that was not a pure effect: a
// running total, two deadlines and a priority order over three signals, none of
// which a host test could reach. The defect that put it here — a terminal signal
// that was never consumed, so every later wait returned instantly and a caller
// that looped span at full current until the watchdog reset it — is exactly the
// kind that a unit test finds in a second and a bench never does.
//
// ── The invariant this type exists to hold ───────────────────────────────────
//
// **No wait it hands out is longer than §5.1's timeout for the current phase,
// and none extends past the absolute cap.** The task watchdog is 10 s and Law 2
// forbids feeding it from inside a wait, so each wait has to fit under it on its
// own. §5.1's numbers are chosen for that; this type is what makes them binding
// rather than aspirational, and `test/test_sync_window/` drives every reachable
// state to prove no wait ever exceeds kAdvertiseTimeoutMs.
//
// ── Termination ─────────────────────────────────────────────────────────────
//
// The cap is checked **before** the signals, and that ordering is load-bearing
// rather than tidy. §5.1 makes the cap absolute — "regardless of any activity" —
// and checking it first is also what guarantees the window ends whatever the
// radio does: a signal the caller forgets to consume, or one that legitimately
// repeats (§4 permits a second Time write in one connection), cannot hold the
// window open past 12 s. Once a terminal event has been produced the window is
// latched: every later call returns that same event immediately and hands out a
// zero wait, so a caller that keeps asking stops blocking rather than starts
// spinning.
//
// ── Why "nothing happened yet" is a value and not an assumption ──────────────
//
// This type used to have no way to say *nothing*. Five values, all of them
// something that happened, so classify() with no new signals had to guess — and
// it guessed "the wait I handed out must have expired, therefore the phase timed
// out". That is only true if the caller blocked for the whole of waitMs(), and a
// caller cannot promise that: an event group wakes on any bit, including one a
// previous call reported and left set, and any such wake lands here with an
// elapsed nowhere near the deadline. The type then reported a §5.1 timeout that
// had not happened, silently, with the phase clock reading zero.
//
// That was found on hardware and not by the 18 tests that were meant to cover
// this file, because every one of them modelled a wait that genuinely expired —
// the assumption was baked into the tests as firmly as into the code. So the fix
// is not a guard inside the old answer, it is a sixth value: StillWaiting means
// the deadline is still in the future and nothing has happened, which is a fact
// this file can check rather than a state of the caller it has to trust. TimedOut
// now requires the deadline to have genuinely passed.
//
// The property that keeps a caller honest is that **StillWaiting is only ever
// returned when waitMs() at the same elapsed is non-zero** — there is always
// real time left to block on, so looping on it is a wait and not a spin, and the
// 12 s cap still ends it because the cap is checked first and elapsed only moves
// forwards.
//
// Time is passed in as milliseconds since the window opened, never read from a
// clock. That keeps the file pure, and it puts the millis() rollover in board/
// where the unsigned subtraction that handles it lives.
#pragma once

#include <cstdint>

#include "core/protocol.h"

namespace core {

// One step of a window. Two of these are §5.1's watchdog feed points and the
// caller must be handed control at both — see runSyncWindow() in main.cpp.
enum class SyncWindowEvent : uint8_t {
  // §5.1 progress point (a). Non-terminal: the idle-after-connect timeout starts
  // here and the advertising one is spent.
  Connected,
  // §5.1 progress point (b). Non-terminal — §6.1 requires the watch to answer
  // with a Status notification and then let the phone hang up, so the phase clock
  // restarts here rather than the window ending.
  TimeWritten,
  // Terminal. §6.1 makes this the normal ending, not a fault.
  Disconnected,
  // Terminal. The §5.1 timeout for the phase that was running expired — which
  // means the deadline has genuinely passed, not merely that no signal arrived.
  TimedOut,
  // Terminal. The §5.1 absolute session cap expired.
  Capped,
  // Non-terminal, and the only one that is not something that happened: the wait
  // came back early with nothing pending and the current phase still has time on
  // it. The caller waits again — and must **not** feed the watchdog, because no
  // §5.1 progress point has been reached and feeding here would be feeding from
  // inside a wait, which is exactly what Law 2 forbids.
  //
  // Last in the enum on purpose. The other five are printed as integers by
  // board/'s WD_LOG diagnostics and read against a running watch during
  // bring-up; renumbering them to make the list prettier would invalidate every
  // trace already captured. Terminal-ness is answered by ended(), never by
  // comparing enumerators, so nothing depends on the order.
  StillWaiting,
  // Terminal, and the only ending that is a success rather than an expiry. §4
  // defines the exchange as complete once a Time write has been answered with
  // `result == 0`, and licenses a second push in the same connection **only if
  // the first one failed**. So a further write after a successful one is not a
  // retry — there is nothing left to retry — and the window is over.
  //
  // This is not what the *normal* success looks like. A phone that behaves hangs
  // up once it has its notification, and that ends the window as `Disconnected`,
  // which is §6.1's normal ending and stays the common case. `Completed` is what
  // a phone that keeps pushing gets instead of another 8 s of radio.
  //
  // Appended, like StillWaiting, so the five original values keep the integers
  // they are logged as.
  Completed,
};

// What the radio saw since the last question. All three are level, not edge: the
// caller reports what is currently pending and this type decides what it means.
struct SyncWindowSignals {
  bool connected = false;
  bool time_written = false;
  bool disconnected = false;
};

class SyncWindow {
 public:
  // How long the caller may block before asking again, given how long the window
  // has been open. Zero once the window has ended, so a caller that keeps asking
  // stops waiting rather than starts spinning.
  uint32_t waitMs(uint32_t elapsed_ms) const;

  // What the signals pending after that wait mean. Non-const: this is where the
  // phase advances.
  //
  // `elapsed_ms` is load-bearing and not decoration: with no new signals it is
  // the *only* thing that distinguishes a phase that expired from a wait that
  // came back early, and answering TimedOut without consulting it was the defect
  // this parameter now closes.
  //
  // `Connected` and `TimeWritten` are results the caller must acknowledge by
  // clearing the underlying signal — the connect because it is level and would
  // otherwise be re-offered on every later call, the write because §4 allows a
  // second Time write in the same connection and this type therefore cannot treat
  // a repeat as a duplicate. Neither omission is a hang: the cap check above them
  // ends the window regardless. Both cost radio time, and since a signal that is
  // never consumed now yields StillWaiting rather than a false TimedOut, the cost
  // is the rest of the phase rather than an instant, silently wrong teardown.
  SyncWindowEvent classify(uint32_t elapsed_ms, const SyncWindowSignals& signals);

  // Report the outcome of the write that classify() last reported as
  // `TimeWritten`, once it has been through the decoder and the RTC.
  //
  // **This is where §4's completion rule lives, and it is deliberately not a
  // `break` in main.cpp.** "Is this window finished?" is the question this type
  // exists to answer — it owns the phase machine, the cap and the latch — and the
  // one place §4's rule could otherwise go is the one file no host test can reach.
  // The `SyncResult` is an *input* produced elsewhere (core::decodeTimeWrite plus
  // board::rtc::write); that it comes from elsewhere no more moves the decision out
  // of here than `elapsed_ms` coming from millis() does. No new coupling either:
  // §5.1's timeouts already bring core/protocol.h into this header.
  //
  // Ok does **not** end the window on the spot, and that restraint is load-bearing.
  // A notification is fire-and-forget — NimBLE queues it and returns — and since
  // `~Session()` stopped calling deinit(), the measured gap between teardown and
  // deep sleep is 0 ms. Ending here would tear the RF domain down with the packet
  // still queued, and §4 makes that notification the only thing that resets the
  // phone's backoff: the watch would set the clock, log result=0, and the phone
  // would learn nothing. So the window stays open, waitMs() keeps handing out real
  // time, and the *next* thing that happens ends it — the hang-up (`Disconnected`,
  // the normal case), a further push (`Completed`), or §5.1's idle timeout.
  //
  // Anything but Ok leaves the window exactly as it was, which is the retry §4
  // licenses and the whole reason runSyncWindow() loops.
  void noteWriteResult(SyncResult result);

  bool ended() const { return ended_; }

 private:
  uint32_t phaseDeadlineMs() const;
  SyncWindowEvent finish(SyncWindowEvent event);

  bool connected_ = false;
  bool ended_ = false;
  // Set once a Time write has been answered with SyncResult::Ok. §4 has no more
  // work after that, so a further write is an ending rather than a retry.
  bool exchange_complete_ = false;
  SyncWindowEvent end_event_ = SyncWindowEvent::Capped;
  // Milliseconds since the window opened at which the current §5.1 phase began:
  // 0 while advertising, then re-stamped at each watchdog feed point, because
  // each feed starts a fresh un-fed interval and the phase timeout is what bounds
  // it.
  uint32_t phase_started_ms_ = 0;
};

}  // namespace core
