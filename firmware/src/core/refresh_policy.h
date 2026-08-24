// Decides whether the e-paper panel is updated at all, and how. Pure logic.
//
// This is the highest-leverage decision in the firmware. A partial refresh is
// roughly 85 % of the energy of an average wake (docs/power-budget.md), so the
// cheapest possible wake is one that concludes the screen already shows the right
// pixels and skips the panel entirely. On a watchface showing HH:MM that never
// happens, but on a menu, an app screen, or any button wake it happens almost
// every time.
//
// The counterweight is ghosting: e-paper accumulates artefacts across partial
// refreshes and needs an occasional full refresh to clear them. So the policy is
// "skip if possible, partial by default, full on a schedule".
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

enum class RefreshKind : uint8_t {
  Skip,     // content unchanged — do not touch the panel
  Partial,  // fast, low energy, accumulates ghosting
  Full,     // slow, ~4x a partial, clears ghosting
};

struct RefreshLimits {
  uint16_t max_partials_before_full = 60;
  uint32_t max_minutes_before_full = 12 * 60;
};

struct RefreshState {
  uint32_t content_hash = 0;
  uint16_t partials_since_full = 0;
  uint32_t minutes_since_full = 0;
  bool drawn = false;  // false until the panel has been driven at least once
};

// FNV-1a, 32-bit. Not cryptographic; it only needs to make "the visible content
// changed" cheap to test. Collisions would show a stale screen for one tick.
uint32_t contentHash(const void* data, size_t length);
uint32_t contentHash(const char* text);

// Folds another value into an existing hash, so a caller can build one hash from
// several fields without concatenating them into a buffer first.
uint32_t hashCombine(uint32_t seed, const void* data, size_t length);
uint32_t hashCombine(uint32_t seed, const char* text);

// Advances `state` and returns what the caller must do. `minutes_elapsed` is
// added to the full-refresh age counter (saturating). `force_full` is for cases
// the policy cannot see, such as a fresh power-on or a wake from an unknown
// source, where the panel contents are not known to match the state.
//
// Note the ordering: a due full refresh beats an unchanged-content skip. Content
// that never changes still gets its periodic ghosting cleanup, which costs about
// two full refreshes a day.
RefreshKind decideRefresh(RefreshState& state, uint32_t new_hash, uint32_t minutes_elapsed,
                          bool force_full, const RefreshLimits& limits = RefreshLimits{});

}  // namespace core
