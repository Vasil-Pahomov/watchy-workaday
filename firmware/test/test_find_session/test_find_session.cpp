#include <unity.h>

#include <cstdint>

#include "core/find_session.h"

using core::FindEvent;
using core::FindOutcome;
using core::FindPhase;
using core::FindSession;
using core::FindSignals;
using core::SyncResult;

void setUp(void) {}
void tearDown(void) {}

static int eventAsInt(FindEvent event) { return static_cast<int>(event); }
static int outcomeAsInt(FindOutcome outcome) { return static_cast<int>(outcome); }
static int phaseAsInt(FindPhase phase) { return static_cast<int>(phase); }

static FindSignals nothing() { return FindSignals{}; }

static FindSignals connected() {
  FindSignals signals;
  signals.connected = true;
  return signals;
}

static FindSignals wrote() {
  FindSignals signals;
  signals.time_written = true;
  return signals;
}

static FindSignals gone() {
  FindSignals signals;
  signals.disconnected = true;
  return signals;
}

static FindSignals dismissed() {
  FindSignals signals;
  signals.find_written = true;
  return signals;
}

static FindSignals back() {
  FindSignals signals;
  signals.back_pressed = true;
  return signals;
}

// Every combination of the five level signals, for the exhaustive sweeps.
static FindSignals signalsFromBits(int bits) {
  FindSignals signals;
  signals.connected = (bits & 1) != 0;
  signals.time_written = (bits & 2) != 0;
  signals.disconnected = (bits & 4) != 0;
  signals.find_written = (bits & 8) != 0;
  signals.back_pressed = (bits & 16) != 0;
  return signals;
}

// The honest caller: block for exactly what waitMs() said, then ask. Signals are
// consumed as board::ble consumes them — the bit behind every non-terminal event
// is cleared — so this models the real loop and not an idealised one.
struct Caller {
  FindSession session;
  FindSignals pending;
  uint32_t now = 0;

  FindEvent step() {
    now += session.waitMs(now);
    const FindEvent event = session.classify(now, pending);
    switch (event) {
      case FindEvent::Connected:
        pending.connected = false;
        break;
      case FindEvent::TimeWritten:
        pending.time_written = false;
        break;
      case FindEvent::FindWritten:
        pending.find_written = false;
        break;
      case FindEvent::Disconnected:
        pending.disconnected = false;
        break;
      default:
        break;
    }
    return event;
  }
};

// ── the invariant the watchdog depends on ────────────────────────────────────

void test_a_round_fits_under_the_advertising_bound(void) {
  // The static_assert says it at compile time; this says it where a reader of the
  // test output sees the two numbers side by side. PROTOCOL.md §5.1 bounds every
  // un-fed interval at the advertising timeout, and a round is one.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kAdvertiseTimeoutMs, core::kFindRoundMs);
  TEST_ASSERT_GREATER_THAN_UINT32(0, core::kFindRoundMs);
  // And the cap is the document's.
  TEST_ASSERT_EQUAL_UINT32(120000, core::kFindPhoneTimeoutMs);
}

void test_no_wait_ever_exceeds_a_round(void) {
  // Driven rather than reasoned about: every reachable phase, at every elapsed
  // that matters, with every combination of signals pending. A wait of 9.9 s
  // would be under the watchdog and still a contract violation, so the bound
  // asserted is the round, not 10 s.
  const uint32_t probes[] = {0,      1,      4999,   5000,   5001,   6000,  11999,
                             12000,  59999,  60000,  119999, 120000, 120001, 0xFFFFFFFFu};
  for (int bits = 0; bits < 32; ++bits) {
    for (const uint32_t enter_at : probes) {
      FindSession session;
      session.classify(enter_at, signalsFromBits(bits));
      for (const uint32_t now : probes) {
        if (now < enter_at) {
          continue;
        }
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kFindRoundMs, session.waitMs(now));
        session.classify(now, signalsFromBits(bits));
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kFindRoundMs, session.waitMs(now));
      }
    }
  }
}

void test_a_fresh_session_waits_one_round(void) {
  const FindSession session;
  TEST_ASSERT_FALSE(session.ended());
  TEST_ASSERT_EQUAL_UINT32(core::kFindRoundMs, session.waitMs(0));
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Searching), phaseAsInt(session.phase()));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::InProgress), outcomeAsInt(session.outcome()));
}

void test_the_cap_truncates_the_last_round(void) {
  // A round that would straddle the cap ends at the cap, never past it.
  FindSession session;
  const uint32_t late = core::kFindPhoneTimeoutMs - 2000;
  session.classify(late, connected());  // a round starts at 118 s
  TEST_ASSERT_EQUAL_UINT32(2000, session.waitMs(late));
}

// ── nobody comes ─────────────────────────────────────────────────────────────

void test_nobody_comes_and_the_search_times_out(void) {
  // The wearer's phone is not here. Every round is an attempt, and at the cap the
  // search ends on its own — PROTOCOL.md §6.1: not a fault.
  Caller caller;
  int rounds = 0;
  FindEvent event = FindEvent::StillWaiting;
  while (!caller.session.ended()) {
    event = caller.step();
    if (event == FindEvent::RoundElapsed) {
      ++rounds;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kFindPhoneTimeoutMs, caller.now);
  }
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(event));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::TimedOut), outcomeAsInt(caller.session.outcome()));
  TEST_ASSERT_EQUAL_UINT32(core::kFindPhoneTimeoutMs, caller.now);
  // 120 s in 5 s rounds is 24 rounds; the 24th ends at the cap and is reported as
  // the ending, not as a round. The attempt counter starts at 1 and gains one per
  // completed round, so it reads 24 when the search gives up.
  TEST_ASSERT_EQUAL_INT(23, rounds);
  TEST_ASSERT_EQUAL_UINT8(24, caller.session.attempts());
}

void test_each_empty_round_is_a_new_attempt(void) {
  FindSession session;
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::RoundElapsed),
                        eventAsInt(session.classify(core::kFindRoundMs, nothing())));
  TEST_ASSERT_EQUAL_UINT8(2, session.attempts());
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::RoundElapsed),
                        eventAsInt(session.classify(2 * core::kFindRoundMs, nothing())));
  TEST_ASSERT_EQUAL_UINT8(3, session.attempts());
}

// ── the phone turns up ───────────────────────────────────────────────────────

void test_the_full_find_exchange(void) {
  // PROTOCOL.md §4.1's happy path: connect, the §4 write, the flagged Status, and
  // then the phone rings until the wearer presses Back.
  FindSession session;

  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Connected), eventAsInt(session.classify(900, connected())));
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Connected), phaseAsInt(session.phase()));
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());

  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::TimeWritten), eventAsInt(session.classify(2400, wrote())));
  session.noteStatusNotified();
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Ringing), phaseAsInt(session.phase()));
  TEST_ASSERT_FALSE(session.ended());

  // Rounds keep coming while the phone rings — that is the redraw of the elapsed
  // counter — and they are not attempts.
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::RoundElapsed),
                        eventAsInt(session.classify(2400 + core::kFindRoundMs, nothing())));
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Ringing), phaseAsInt(session.phase()));

  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(session.classify(9000, back())));
  TEST_ASSERT_TRUE(session.ended());
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::BackPressed), outcomeAsInt(session.outcome()));
}

void test_connect_is_reported_before_a_write_that_arrived_with_it(void) {
  // A phone can connect and write faster than the caller is scheduled. The connect
  // is the feed point, so it must not be skipped.
  FindSession session;
  FindSignals both = connected();
  both.time_written = true;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Connected), eventAsInt(session.classify(50, both)));
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::TimeWritten), eventAsInt(session.classify(50, both)));
}

void test_status_notified_only_moves_a_connected_session_to_ringing(void) {
  // The screen must not claim the phone is ringing on a session nobody has
  // connected to, whatever the caller does with the call.
  FindSession fresh;
  fresh.noteStatusNotified();
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Searching), phaseAsInt(fresh.phase()));
}

void test_a_lost_link_is_a_new_attempt_and_the_watch_looks_again(void) {
  // §4.1: the watch re-advertises after a lost link and counts a new attempt.
  FindSession session;
  session.classify(900, connected());
  session.classify(2400, wrote());
  session.noteStatusNotified();

  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Disconnected), eventAsInt(session.classify(30000, gone())));
  TEST_ASSERT_FALSE(session.ended());
  TEST_ASSERT_EQUAL_UINT8(2, session.attempts());
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Searching), phaseAsInt(session.phase()));
  // And a fresh round starts at the drop: the wearer sees "searching" for a full
  // round before the counter moves again.
  TEST_ASSERT_EQUAL_UINT32(core::kFindRoundMs, session.waitMs(30000));

  // The phone comes back.
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Connected), eventAsInt(session.classify(33000, connected())));
  TEST_ASSERT_EQUAL_UINT8(2, session.attempts());
}

void test_a_stale_disconnect_is_consumed_and_costs_no_attempt(void) {
  // A disconnect bit with no link believed up — the header explains how one could
  // be left pending. It has to be consumed (reported) or the caller's event group
  // wakes on it forever, but it is not a lost link and not a new attempt.
  FindSession session;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Disconnected), eventAsInt(session.classify(700, gone())));
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Searching), phaseAsInt(session.phase()));
  TEST_ASSERT_FALSE(session.ended());
}

void test_a_write_with_no_connect_under_it_is_still_answered(void) {
  // Not a sequence a phone produces, but the bits are independent, and a level bit
  // left pending is a spin. It is consumed as a write and the phase follows.
  FindSession session;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::TimeWritten), eventAsInt(session.classify(700, wrote())));
  TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Connected), phaseAsInt(session.phase()));
}

void test_attempts_saturate(void) {
  // A pathological phone that connects and drops every millisecond must not wrap
  // the counter back to a hopeful "try 1".
  FindSession session;
  for (int i = 0; i < 600; ++i) {
    session.classify(1, connected());
    session.classify(1, gone());
  }
  TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, session.attempts());
  TEST_ASSERT_FALSE(session.ended());
}

// ── the three ways it ends early ─────────────────────────────────────────────

void test_back_ends_the_search_in_any_phase(void) {
  // Searching, connected, ringing: Back is the wearer's way out from all three,
  // and it is latched — every later question gets the same answer and no wait.
  for (int phase = 0; phase < 3; ++phase) {
    FindSession session;
    if (phase >= 1) {
      session.classify(900, connected());
    }
    if (phase >= 2) {
      session.classify(2400, wrote());
      session.noteStatusNotified();
    }
    TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(session.classify(5000, back())));
    TEST_ASSERT_TRUE(session.ended());
    TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::BackPressed), outcomeAsInt(session.outcome()));
    for (int again = 0; again < 20; ++again) {
      TEST_ASSERT_EQUAL_UINT32(0, session.waitMs(5000 + static_cast<uint32_t>(again) * 250u));
      TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended),
                            eventAsInt(session.classify(5000 + static_cast<uint32_t>(again) * 250u,
                                                        signalsFromBits(again % 32))));
    }
  }
}

void test_a_valid_dismiss_from_the_phone_ends_the_search(void) {
  // §3.3 / §4.1: the write is reported for the caller to decode, and Ok ends it
  // with the message the wearer will read.
  FindSession session;
  session.classify(900, connected());
  session.classify(2400, wrote());
  session.noteStatusNotified();

  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::FindWritten), eventAsInt(session.classify(20000, dismissed())));
  TEST_ASSERT_FALSE(session.ended());  // not until the bytes have been judged

  session.noteFindWrite(SyncResult::Ok);
  TEST_ASSERT_TRUE(session.ended());
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::DismissedByPhone), outcomeAsInt(session.outcome()));
  TEST_ASSERT_EQUAL_UINT32(0, session.waitMs(20000));
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(session.classify(20001, nothing())));
}

void test_a_rejected_dismiss_is_ignored_and_the_search_goes_on(void) {
  // §3.3: "a malformed dismiss is logged on the watch and ignored — the search
  // continues". Exhaustive over every rejection the decoder can produce.
  const SyncResult rejections[] = {SyncResult::BadLength, SyncResult::BadVersion, SyncResult::BadType,
                                   SyncResult::OutOfRange, SyncResult::RtcWriteFailed,
                                   SyncResult::Busy};
  for (const SyncResult rejection : rejections) {
    FindSession session;
    session.classify(900, connected());
    session.classify(2400, wrote());
    session.noteStatusNotified();
    TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::FindWritten),
                          eventAsInt(session.classify(20000, dismissed())));
    session.noteFindWrite(rejection);

    TEST_ASSERT_FALSE(session.ended());
    TEST_ASSERT_EQUAL_INT(phaseAsInt(FindPhase::Ringing), phaseAsInt(session.phase()));
    TEST_ASSERT_GREATER_THAN_UINT32(0, session.waitMs(20000));
    // A later, valid one still works.
    session.classify(21000, dismissed());
    session.noteFindWrite(SyncResult::Ok);
    TEST_ASSERT_TRUE(session.ended());
  }
}

void test_a_dismiss_arriving_with_a_disconnect_still_gets_its_message(void) {
  // The phone writes Find and closes in the same breath (§4.1). Both bits can be
  // pending on one look, and the message must win over the mere link drop.
  FindSession session;
  session.classify(900, connected());
  session.classify(2400, wrote());
  session.noteStatusNotified();
  FindSignals both = dismissed();
  both.disconnected = true;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::FindWritten), eventAsInt(session.classify(20000, both)));
  session.noteFindWrite(SyncResult::Ok);
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::DismissedByPhone), outcomeAsInt(session.outcome()));
}

void test_back_outranks_a_dismiss_in_the_same_instant(void) {
  // The wearer is standing at the watch and asked for the menu; the phone's
  // message would be shown to nobody.
  FindSession session;
  session.classify(900, connected());
  FindSignals both = dismissed();
  both.back_pressed = true;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(session.classify(20000, both)));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::BackPressed), outcomeAsInt(session.outcome()));
}

void test_the_cap_outranks_everything(void) {
  // A Back press, a dismiss, a connect — none of them can be reported at or past
  // the cap. §5.1 measures it "regardless of any activity".
  for (int bits = 0; bits < 32; ++bits) {
    FindSession session;
    session.classify(100, connected());
    TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended),
                          eventAsInt(session.classify(core::kFindPhoneTimeoutMs, signalsFromBits(bits))));
    TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::TimedOut), outcomeAsInt(session.outcome()));
  }
}

void test_the_cap_boundary_is_exact(void) {
  FindSession before;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::RoundElapsed),
                        eventAsInt(before.classify(core::kFindPhoneTimeoutMs - 1, nothing())));
  TEST_ASSERT_FALSE(before.ended());
  TEST_ASSERT_EQUAL_UINT32(1, before.waitMs(core::kFindPhoneTimeoutMs - 1));

  FindSession at;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended),
                        eventAsInt(at.classify(core::kFindPhoneTimeoutMs, nothing())));
}

void test_a_dismiss_after_the_end_changes_nothing(void) {
  FindSession session;
  session.classify(5000, back());
  session.noteFindWrite(SyncResult::Ok);
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::BackPressed), outcomeAsInt(session.outcome()));
  session.noteStatusNotified();
  TEST_ASSERT_TRUE(session.ended());
}

// ── termination, whatever the caller does ────────────────────────────────────

void test_a_session_that_never_ends_on_its_own_is_impossible(void) {
  // From a fresh session, feeding the same signals forever must reach Ended by
  // the cap — for every combination, including the ones nobody consumes.
  for (int bits = 0; bits < 32; ++bits) {
    const FindSignals signals = signalsFromBits(bits);
    FindSession session;
    uint32_t now = 0;
    int steps = 0;
    while (!session.ended() && steps < 100000) {
      now += session.waitMs(now);
      session.classify(now, signals);
      ++steps;
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kFindPhoneTimeoutMs, now);
    }
    TEST_ASSERT_TRUE(session.ended());
  }
}

void test_a_caller_that_wakes_early_every_time_still_ends_inside_the_cap(void) {
  // The worst caller: ignores waitMs() and comes back a millisecond later, every
  // time, with the same bits pending. StillWaiting must not turn that into a
  // search that never closes.
  for (int bits = 0; bits < 32; ++bits) {
    const FindSignals signals = signalsFromBits(bits);
    FindSession session;
    uint32_t now = 0;
    uint32_t steps = 0;
    while (!session.ended() && steps < 2u * core::kFindPhoneTimeoutMs) {
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kFindPhoneTimeoutMs, now);
      session.classify(now, signals);
      ++now;
      ++steps;
    }
    TEST_ASSERT_TRUE(session.ended());
  }
}

void test_still_waiting_always_leaves_time_to_wait_on(void) {
  // The property that keeps StillWaiting from being a spin: it is only ever
  // returned while waitMs() at the same elapsed is non-zero.
  const uint32_t marks[] = {0, 1, 900, 4999, 5000, 7500, 60000, 119999, 120000};
  for (const uint32_t connect_at : marks) {
    for (const uint32_t write_at : marks) {
      FindSession session;
      for (uint32_t now = 0; now <= core::kFindPhoneTimeoutMs + 100; now += 37) {
        if (session.ended()) {
          break;
        }
        FindSignals signals;
        signals.connected = now >= connect_at;
        signals.time_written = now >= write_at && now >= connect_at;
        const FindEvent event = session.classify(now, signals);
        if (event == FindEvent::StillWaiting) {
          TEST_ASSERT_FALSE(session.ended());
          TEST_ASSERT_GREATER_THAN_UINT32(0, session.waitMs(now));
        }
      }
    }
  }
}

void test_no_signal_short_of_the_round_is_a_round(void) {
  // SyncWindow's hardware lesson, applied here from the start: a wait that came
  // back early with nothing pending is StillWaiting, not RoundElapsed, and the
  // attempt counter must not move for it.
  FindSession session;
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::StillWaiting), eventAsInt(session.classify(429, nothing())));
  TEST_ASSERT_EQUAL_UINT8(1, session.attempts());
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::StillWaiting),
                        eventAsInt(session.classify(core::kFindRoundMs - 1, nothing())));
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::RoundElapsed),
                        eventAsInt(session.classify(core::kFindRoundMs, nothing())));
}

void test_an_elapsed_far_past_the_cap_ends_rather_than_wraps(void) {
  FindSession session;
  TEST_ASSERT_EQUAL_UINT32(0, session.waitMs(0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_INT(eventAsInt(FindEvent::Ended), eventAsInt(session.classify(0xFFFFFFFFu, connected())));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::TimedOut), outcomeAsInt(session.outcome()));
}

// ── the screen's numbers ─────────────────────────────────────────────────────

void test_elapsed_seconds_for_the_screen(void) {
  TEST_ASSERT_EQUAL_UINT16(0, FindSession::elapsedSeconds(0));
  TEST_ASSERT_EQUAL_UINT16(0, FindSession::elapsedSeconds(999));
  TEST_ASSERT_EQUAL_UINT16(1, FindSession::elapsedSeconds(1000));
  TEST_ASSERT_EQUAL_UINT16(120, FindSession::elapsedSeconds(core::kFindPhoneTimeoutMs));
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, FindSession::elapsedSeconds(0xFFFFFFFFu));
}

// ── the gates, and where the wearer lands afterwards ─────────────────────────

void test_a_refused_gate_names_itself(void) {
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::InProgress),
                        outcomeAsInt(core::findOutcomeForGate(core::SyncGate::Open)));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::BatteryTooLow),
                        outcomeAsInt(core::findOutcomeForGate(core::SyncGate::BatteryTooLow)));
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::NotAvailable),
                        outcomeAsInt(core::findOutcomeForGate(core::SyncGate::DegradedMode)));
  // A user request overrides the interval (PROTOCOL.md §5.1), so this gate can
  // never be the one that closed; the honest mapping is "the session runs".
  TEST_ASSERT_EQUAL_INT(outcomeAsInt(FindOutcome::InProgress),
                        outcomeAsInt(core::findOutcomeForGate(core::SyncGate::IntervalNotElapsed)));
}

// The wearer's route to the search: menu, walk to Find phone, press Menu.
static core::UiState onTheFindScreen(void) {
  core::UiState ui;
  core::handleButton(ui, core::ButtonId::Menu);
  while (ui.menu_index != core::kFindPhoneMenuIndex) {
    core::handleButton(ui, core::ButtonId::Down);
  }
  core::handleButton(ui, core::ButtonId::Menu);
  return ui;
}

void test_a_timeout_returns_to_the_watchface(void) {
  // The spec's ending: two minutes, nothing found, the watch is a watch again.
  core::UiState ui = onTheFindScreen();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::Screen::App), static_cast<int>(ui.screen));
  ui.idle_minutes = 1;
  core::applyFindOutcome(ui, FindOutcome::TimedOut);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::Screen::Watchface), static_cast<int>(ui.screen));
  TEST_ASSERT_EQUAL_UINT8(0, ui.menu_index);
  TEST_ASSERT_EQUAL_UINT16(0, ui.idle_minutes);
}

void test_back_returns_to_the_menu_on_the_find_item(void) {
  // Exactly what Back does from any app screen: the menu, pointer where it was.
  core::UiState ui = onTheFindScreen();
  core::applyFindOutcome(ui, FindOutcome::BackPressed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::Screen::Menu), static_cast<int>(ui.screen));
  TEST_ASSERT_EQUAL_UINT8(core::kFindPhoneMenuIndex, ui.menu_index);
}

void test_everything_with_a_message_stays_on_the_find_screen(void) {
  const FindOutcome with_a_message[] = {FindOutcome::DismissedByPhone, FindOutcome::RadioFailed,
                                        FindOutcome::BatteryTooLow, FindOutcome::NotAvailable,
                                        FindOutcome::InProgress};
  for (const FindOutcome outcome : with_a_message) {
    core::UiState ui = onTheFindScreen();
    const core::UiState before = ui;
    core::applyFindOutcome(ui, outcome);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(core::Screen::App), static_cast<int>(ui.screen));
    TEST_ASSERT_EQUAL_UINT8(before.menu_index, ui.menu_index);
    TEST_ASSERT_EQUAL_UINT8(before.app_index, ui.app_index);
    // And the idle timeout still takes the wearer home from the message.
    TEST_ASSERT_TRUE(core::tickIdle(ui, core::kIdleTimeoutMinutes));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(core::Screen::Watchface), static_cast<int>(ui.screen));
  }
}

void test_outcome_values_are_the_persisted_ones(void) {
  // The byte lives in RTC memory across builds. Renumbering it would make a
  // stored "phone found" read as something else after a reflash.
  TEST_ASSERT_EQUAL_INT(0, outcomeAsInt(FindOutcome::InProgress));
  TEST_ASSERT_EQUAL_INT(1, outcomeAsInt(FindOutcome::BackPressed));
  TEST_ASSERT_EQUAL_INT(2, outcomeAsInt(FindOutcome::TimedOut));
  TEST_ASSERT_EQUAL_INT(3, outcomeAsInt(FindOutcome::DismissedByPhone));
  TEST_ASSERT_EQUAL_INT(4, outcomeAsInt(FindOutcome::RadioFailed));
  TEST_ASSERT_EQUAL_INT(5, outcomeAsInt(FindOutcome::BatteryTooLow));
  TEST_ASSERT_EQUAL_INT(6, outcomeAsInt(FindOutcome::NotAvailable));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_a_round_fits_under_the_advertising_bound);
  RUN_TEST(test_no_wait_ever_exceeds_a_round);
  RUN_TEST(test_a_fresh_session_waits_one_round);
  RUN_TEST(test_the_cap_truncates_the_last_round);

  RUN_TEST(test_nobody_comes_and_the_search_times_out);
  RUN_TEST(test_each_empty_round_is_a_new_attempt);

  RUN_TEST(test_the_full_find_exchange);
  RUN_TEST(test_connect_is_reported_before_a_write_that_arrived_with_it);
  RUN_TEST(test_status_notified_only_moves_a_connected_session_to_ringing);
  RUN_TEST(test_a_lost_link_is_a_new_attempt_and_the_watch_looks_again);
  RUN_TEST(test_a_stale_disconnect_is_consumed_and_costs_no_attempt);
  RUN_TEST(test_a_write_with_no_connect_under_it_is_still_answered);
  RUN_TEST(test_attempts_saturate);

  RUN_TEST(test_back_ends_the_search_in_any_phase);
  RUN_TEST(test_a_valid_dismiss_from_the_phone_ends_the_search);
  RUN_TEST(test_a_rejected_dismiss_is_ignored_and_the_search_goes_on);
  RUN_TEST(test_a_dismiss_arriving_with_a_disconnect_still_gets_its_message);
  RUN_TEST(test_back_outranks_a_dismiss_in_the_same_instant);
  RUN_TEST(test_the_cap_outranks_everything);
  RUN_TEST(test_the_cap_boundary_is_exact);
  RUN_TEST(test_a_dismiss_after_the_end_changes_nothing);

  RUN_TEST(test_a_session_that_never_ends_on_its_own_is_impossible);
  RUN_TEST(test_a_caller_that_wakes_early_every_time_still_ends_inside_the_cap);
  RUN_TEST(test_still_waiting_always_leaves_time_to_wait_on);
  RUN_TEST(test_no_signal_short_of_the_round_is_a_round);
  RUN_TEST(test_an_elapsed_far_past_the_cap_ends_rather_than_wraps);

  RUN_TEST(test_elapsed_seconds_for_the_screen);

  RUN_TEST(test_a_refused_gate_names_itself);
  RUN_TEST(test_a_timeout_returns_to_the_watchface);
  RUN_TEST(test_back_returns_to_the_menu_on_the_find_item);
  RUN_TEST(test_everything_with_a_message_stays_on_the_find_screen);
  RUN_TEST(test_outcome_values_are_the_persisted_ones);

  return UNITY_END();
}
