// How one find-phone session runs (PROTOCOL.md §4.1): how long the watch may wait
// at each step, what the radio's signals mean while it is looking for the phone,
// how many attempts it has made, and how it ended. Pure logic — no radio here.
//
// `core::sync_window` is the same idea for the hourly sync window, and the two
// are deliberately separate types rather than one with a mode flag, because the
// rules differ at every step: a sync window ends on the first disconnect and
// never outlives 12 s; a find session re-advertises after a disconnect, holds a
// live link for as long as the phone is ringing, and ends only on the wearer's
// Back press, the phone's dismissal, or a two-minute cap.
//
// ── The invariant this type exists to hold ───────────────────────────────────
//
// **No wait it hands out is longer than kFindRoundMs, which is under the 6 s
// advertising bound PROTOCOL.md §5.1 puts on every un-fed interval.** The task
// watchdog is 10 s and Law 2 forbids feeding it from inside a wait, and a search
// that lasts two minutes looks exactly like the loop Law 2 is written against. The
// resolution is the one §5.1 already uses: the session is a sequence of short
// waits, and every one of them ends in either a signal the caller acts on or a
// RoundElapsed the caller acts on — a panel redraw, which is a completed step and
// therefore a legitimate feed point. test/test_find_session/ drives every
// reachable state to prove no wait ever exceeds the bound and no session outlives
// the cap.
//
// ── Termination ─────────────────────────────────────────────────────────────
//
// The cap is checked before the signals, as in SyncWindow and for the same
// reason: §5.1 makes it absolute, and checking it first is what guarantees the
// session ends whatever the radio reports. Once ended the session is latched —
// every later classify() returns Ended and waitMs() hands out zero, so a caller
// that keeps asking stops blocking rather than starts spinning.
//
// ── Signals are level, and the caller clears what it consumes ────────────────
//
// board::ble reports what is pending and clears the bit behind each non-terminal
// event this type returns, exactly as it does for SyncWindow. A disconnect is
// therefore consumed whether or not this type believed a link was up: a stale
// disconnect bit left set would otherwise wake the caller instantly on every
// wait, and that is a spin at radio current. It is reported as Disconnected and
// costs a redraw, and it advances the attempt counter only when a link really
// was up.
//
// Time is passed in as milliseconds since the session opened, never read from a
// clock, which keeps the file pure and leaves the millis() rollover in board/.
#pragma once

#include <cstdint>

#include "core/protocol.h"
#include "core/sync_policy.h"
#include "core/ui_state.h"

namespace core {

// The redraw-and-feed cadence: how often the screen is repainted with the elapsed
// time and the attempt counter while the search runs, which is also how often the
// watchdog is fed. Not a §5 number — the phone never sees it — so it lives here
// rather than in protocol.h. What it must be is under §5.1's 6 s bound, and the
// assertion below is what keeps a future "make the counter smoother" or "redraw
// less often" from walking into a watchdog reset in either direction: shorter
// only costs refreshes, longer breaks the invariant above.
constexpr uint32_t kFindRoundMs = 5000;
static_assert(kFindRoundMs <= kAdvertiseTimeoutMs,
              "a find round is an un-fed interval and must fit under PROTOCOL.md §5.1's bound");
static_assert(kFindRoundMs > 0, "a zero-length round is a spin");

// How long after a Menu edge the pin is looked at again before the press counts
// (§4.1's sound toggle). The Menu button is read by a GPIO interrupt during a
// search, and a tactile switch bounces: several edges on the way down, and —
// worse — a burst of them on the way *up*, including the release of the very
// press that started the search. Counting edges would toggle the tone on a
// release. So an edge only starts a clock, and the press counts when the pin is
// still high once the contacts have settled. Well past any bounce and well short
// of any human press, and a bounded wait rather than a delay: waitMs() hands it
// out like any other deadline, and the round bound still holds.
constexpr uint32_t kFindButtonSettleMs = 40;
static_assert(kFindButtonSettleMs > 0 && kFindButtonSettleMs < kFindRoundMs,
              "the settle wait must be a real wait and must not lengthen a round");

// How a find session ended, or why it never started. Persisted in the caller's
// RTC-backed block so the Find phone screen can say it on the wakes after the
// session — "phone found" has to survive the deep sleep between the search
// and the wearer looking at the watch.
//
// Explicit values: the byte lives in RTC memory across builds, and a block whose
// magic and version check out must still decode to the outcome that was stored.
enum class FindOutcome : uint8_t {
  // The session is running, or is about to. Also what a wake finds persisted
  // when the previous wake died inside a session — a watchdog reset, a brownout
  // — so the screen treats a stored InProgress with no live session as
  // "interrupted" rather than as a search still going on.
  InProgress = 0,
  BackPressed = 1,       // the wearer ended it; back to the menu
  TimedOut = 2,          // kFindPhoneTimeoutMs elapsed; back to the watchface
  DismissedByPhone = 3,  // §3.3: the phone was found from the phone's side
  RadioFailed = 4,       // the BLE stack would not come up (PROTOCOL.md §6.1)
  // Refused by the battery gate before the radio was touched. **No longer
  // reachable from a press**, and kept rather than deleted: §5.1's battery gate
  // now stops the schedule and not the wearer, so a Find phone press — which is
  // always a user request — cannot close it. The value stays because the byte
  // lives in RTC memory across builds and a watch upgrading from an older one may
  // still be holding it, and the switch below stays total because a gate that
  // acquires a new way to refuse must be a compiler error here rather than a
  // silent "interrupted".
  BatteryTooLow = 5,
  NotAvailable = 6,      // refused by the run-mode gate (Safe / Recovery)
};

// What the search is doing right now, for the screen.
enum class FindPhase : uint8_t {
  Searching,  // advertising; nobody is on the link
  Connected,  // a central is on the link and the §4 exchange is in progress
  Ringing,    // Status with FIND_PHONE has gone out: the phone is making itself heard
};

// One step of a session. Every non-terminal event except StillWaiting is a
// completed step the caller may feed the watchdog after — see runFindSession() in
// main.cpp for which ones it does.
enum class FindEvent : uint8_t {
  Connected,       // a central arrived. Caller: feed, redraw.
  TimeWritten,     // twelve bytes arrived on Time. Caller: answer them as §4 requires, then feed.
  FindWritten,     // a frame arrived on Find. Caller: decode it and call noteFindWrite().
  RoundElapsed,    // kFindRoundMs passed with nothing else to report. Caller: redraw, feed.
  Disconnected,    // the link dropped. Caller: restart advertising, redraw, feed.
  Ended,           // terminal. outcome() says how.
  StillWaiting,    // the wait came back early; wait again, and do NOT feed.
  FindSubscribed,  // the phone subscribed to Find. Caller: notify the current mode (§4.1), feed.
  SoundToggled,    // a confirmed Menu press flipped sound(). Caller: notify the mode, redraw, feed.
};

// What the radio saw since the last question, plus the signals that are not the
// radio's: the wearer's Back press and Menu edge, delivered from GPIO interrupts,
// and the Menu pin's level, sampled by the caller when the wait returns.
struct FindSignals {
  bool connected = false;
  bool time_written = false;
  bool disconnected = false;
  bool find_written = false;
  bool back_pressed = false;
  // The phone wrote the CCCD on Find (§4.1). Level, cleared once reported.
  bool find_subscribed = false;
  // A rising edge on the Menu pin since the last question. Level, and always
  // consumed: classify() records it whatever else it reports, so the caller
  // clears it after every call in which it was set.
  bool menu_edge = false;
  // Whether the Menu pin reads pressed right now. Not latched — the caller
  // samples it after each wait — and only ever consulted when a press is being
  // confirmed, kFindButtonSettleMs after its edge.
  bool menu_held = false;
};

class FindSession {
 public:
  // How long the caller may block before asking again. Never more than
  // kFindRoundMs, never past the cap, no later than a pending Menu press's settle
  // deadline, zero once the session has ended.
  uint32_t waitMs(uint32_t elapsed_ms) const;

  // What the pending signals mean, in priority order: the cap, then Back, then a
  // settled Menu press, then a Find write, then the link events, then the phone's
  // subscription, then the round clock. The settled press sits ahead of the level
  // signals because waitMs() wakes the caller for it: a signal left pending could
  // otherwise pre-empt it on every pass and turn the wait into zero. Non-const:
  // this is where the phase, the attempt counter and the mode advance.
  FindEvent classify(uint32_t elapsed_ms, const FindSignals& signals);

  // The Find write classify() last reported, once it has been through
  // core::decodeFindWrite(). Ok ends the session as DismissedByPhone — the next
  // classify() returns Ended and waitMs() zero. Any rejection leaves the session
  // exactly as it was: PROTOCOL.md §3.3 says a malformed dismiss is ignored.
  void noteFindWrite(SyncResult result);

  // The Status frame carrying FIND_PHONE has been handed to the radio. Moves the
  // phase to Ringing, which is what the screen shows from here until the link
  // drops. Nothing else changes: how long the phone rings is the phone's, and the
  // session ends on the same four endings either way.
  void noteStatusNotified();

  bool ended() const { return ended_; }
  FindOutcome outcome() const { return outcome_; }
  FindPhase phase() const { return phase_; }

  // Whether the wearer has asked for the alarm tone as well as vibration (§4.1).
  // Off when a search starts; flipped by each Menu press that settles while a
  // phone is on the link; kept across a lost link so the phone that reconnects
  // resumes the same way.
  bool sound() const { return sound_; }

  // §3.2's `flags` for every Status this session answers with: FIND_PHONE always,
  // FIND_SOUND while sound() is on. One place, so the flag and the mode the
  // FindMode frames carry cannot disagree.
  uint8_t statusFlags() const;

  // How many times the watch has started listening: 1 while the first round runs,
  // +1 for every round that ends with nobody connected, +1 for every link that
  // drops. Saturates at 255 rather than wrapping back to a hopeful "try 1".
  uint8_t attempts() const { return attempts_; }

  // The elapsed time as the screen shows it, in whole seconds, saturating.
  static uint16_t elapsedSeconds(uint32_t elapsed_ms);

 private:
  FindEvent finish(FindOutcome outcome);

  bool ended_ = false;
  FindOutcome outcome_ = FindOutcome::InProgress;
  FindPhase phase_ = FindPhase::Searching;
  bool sound_ = false;
  uint8_t attempts_ = 1;
  // Milliseconds since the session opened at which the current round began:
  // re-stamped at every event the caller feeds after, so each round is a fresh
  // un-fed interval measured from a feed.
  uint32_t round_started_ms_ = 0;
  // When a Menu edge that arrived while a phone was on the link is due to be
  // confirmed against the pin, or kNoPendingPress. See kFindButtonSettleMs.
  static constexpr uint32_t kNoPendingPress = UINT32_MAX;
  uint32_t menu_settles_at_ms_ = kNoPendingPress;
};

// The outcome a refused gate reads as. Open is InProgress — the session runs —
// and IntervalNotElapsed is InProgress too, because a user request overrides the
// interval (PROTOCOL.md §5.1) and that gate can therefore never be the one that
// closed. The other two name themselves.
FindOutcome findOutcomeForGate(SyncGate gate);

// Where the wearer is once the session is over, applied to the navigation state.
//
// This is a decision about screens and it lives here rather than in main.cpp so
// a test can hold it: TimedOut goes to the watchface (the spec's "the watch
// returns to the watchface"), BackPressed to the menu (the same place Back from
// any app goes), and everything with a message to show — dismissed by the phone,
// the radio failing, a refused gate — stays on the Find phone screen until the
// wearer presses Back or the idle timeout takes them home.
void applyFindOutcome(UiState& ui, FindOutcome outcome);

}  // namespace core
