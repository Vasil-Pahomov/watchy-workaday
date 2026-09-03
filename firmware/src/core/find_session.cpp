#include "core/find_session.h"

namespace core {

uint32_t FindSession::waitMs(uint32_t elapsed_ms) const {
  if (ended_) {
    return 0;
  }
  // The round deadline, truncated by the cap, exactly as SyncWindow truncates a
  // phase by its cap. round_started_ms_ never exceeds elapsed_ms — it is only
  // ever stamped from it — so the sum cannot overflow before the cap ends the
  // session, and both operands are far below UINT32_MAX anyway.
  const uint32_t round_deadline = round_started_ms_ + kFindRoundMs;
  const uint32_t deadline = round_deadline < kFindPhoneTimeoutMs ? round_deadline : kFindPhoneTimeoutMs;
  return elapsed_ms >= deadline ? 0u : deadline - elapsed_ms;
}

FindEvent FindSession::finish(FindOutcome outcome) {
  ended_ = true;
  outcome_ = outcome;
  return FindEvent::Ended;
}

void FindSession::noteFindWrite(SyncResult result) {
  if (ended_) {
    return;
  }
  // §3.3: only a valid frame means anything. Every rejection is ignored and the
  // search carries on — same phase, same clock, same counter.
  if (result == SyncResult::Ok) {
    finish(FindOutcome::DismissedByPhone);
  }
}

void FindSession::noteStatusNotified() {
  if (!ended_ && phase_ == FindPhase::Connected) {
    phase_ = FindPhase::Ringing;
  }
}

uint16_t FindSession::elapsedSeconds(uint32_t elapsed_ms) {
  const uint32_t seconds = elapsed_ms / 1000u;
  return seconds > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(seconds);
}

FindEvent FindSession::classify(uint32_t elapsed_ms, const FindSignals& signals) {
  if (ended_) {
    // Latched: the caller gets the answer it already acted on, and waitMs() is
    // zero, so asking again is neither a new decision nor a spin.
    return FindEvent::Ended;
  }

  // The cap, before any signal. PROTOCOL.md §5.1 makes it absolute, and putting
  // it first is what makes this function terminate whatever the radio reports —
  // a signal the caller forgets to consume cannot hold the search open past two
  // minutes.
  if (elapsed_ms >= kFindPhoneTimeoutMs) {
    return finish(FindOutcome::TimedOut);
  }

  // The wearer outranks the phone. If both ended the search in the same instant
  // the wearer is standing at the watch and pressed Back to get the menu; the
  // phone's message would be shown to nobody.
  if (signals.back_pressed) {
    return finish(FindOutcome::BackPressed);
  }

  // A Find write is reported before the link events so a dismissal that arrived
  // together with a disconnect still gets its message; whether it is valid is the
  // decoder's question and the caller's noteFindWrite() is the answer. The round
  // restarts here as at every other event the caller feeds after — a rejected
  // frame costs a redraw and a feed, and the next round is measured from them.
  if (signals.find_written) {
    round_started_ms_ = elapsed_ms;
    return FindEvent::FindWritten;
  }

  // Connect before write, when both are pending, for SyncWindow's reason: a phone
  // can connect and write faster than the caller is scheduled, and the connect is
  // the feed point.
  if (phase_ == FindPhase::Searching && signals.connected) {
    phase_ = FindPhase::Connected;
    round_started_ms_ = elapsed_ms;
    return FindEvent::Connected;
  }

  if (phase_ != FindPhase::Searching && signals.time_written) {
    // §4 unchanged: the caller answers with Status, which now carries the flag.
    round_started_ms_ = elapsed_ms;
    return FindEvent::TimeWritten;
  }

  if (signals.disconnected) {
    // Consumed whether or not a link was believed to be up — see the header on
    // why a stale disconnect must not be left pending — but only a real link
    // dropping is a new attempt.
    const bool was_connected = phase_ != FindPhase::Searching;
    phase_ = FindPhase::Searching;
    if (was_connected && attempts_ < UINT8_MAX) {
      ++attempts_;
    }
    round_started_ms_ = elapsed_ms;
    return FindEvent::Disconnected;
  }

  // A write that arrived with no connect under it. Not a sequence a phone
  // produces, but the bits are independent and a level bit left pending is a
  // spin, so it is consumed as a write and the caller answers it. The phase is
  // moved to Connected so the answer's TimeWritten bookkeeping holds.
  if (signals.time_written) {
    phase_ = FindPhase::Connected;
    round_started_ms_ = elapsed_ms;
    return FindEvent::TimeWritten;
  }

  // Nothing pending. Whether the round is over is a question about the clock,
  // and it is asked rather than assumed — the same lesson SyncWindow learned on
  // hardware, where an event group that wakes on a stale bit hands back an
  // elapsed nowhere near the deadline.
  if (elapsed_ms >= round_started_ms_ + kFindRoundMs) {
    round_started_ms_ = elapsed_ms;
    if (phase_ == FindPhase::Searching && attempts_ < UINT8_MAX) {
      ++attempts_;
    }
    return FindEvent::RoundElapsed;
  }

  return FindEvent::StillWaiting;
}

FindOutcome findOutcomeForGate(SyncGate gate) {
  switch (gate) {
    case SyncGate::DegradedMode:
      return FindOutcome::NotAvailable;
    case SyncGate::BatteryTooLow:
      return FindOutcome::BatteryTooLow;
    case SyncGate::Open:
    case SyncGate::IntervalNotElapsed:
      break;
  }
  return FindOutcome::InProgress;
}

void applyFindOutcome(UiState& ui, FindOutcome outcome) {
  switch (outcome) {
    case FindOutcome::TimedOut:
      // The spec's ending: two minutes with the phone unfound, and the watch is a
      // watch again. Same shape as tickIdle()'s return home.
      ui.screen = Screen::Watchface;
      ui.menu_index = 0;
      ui.idle_minutes = 0;
      return;

    case FindOutcome::BackPressed:
      // Exactly what Back does from any app screen, through the same function,
      // so the two cannot drift: back to the menu with the pointer where it was.
      handleButton(ui, ButtonId::Back);
      return;

    case FindOutcome::InProgress:
    case FindOutcome::DismissedByPhone:
    case FindOutcome::RadioFailed:
    case FindOutcome::BatteryTooLow:
    case FindOutcome::NotAvailable:
      // Something to read. The screen stays where it is; the message is the
      // caller's persisted outcome, and Back or the idle timeout clears it.
      return;
  }
}

}  // namespace core
