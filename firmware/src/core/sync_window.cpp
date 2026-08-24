#include "core/sync_window.h"

namespace core {

uint32_t SyncWindow::phaseDeadlineMs() const {
  // §5.1: 6 s advertising with nobody there, 4 s connected and idle. Both are
  // measured from the start of the phase, which is also the moment the caller
  // last fed the watchdog — that is what makes each one a complete un-fed
  // interval rather than a slice of a longer one.
  const uint32_t limit = connected_ ? kIdleAfterConnectTimeoutMs : kAdvertiseTimeoutMs;
  return phase_started_ms_ + limit;
}

uint32_t SyncWindow::waitMs(uint32_t elapsed_ms) const {
  if (ended_) {
    return 0;
  }
  const uint32_t phase_deadline = phaseDeadlineMs();
  const uint32_t deadline = phase_deadline < kSessionCapMs ? phase_deadline : kSessionCapMs;
  return elapsed_ms >= deadline ? 0u : deadline - elapsed_ms;
}

SyncWindowEvent SyncWindow::finish(SyncWindowEvent event) {
  ended_ = true;
  end_event_ = event;
  return event;
}

void SyncWindow::noteWriteResult(SyncResult result) {
  // Only Ok completes the exchange. Every other code is a failure that §4 invites
  // the phone to retry inside this same window, so the window is left untouched —
  // same phase, same clock, same latch — and the next write is a fresh TimeWritten.
  //
  // Note what this does not do: it does not call finish(). The window has to stay
  // open long enough for the Status notification to actually leave the radio; see
  // the contract in sync_window.h for why that is not optional.
  if (result == SyncResult::Ok) {
    exchange_complete_ = true;
  }
}

SyncWindowEvent SyncWindow::classify(uint32_t elapsed_ms, const SyncWindowSignals& signals) {
  if (ended_) {
    // Latched. The caller gets the same answer it already acted on, and waitMs()
    // hands out zero, so asking again is neither a new decision nor a spin.
    return end_event_;
  }

  // The cap, before the signals. §5.1 measures it "from session open to teardown,
  // regardless of any activity", so activity cannot postpone it — and putting it
  // first is also what makes this function terminate no matter what the radio
  // reports. A signal that is never cleared, or one that repeats, cannot hold the
  // window open past 12 s.
  if (elapsed_ms >= kSessionCapMs) {
    return finish(SyncWindowEvent::Capped);
  }

  // Connect before write, when both are pending. A phone can connect and write
  // faster than the caller is scheduled, and reporting the write first would cost
  // the watchdog feed §5.1 attaches to the connect.
  if (!connected_ && signals.connected) {
    connected_ = true;
    phase_started_ms_ = elapsed_ms;
    return SyncWindowEvent::Connected;
  }

  if (signals.time_written) {
    if (exchange_complete_) {
      // §4: "A second push in the same connection happens only if the first one
      // failed." The first one did not fail, so this is not the retry that clause
      // licenses and there is nothing left to answer. Ending here is what stops a
      // successful sync holding the radio up to the 12 s cap — measured on
      // hardware as twelve writes and twelve RTC commits in one window, where §4
      // asked for one.
      return finish(SyncWindowEvent::Completed);
    }
    // Not terminal: §6.1 requires the failure or success code to go back as a
    // Status notification and the phone to be left to hang up on its own terms.
    // The phase restarts because the caller feeds the watchdog here, so what
    // follows is a fresh un-fed interval and gets a fresh 4 s to fit inside.
    phase_started_ms_ = elapsed_ms;
    return SyncWindowEvent::TimeWritten;
  }

  if (signals.disconnected) {
    // §6.1: normal, not a fault, and it ends the window — the watch does not
    // spend the remainder looking for another central.
    return finish(SyncWindowEvent::Disconnected);
  }

  // Nothing pending. Whether that is a §5.1 timeout is a question about the
  // clock, and it gets asked rather than assumed.
  //
  // The old code answered TimedOut here on the reasoning that nothing pending
  // meant the wait had run to the deadline waitMs() handed out. That is a claim
  // about the caller, not about this object, and the caller cannot make it good:
  // it waits on an event group, which wakes on any bit — including one an earlier
  // call reported and left set — so a return at 429 ms into a 4000 ms phase is an
  // ordinary thing for it to do. Answering TimedOut there ended the window with
  // the phase clock reading zero and reported a timeout that had not happened.
  //
  // So: the deadline is checked. Past it, the phase really did expire and the
  // window is over. Short of it, nothing has happened yet and there is still time
  // to wait — and there is provably time, because waitMs() takes the smaller of
  // this deadline and the cap and both are still ahead of elapsed_ms here, so the
  // caller blocks rather than spins. It also gets no watchdog feed, which is what
  // keeps a pathological caller bounded by the watchdog rather than by trust.
  if (elapsed_ms >= phaseDeadlineMs()) {
    return finish(SyncWindowEvent::TimedOut);
  }
  return SyncWindowEvent::StillWaiting;
}

}  // namespace core
