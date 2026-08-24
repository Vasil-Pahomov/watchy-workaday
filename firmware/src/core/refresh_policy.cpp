#include "core/refresh_policy.h"

namespace core {
namespace {

constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint32_t saturatingAdd(uint32_t base, uint32_t addend) {
  const uint32_t sum = base + addend;
  return sum < base ? UINT32_MAX : sum;
}

}  // namespace

uint32_t hashCombine(uint32_t seed, const void* data, size_t length) {
  if (data == nullptr) {
    return seed;
  }
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint32_t hash = seed;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint32_t>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

uint32_t hashCombine(uint32_t seed, const char* text) {
  if (text == nullptr) {
    return seed;
  }
  uint32_t hash = seed;
  for (const char* p = text; *p != '\0'; ++p) {
    hash ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
    hash *= kFnvPrime;
  }
  return hash;
}

uint32_t contentHash(const void* data, size_t length) {
  return hashCombine(kFnvOffsetBasis, data, length);
}

uint32_t contentHash(const char* text) { return hashCombine(kFnvOffsetBasis, text); }

RefreshKind decideRefresh(RefreshState& state, uint32_t new_hash, uint32_t minutes_elapsed,
                          bool force_full, const RefreshLimits& limits) {
  state.minutes_since_full = saturatingAdd(state.minutes_since_full, minutes_elapsed);

  const bool never_drawn = !state.drawn;
  const bool partials_exhausted = state.partials_since_full >= limits.max_partials_before_full;
  const bool too_long_since_full = state.minutes_since_full >= limits.max_minutes_before_full;

  if (never_drawn || force_full || partials_exhausted || too_long_since_full) {
    state.content_hash = new_hash;
    state.partials_since_full = 0;
    state.minutes_since_full = 0;
    state.drawn = true;
    return RefreshKind::Full;
  }

  if (new_hash == state.content_hash) {
    // The panel already shows this. Leaving partials_since_full untouched is
    // deliberate: no refresh means no new ghosting to account for.
    return RefreshKind::Skip;
  }

  state.content_hash = new_hash;
  if (state.partials_since_full < UINT16_MAX) {
    state.partials_since_full++;
  }
  return RefreshKind::Partial;
}

}  // namespace core
