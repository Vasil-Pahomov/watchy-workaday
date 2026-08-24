#include <unity.h>

#include <cstdint>

#include "core/sync_window.h"

using core::SyncWindow;
using core::SyncWindowEvent;
using core::SyncWindowSignals;

void setUp(void) {}
void tearDown(void) {}

static int eventAsInt(SyncWindowEvent event) { return static_cast<int>(event); }

static SyncWindowSignals nothing() { return SyncWindowSignals{}; }

static SyncWindowSignals connected() {
  SyncWindowSignals signals;
  signals.connected = true;
  return signals;
}

// Signals are level, not edge: once a central is connected that bit stays set, so
// a realistic "connected and now wrote" reports both.
static SyncWindowSignals connectedAndWrote() {
  SyncWindowSignals signals;
  signals.connected = true;
  signals.time_written = true;
  return signals;
}

static SyncWindowSignals connectedAndGone() {
  SyncWindowSignals signals;
  signals.connected = true;
  signals.disconnected = true;
  return signals;
}

// A write with no connect under it. Not a sequence a phone produces, but the
// signals are level and independent, so it is reachable — and it is the one phase
// whose budget is the advertising one while the window is not yet connected.
static SyncWindowSignals wroteOnly() {
  SyncWindowSignals signals;
  signals.time_written = true;
  return signals;
}

// The four ways a window can end, each reached by driving a real sequence rather
// than by poking at state. `ended_at_ms` is when the terminal event was reported;
// a caller only ever asks again at or after that, because board/ derives elapsed
// from millis() and it moves in one direction within a wake.
struct EndedWindow {
  SyncWindow window;
  SyncWindowEvent event = SyncWindowEvent::Capped;
  uint32_t ended_at_ms = 0;
};

static EndedWindow endedWindow(int which) {
  EndedWindow ended;
  switch (which) {
    case 0:  // nobody came, and the advertising phase timed out
      ended.ended_at_ms = core::kAdvertiseTimeoutMs;
      ended.event = ended.window.classify(ended.ended_at_ms, nothing());
      break;
    case 1:  // a central connected and then said nothing
      ended.window.classify(1000, connected());
      ended.ended_at_ms = 1000 + core::kIdleAfterConnectTimeoutMs;
      ended.event = ended.window.classify(ended.ended_at_ms, connected());
      break;
    case 2:  // §6.1's normal ending: the phone hung up
      ended.window.classify(300, connected());
      ended.ended_at_ms = 700;
      ended.event = ended.window.classify(ended.ended_at_ms, connectedAndGone());
      break;
    default:  // a peer that kept the watch busy all the way to the cap
      ended.window.classify(100, connected());
      ended.ended_at_ms = core::kSessionCapMs;
      ended.event = ended.window.classify(ended.ended_at_ms, connectedAndWrote());
      break;
  }
  return ended;
}

// Each §5.1 phase, reached by driving the window into it. `entered_ms` is when
// the phase began and `deadline_ms` is when its own timeout expires; between the
// two, nothing has happened yet and the window is not over. `stale` is what a
// caller reports in this phase when nothing new has arrived — the signals are
// level, so in a connected phase that is not an empty set.
struct Phase {
  SyncWindow window;
  SyncWindowSignals stale;
  uint32_t entered_ms = 0;
  uint32_t deadline_ms = 0;
};

static Phase phaseUnderTest(int which) {
  Phase phase;
  switch (which) {
    case 0:  // advertising, nobody here yet
      phase.entered_ms = 0;
      phase.deadline_ms = core::kAdvertiseTimeoutMs;
      break;
    case 1:  // connected and idle
      phase.window.classify(900, connected());
      phase.stale = connected();
      phase.entered_ms = 900;
      phase.deadline_ms = 900 + core::kIdleAfterConnectTimeoutMs;
      break;
    case 2:  // after a Time write, which restarts the phase so §6.1's notify fits
      phase.window.classify(900, connected());
      phase.window.classify(1500, connectedAndWrote());
      phase.stale = connected();  // the caller acknowledged the write
      phase.entered_ms = 1500;
      phase.deadline_ms = 1500 + core::kIdleAfterConnectTimeoutMs;
      break;
    default:  // a write with no connect: the phase restarts, the budget does not
      phase.window.classify(1200, wroteOnly());
      phase.entered_ms = 1200;
      phase.deadline_ms = 1200 + core::kAdvertiseTimeoutMs;
      break;
  }
  return phase;
}

// ── the invariant the watchdog depends on ────────────────────────────────────

void test_no_wait_ever_reaches_the_watchdog(void) {
  // The whole reason §5.1's numbers are what they are. board::power's task
  // watchdog is 10 s and Law 2 forbids feeding it from inside a wait, so every
  // wait this type hands out has to fit under it alone. §5.1 states the bound as
  // 6 s, which is what is asserted — a wait of 9.9 s would satisfy "under the
  // watchdog" and still be a contract violation.
  //
  // Driven rather than reasoned about: every reachable state, at every
  // millisecond boundary that matters, including after the phase and the cap have
  // both passed.
  const uint32_t probes[] = {0,     1,     5999,  6000,  6001, 9998,
                             9999,  10000, 11999, 12000, 12001, 100000};

  for (const uint32_t connect_at : probes) {
    for (const uint32_t write_at : probes) {
      SyncWindow window;
      for (const uint32_t now : probes) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kAdvertiseTimeoutMs, window.waitMs(now));
        SyncWindowSignals signals;
        signals.connected = now >= connect_at;
        signals.time_written = now >= write_at && now >= connect_at;
        const SyncWindowEvent event = window.classify(now, signals);
        if (event == SyncWindowEvent::TimeWritten) {
          signals.time_written = false;  // the caller acknowledges it
        }
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kAdvertiseTimeoutMs, window.waitMs(now));
      }
    }
  }
}

void test_a_connected_wait_is_the_shorter_one(void) {
  // The two phases are different lengths and the connected one is shorter,
  // because the watchdog was fed on connect and §5.1 spends that fresh interval
  // on 4 s rather than another 6.
  SyncWindow window;
  TEST_ASSERT_EQUAL_UINT32(core::kAdvertiseTimeoutMs, window.waitMs(0));

  window.classify(1000, connected());
  TEST_ASSERT_EQUAL_UINT32(core::kIdleAfterConnectTimeoutMs, window.waitMs(1000));
}

void test_the_cap_truncates_a_phase_but_never_extends_one(void) {
  // A connect late in the window gets the remainder, not a full fresh phase —
  // otherwise the cap would not be a cap. And a connect early in the window gets
  // its whole phase and not more, which is the half that would be an un-fed
  // interval longer than §5.1 allows.
  SyncWindow late;
  late.classify(11000, connected());
  TEST_ASSERT_EQUAL_UINT32(core::kSessionCapMs - 11000, late.waitMs(11000));

  SyncWindow early;
  early.classify(100, connected());
  TEST_ASSERT_EQUAL_UINT32(core::kIdleAfterConnectTimeoutMs, early.waitMs(100));
}

// ── the phases, in the order PROTOCOL.md §4 runs them ────────────────────────

void test_nobody_comes(void) {
  SyncWindow window;
  TEST_ASSERT_EQUAL_UINT32(core::kAdvertiseTimeoutMs, window.waitMs(0));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimedOut),
                        eventAsInt(window.classify(core::kAdvertiseTimeoutMs, nothing())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_the_full_successful_exchange(void) {
  SyncWindow window;

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Connected),
                        eventAsInt(window.classify(900, connected())));
  TEST_ASSERT_FALSE(window.ended());

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(2400, connectedAndWrote())));
  TEST_ASSERT_FALSE(window.ended());

  // §6.1: the watch notifies and then lets the phone hang up.
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Disconnected),
                        eventAsInt(window.classify(2600, connectedAndGone())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_connect_is_reported_before_a_write_that_arrived_with_it(void) {
  // A phone can connect and write faster than the watch is scheduled, so both
  // signals can be pending on the first look. Reporting the write first would
  // skip §5.1's progress point (a) and cost a watchdog feed.
  SyncWindow window;
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Connected),
                        eventAsInt(window.classify(50, connectedAndWrote())));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(50, connectedAndWrote())));
}

void test_a_write_restarts_the_phase_so_the_notify_has_room(void) {
  // §6.1 requires the Status notification to be given a chance to reach the phone
  // before the radio goes down. The caller feeds the watchdog at the write, so
  // what follows is a fresh un-fed interval and gets a fresh phase to sit in —
  // otherwise a write at 3.9 s into a 4 s phase would leave 100 ms.
  SyncWindow window;
  window.classify(500, connected());
  window.classify(4300, connectedAndWrote());
  TEST_ASSERT_EQUAL_UINT32(core::kIdleAfterConnectTimeoutMs, window.waitMs(4300));
}

void test_a_second_write_in_one_connection_is_a_second_event(void) {
  // §4: "A second push in the same connection happens only if the first one
  // failed." It must not be swallowed as a duplicate.
  SyncWindow window;
  window.classify(500, connected());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(800, connectedAndWrote())));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(1200, connectedAndWrote())));
}

void test_connected_but_idle_times_out(void) {
  SyncWindow window;
  window.classify(1000, connected());
  TEST_ASSERT_EQUAL_INT(
      eventAsInt(SyncWindowEvent::TimedOut),
      eventAsInt(window.classify(1000 + core::kIdleAfterConnectTimeoutMs, connected())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_the_cap_ends_a_window_that_activity_would_otherwise_hold_open(void) {
  // §5.1: "measured from session open to teardown, regardless of any activity."
  // A phone that keeps writing must not be able to keep the radio up.
  SyncWindow window;
  window.classify(100, connected());
  for (uint32_t now = 200; now < core::kSessionCapMs; now += 300) {
    const SyncWindowEvent event = window.classify(now, connectedAndWrote());
    TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten), eventAsInt(event));
  }
  TEST_ASSERT_EQUAL_INT(
      eventAsInt(SyncWindowEvent::Capped),
      eventAsInt(window.classify(core::kSessionCapMs, connectedAndWrote())));
  TEST_ASSERT_TRUE(window.ended());
}

// ── termination, which is the defect this type was extracted to fix ──────────

void test_a_terminal_event_latches_and_stops_costing_time(void) {
  // The original bug, in one test. A disconnect signal was set and never
  // consumed, so every later wait returned instantly with Disconnected and a
  // caller that looped span at full radio current until the watchdog reset it.
  // Both halves are asserted: the answer stays the same, and the wait it hands
  // out is zero rather than a fresh phase.
  SyncWindow window;
  window.classify(300, connected());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Disconnected),
                        eventAsInt(window.classify(700, connectedAndGone())));

  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_EQUAL_UINT32(0, window.waitMs(700));
    TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Disconnected),
                          eventAsInt(window.classify(700, connectedAndGone())));
  }
}

void test_no_late_signal_can_reopen_a_window_that_has_ended(void) {
  // The other half of the latch, and the half the test above cannot see. That
  // one re-feeds the *same* signal that ended the window, so classify() derives
  // the same answer a second time from scratch and deleting
  //
  //     if (ended_) { return end_event_; }
  //
  // leaves it — and the whole suite — green. What the latch actually holds is
  // the case where a late signal derives a **different** answer, and the
  // damaging ones are the non-terminal answers. §5.1's own race is the concrete
  // instance: advertising expires and a central connects before teardown begins,
  // which without the latch turns a window that already reported TimedOut back
  // into `Connected`. A caller looping on any predicate other than
  // `event == TimeWritten` reads that as progress and goes round again.
  //
  // waitMs() stays 0 throughout — `ended_` is set by finish() and this branch
  // never touches it — so such a loop does not block. It spins at radio current
  // for whatever is left of the 12 s cap, which is the cost the two comments in
  // sync_window.h and ble.h promise a caller it cannot pay.
  //
  // Exhaustive rather than anecdotal: every way a window can end, against every
  // combination of the three signals. They are level, and nothing obliges a
  // caller to clear them once the window is over, so all eight is the real
  // domain and not a stress test. The `Capped` ending is the one row that would
  // hold without the latch, because the cap check above it fires first at any
  // elapsed past 12 s; it is asserted for completeness, not for bite.
  for (int which = 0; which < 4; ++which) {
    for (int bits = 0; bits < 8; ++bits) {
      SyncWindowSignals signals;
      signals.connected = (bits & 1) != 0;
      signals.time_written = (bits & 2) != 0;
      signals.disconnected = (bits & 4) != 0;

      EndedWindow ended = endedWindow(which);
      TEST_ASSERT_TRUE(ended.window.ended());

      for (int again = 0; again < 3; ++again) {
        const uint32_t now = ended.ended_at_ms + static_cast<uint32_t>(again) * 250u;
        TEST_ASSERT_EQUAL_INT(eventAsInt(ended.event),
                              eventAsInt(ended.window.classify(now, signals)));
        TEST_ASSERT_EQUAL_UINT32(0, ended.window.waitMs(now));
        TEST_ASSERT_TRUE(ended.window.ended());
      }
    }
  }
}

void test_an_unconsumed_write_cannot_hold_the_window_open(void) {
  // The caller is required to acknowledge a TimeWritten by clearing its signal.
  // If it does not, the cap must still end the window — which is why the cap is
  // checked before the signals rather than after them.
  SyncWindow window;
  window.classify(100, connected());

  SyncWindowEvent event = SyncWindowEvent::Connected;
  uint32_t now = 200;
  for (int i = 0; i < 1000 && !window.ended(); ++i) {
    event = window.classify(now, connectedAndWrote());
    now += 250;
  }
  TEST_ASSERT_TRUE(window.ended());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Capped), eventAsInt(event));
}

void test_a_window_that_never_ends_on_its_own_is_impossible(void) {
  // Exhaustive over the signal domain: from any starting phase, feeding the same
  // signals forever must reach a terminal event within the cap. This is the
  // property board::ble::Session::wait() advertises and the one a future caller
  // will lean on.
  for (int bits = 0; bits < 8; ++bits) {
    SyncWindowSignals signals;
    signals.connected = (bits & 1) != 0;
    signals.time_written = (bits & 2) != 0;
    signals.disconnected = (bits & 4) != 0;

    SyncWindow window;
    uint32_t now = 0;
    int steps = 0;
    while (!window.ended() && steps < 10000) {
      now += window.waitMs(now);
      window.classify(now, signals);
      ++steps;
      // Nothing may keep the window open past the cap.
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kSessionCapMs, now);
    }
    TEST_ASSERT_TRUE(window.ended());
  }
}

void test_a_caller_that_only_ever_waits_reaches_the_cap_and_stops(void) {
  // The realistic loop: block for exactly what waitMs() said, ask again, repeat.
  // With no signals at all it must end at the advertising timeout, not at the cap
  // — a window nobody joined costs 6 s and not 12.
  SyncWindow window;
  uint32_t now = window.waitMs(0);
  TEST_ASSERT_EQUAL_UINT32(core::kAdvertiseTimeoutMs, now);
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimedOut),
                        eventAsInt(window.classify(now, nothing())));
  TEST_ASSERT_EQUAL_UINT32(0, window.waitMs(now));
}

// ── a wait that came back early is not a timeout ─────────────────────────────

void test_no_signal_short_of_the_deadline_is_not_a_timeout(void) {
  // The case none of the 18 tests above feeds, and the reason a P0 shipped.
  //
  // Every one of them models a wait that genuinely expired, so "no new signal"
  // and "the deadline passed" were never separated, and classify() was free to
  // treat the first as proof of the second. On hardware they came apart at once:
  //
  //     w: el=429  bits=01 ev=0     connected at 429 ms, correct
  //     w: el=429  wait=4000        the 4 s idle budget starts here, correct
  //     w: el=429  bits=01 ev=3     TimedOut — with the phase clock at zero
  //
  // board::ble waits on an event group, which wakes on any bit that is set,
  // including one an earlier call reported and left behind. classify() was handed
  // an elapsed 3571 ms short of the deadline and reported that the phase had
  // expired. The window ended in the same millisecond the phone connected, so the
  // phone never got a write window and no sync could complete.
  //
  // Driven for all four reachable phases, at every elapsed strictly inside each
  // one, with the signals a caller actually reports there — and then at the
  // deadline itself, because the fix must not cost the timeout that is real.
  for (int which = 0; which < 4; ++which) {
    const uint32_t offsets[] = {0, 1, 2, 429, 1000, 2500, 3998, 3999, 5998, 5999};

    for (const uint32_t offset : offsets) {
      Phase phase = phaseUnderTest(which);
      const uint32_t now = phase.entered_ms + offset;
      if (now >= phase.deadline_ms) {
        continue;
      }

      // Both the empty set and the stale level bits: neither carries anything
      // new, and neither is evidence that time has passed.
      TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::StillWaiting),
                            eventAsInt(phase.window.classify(now, nothing())));
      TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::StillWaiting),
                            eventAsInt(phase.window.classify(now, phase.stale)));

      // Still open, and still with real time on the clock — which is what makes
      // a caller that goes round again a waiter rather than a spinner.
      TEST_ASSERT_FALSE(phase.window.ended());
      TEST_ASSERT_GREATER_THAN_UINT32(0, phase.window.waitMs(now));
    }

    // The deadline itself is still a timeout, and still terminal.
    Phase expired = phaseUnderTest(which);
    TEST_ASSERT_EQUAL_INT(
        eventAsInt(SyncWindowEvent::TimedOut),
        eventAsInt(expired.window.classify(expired.deadline_ms, expired.stale)));
    TEST_ASSERT_TRUE(expired.window.ended());
  }
}

void test_still_waiting_always_leaves_time_to_wait_on(void) {
  // The invariant that stops the new value being a hang: StillWaiting is only
  // ever returned while waitMs() at the same elapsed is non-zero. A caller that
  // honours it therefore blocks, and elapsed only moves forwards, so the cap is
  // still reachable. If this ever failed, runSyncWindow() would have a loop whose
  // every pass returned instantly.
  //
  // The sweep steps by a fixed amount rather than by waitMs(), which is precisely
  // the early-waking caller the honest-caller tests above cannot model.
  const uint32_t marks[] = {0, 1, 900, 4000, 5999, 6000, 7500, 11999, 12000};

  for (const uint32_t connect_at : marks) {
    for (const uint32_t write_at : marks) {
      SyncWindow window;
      for (uint32_t now = 0; now <= core::kSessionCapMs + 100; now += 37) {
        if (window.ended()) {
          break;
        }
        SyncWindowSignals signals;
        signals.connected = now >= connect_at;
        signals.time_written = now >= write_at && now >= connect_at;

        const SyncWindowEvent event = window.classify(now, signals);
        if (event == SyncWindowEvent::StillWaiting) {
          TEST_ASSERT_FALSE(window.ended());
          TEST_ASSERT_GREATER_THAN_UINT32(0, window.waitMs(now));
        }
      }
    }
  }
}

void test_a_caller_that_wakes_early_every_time_still_ends_inside_the_cap(void) {
  // runSyncWindow()'s termination, against the worst caller this type has to
  // survive: one that ignores waitMs() entirely and comes back a millisecond
  // later, every time, forever. That is the shape a stuck event-group bit gives
  // it, and StillWaiting must not turn it into a window that never closes.
  //
  // Exhaustive over the signal domain, and asserted the strong way: the window
  // ends, and it is never classified at an elapsed past the cap.
  for (int bits = 0; bits < 8; ++bits) {
    SyncWindowSignals signals;
    signals.connected = (bits & 1) != 0;
    signals.time_written = (bits & 2) != 0;
    signals.disconnected = (bits & 4) != 0;

    SyncWindow window;
    uint32_t now = 0;
    // Two caps' worth of milliseconds is far more passes than any ending needs;
    // reaching it means the window did not close and the test has caught it.
    uint32_t steps = 0;
    while (!window.ended() && steps < 2u * core::kSessionCapMs) {
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(core::kSessionCapMs, now);
      window.classify(now, signals);
      ++now;
      ++steps;
    }
    TEST_ASSERT_TRUE(window.ended());
  }
}

// ── §4's completion rule: a success ends the exchange, a failure does not ────

void test_a_successful_write_does_not_end_the_window_on_the_spot(void) {
  // The half that is easy to get wrong in the other direction, and the reason
  // noteWriteResult(Ok) does not call finish().
  //
  // A Status notification is fire-and-forget — NimBLE queues it and returns — and
  // since ~Session() stopped calling deinit() the measured gap between teardown
  // and deep sleep is 0 ms. A window that ended the instant the write was
  // processed would drop the RF domain with the packet still queued, and §4 makes
  // that notification the only thing that resets the phone's backoff: clock set,
  // log says result=0, phone learns nothing and backs off anyway.
  SyncWindow window;
  window.classify(900, connected());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(1500, connectedAndWrote())));

  window.noteWriteResult(core::SyncResult::Ok);

  TEST_ASSERT_FALSE(window.ended());
  TEST_ASSERT_GREATER_THAN_UINT32(0, window.waitMs(1500));
  // And it is the whole idle phase, restarted at the write, not a leftover sliver:
  // §6.1 gives the phone that long to hang up on its own terms.
  TEST_ASSERT_EQUAL_UINT32(core::kIdleAfterConnectTimeoutMs, window.waitMs(1500));
}

void test_a_successful_write_ends_the_window_at_the_next_thing_that_happens(void) {
  // §4: "A second push in the same connection happens only if the first one
  // failed." It did not fail, so a further push is not a retry and there is
  // nothing left to answer.
  //
  // Measured on hardware before this rule was applied: twelve writes, every one
  // result=0, ~780 ms apart, the window running the full 12 s to the cap and
  // committing twelve RTC writes where §4 asked for one.
  SyncWindow window;
  window.classify(900, connected());
  window.classify(1500, connectedAndWrote());
  window.noteWriteResult(core::SyncResult::Ok);

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Completed),
                        eventAsInt(window.classify(2280, connectedAndWrote())));
  TEST_ASSERT_TRUE(window.ended());
  TEST_ASSERT_EQUAL_UINT32(0, window.waitMs(2280));
}

void test_the_normal_success_still_ends_as_a_hang_up(void) {
  // §6.1's normal ending, and it must not be displaced by the rule above: a phone
  // that behaves takes its notification and disconnects. That is `Disconnected`,
  // not `Completed` — the distinction is between "the phone let go" and "the phone
  // would not stop", and the log is where someone will read it.
  SyncWindow window;
  window.classify(900, connected());
  window.classify(1500, connectedAndWrote());
  window.noteWriteResult(core::SyncResult::Ok);

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Disconnected),
                        eventAsInt(window.classify(1800, connectedAndGone())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_a_silent_phone_after_a_success_times_out_on_the_idle_phase(void) {
  // The third way the post-success wait can end: the phone neither hangs up nor
  // pushes again. §6.1 allows exactly this — "let the phone disconnect or time
  // out" — and the §5.1 idle timeout must be what bounds it, not the 12 s cap.
  SyncWindow window;
  window.classify(900, connected());
  window.classify(1500, connectedAndWrote());
  window.noteWriteResult(core::SyncResult::Ok);

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::StillWaiting),
                        eventAsInt(window.classify(3000, connected())));
  TEST_ASSERT_EQUAL_INT(
      eventAsInt(SyncWindowEvent::TimedOut),
      eventAsInt(window.classify(1500 + core::kIdleAfterConnectTimeoutMs, connected())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_a_failed_write_keeps_the_retry_section_4_licenses(void) {
  // The property runSyncWindow()'s loop exists for, and the one this change must
  // not regress into "hang up after any write". §4 licenses a second push when the
  // first failed, and a watch that answered one failure and then hung up would
  // drop the retry of exactly the exchange that had just gone wrong.
  //
  // Exhaustive over every rejection core::protocol can produce: only Ok completes.
  const core::SyncResult failures[] = {
      core::SyncResult::BadLength,  core::SyncResult::BadVersion,
      core::SyncResult::OutOfRange, core::SyncResult::RtcWriteFailed,
      core::SyncResult::BadType,    core::SyncResult::Busy,
  };

  for (const core::SyncResult failure : failures) {
    SyncWindow window;
    window.classify(900, connected());
    TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                          eventAsInt(window.classify(1500, connectedAndWrote())));
    window.noteWriteResult(failure);

    // The retry is a fresh TimeWritten, not an ending.
    TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                          eventAsInt(window.classify(2300, connectedAndWrote())));
    TEST_ASSERT_FALSE(window.ended());
    // And it restarts the phase, so the retry's own notification has room too.
    TEST_ASSERT_EQUAL_UINT32(core::kIdleAfterConnectTimeoutMs, window.waitMs(2300));

    // A failure after a failure keeps the window open just the same.
    window.noteWriteResult(failure);
    TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                          eventAsInt(window.classify(3100, connectedAndWrote())));
    TEST_ASSERT_FALSE(window.ended());
  }
}

void test_a_retry_that_finally_succeeds_ends_the_window(void) {
  // The two halves meeting: §4's retry runs, the second attempt validates, and the
  // window closes on the next push rather than carrying on to the cap.
  SyncWindow window;
  window.classify(500, connected());
  window.classify(900, connectedAndWrote());
  window.noteWriteResult(core::SyncResult::RtcWriteFailed);
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimeWritten),
                        eventAsInt(window.classify(1700, connectedAndWrote())));
  window.noteWriteResult(core::SyncResult::Ok);
  TEST_ASSERT_FALSE(window.ended());

  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Completed),
                        eventAsInt(window.classify(2500, connectedAndWrote())));
  TEST_ASSERT_TRUE(window.ended());
}

void test_the_completion_rule_cannot_extend_a_window(void) {
  // §5.1's cap outranks §4 as it outranks every other signal: it is checked before
  // any of them. A success late in the window buys the phone no extra time.
  SyncWindow window;
  window.classify(100, connected());
  window.classify(11500, connectedAndWrote());
  window.noteWriteResult(core::SyncResult::Ok);

  TEST_ASSERT_EQUAL_INT(
      eventAsInt(SyncWindowEvent::Capped),
      eventAsInt(window.classify(core::kSessionCapMs, connectedAndWrote())));
  TEST_ASSERT_TRUE(window.ended());
}

// ── boundaries ───────────────────────────────────────────────────────────────

void test_the_phase_boundary_is_exact(void) {
  // One millisecond before the deadline is still waiting; the deadline itself is
  // over. Off by one here is either a wasted millisecond of radio or an un-fed
  // interval a millisecond past what §5.1 allows.
  SyncWindow early;
  TEST_ASSERT_EQUAL_UINT32(1, early.waitMs(core::kAdvertiseTimeoutMs - 1));

  SyncWindow exact;
  TEST_ASSERT_EQUAL_UINT32(0, exact.waitMs(core::kAdvertiseTimeoutMs));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::TimedOut),
                        eventAsInt(exact.classify(core::kAdvertiseTimeoutMs, nothing())));
}

void test_the_cap_boundary_is_exact(void) {
  // A connect at 9 s gives the idle phase a deadline of 13 s, which the cap
  // truncates to 12 s. So at 11999 ms nothing has expired: not the phase, not the
  // cap, and there is exactly one millisecond of window left to wait in.
  //
  // This test used to assert TimedOut there, and that assertion was the defect
  // written down — it is the same claim classify() was making, that no new signal
  // means the deadline passed, at an elapsed where the deadline provably had not.
  // Asserting StillWaiting is the stronger statement, not the looser one: it
  // pins the two boundaries apart. The window still ends at exactly 12000, which
  // is what the test is named for, and the one-millisecond wait is asserted so
  // that "not over yet" cannot be satisfied by a window that hands out nothing.
  SyncWindow before;
  before.classify(9000, connected());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::StillWaiting),
                        eventAsInt(before.classify(core::kSessionCapMs - 1, connected())));
  TEST_ASSERT_FALSE(before.ended());
  TEST_ASSERT_EQUAL_UINT32(1, before.waitMs(core::kSessionCapMs - 1));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Capped),
                        eventAsInt(before.classify(core::kSessionCapMs, connected())));

  SyncWindow at;
  at.classify(9000, connected());
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Capped),
                        eventAsInt(at.classify(core::kSessionCapMs, connected())));
}

void test_a_fresh_window_is_advertising_and_not_ended(void) {
  const SyncWindow window;
  TEST_ASSERT_FALSE(window.ended());
  TEST_ASSERT_EQUAL_UINT32(core::kAdvertiseTimeoutMs, window.waitMs(0));
}

void test_an_elapsed_far_past_the_cap_ends_rather_than_wraps(void) {
  // board/ derives elapsed from millis() with unsigned subtraction. A wake never
  // lasts long enough to wrap, but a corrupt or absurd value must end the window
  // rather than compute a fresh phase out of it.
  SyncWindow window;
  TEST_ASSERT_EQUAL_UINT32(0, window.waitMs(0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_INT(eventAsInt(SyncWindowEvent::Capped),
                        eventAsInt(window.classify(0xFFFFFFFFu, connectedAndWrote())));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_no_wait_ever_reaches_the_watchdog);
  RUN_TEST(test_a_connected_wait_is_the_shorter_one);
  RUN_TEST(test_the_cap_truncates_a_phase_but_never_extends_one);

  RUN_TEST(test_nobody_comes);
  RUN_TEST(test_the_full_successful_exchange);
  RUN_TEST(test_connect_is_reported_before_a_write_that_arrived_with_it);
  RUN_TEST(test_a_write_restarts_the_phase_so_the_notify_has_room);
  RUN_TEST(test_a_second_write_in_one_connection_is_a_second_event);
  RUN_TEST(test_connected_but_idle_times_out);
  RUN_TEST(test_the_cap_ends_a_window_that_activity_would_otherwise_hold_open);

  RUN_TEST(test_a_terminal_event_latches_and_stops_costing_time);
  RUN_TEST(test_no_late_signal_can_reopen_a_window_that_has_ended);
  RUN_TEST(test_an_unconsumed_write_cannot_hold_the_window_open);
  RUN_TEST(test_a_window_that_never_ends_on_its_own_is_impossible);
  RUN_TEST(test_a_caller_that_only_ever_waits_reaches_the_cap_and_stops);

  RUN_TEST(test_no_signal_short_of_the_deadline_is_not_a_timeout);
  RUN_TEST(test_still_waiting_always_leaves_time_to_wait_on);
  RUN_TEST(test_a_caller_that_wakes_early_every_time_still_ends_inside_the_cap);

  RUN_TEST(test_a_successful_write_does_not_end_the_window_on_the_spot);
  RUN_TEST(test_a_successful_write_ends_the_window_at_the_next_thing_that_happens);
  RUN_TEST(test_the_normal_success_still_ends_as_a_hang_up);
  RUN_TEST(test_a_silent_phone_after_a_success_times_out_on_the_idle_phase);
  RUN_TEST(test_a_failed_write_keeps_the_retry_section_4_licenses);
  RUN_TEST(test_a_retry_that_finally_succeeds_ends_the_window);
  RUN_TEST(test_the_completion_rule_cannot_extend_a_window);

  RUN_TEST(test_the_phase_boundary_is_exact);
  RUN_TEST(test_the_cap_boundary_is_exact);
  RUN_TEST(test_a_fresh_window_is_advertising_and_not_ended);
  RUN_TEST(test_an_elapsed_far_past_the_cap_ends_rather_than_wraps);

  return UNITY_END();
}
