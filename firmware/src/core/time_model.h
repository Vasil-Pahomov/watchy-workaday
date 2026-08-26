// Calendar and clock arithmetic. Pure logic — no hardware, host-tested.
//
// The PCF8563 is not trusted: a dead battery, a fresh chip, or a corrupted I2C
// read all produce values that look like a time but are not one. Everything that
// leaves the RTC passes through isValid() before it is allowed to influence the
// next alarm, because an alarm set to an impossible time never fires and the
// watch freezes until someone resets it by hand.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

struct DateTime {
  uint16_t year = 0;  // full year, e.g. 2026
  uint8_t month = 0;  // 1..12
  uint8_t day = 0;    // 1..31
  uint8_t hour = 0;   // 0..23
  uint8_t minute = 0; // 0..59
  uint8_t second = 0; // 0..59
};

// Accepted range. The lower bound rejects the PCF8563's power-on defaults, which
// is the common "RTC was never set" case.
constexpr uint16_t kMinYear = 2020;
constexpr uint16_t kMaxYear = 2099;

bool isLeapYear(uint16_t year);

// Returns 0 for an out-of-range month, so callers cannot accidentally accept
// day 31 of month 13.
uint8_t daysInMonth(uint16_t year, uint8_t month);

bool isValid(const DateTime& dt);

// Days since 2000-01-01. Valid input required; meaningless otherwise.
int32_t daysSinceEpoch(const DateTime& dt);

// Unix UTC epoch seconds plus a signed UTC offset in minutes -> local wall clock.
// `local = utc + offset * 60`, exactly as PROTOCOL.md §3.1 defines it.
//
// **The two epochs in this file are not the same one, and that is deliberate.**
// daysSinceEpoch() counts from 2000-01-01 because that is what the alarm
// arithmetic wants; this counts from 1970-01-01 because that is what every phone
// platform hands out. Do not "unify" them — the wire format is fixed.
//
// The offset already includes DST and is simply applied. The watch carries no
// timezone database and must never derive one; a DST transition arrives as a
// different offset on the next sync.
//
// The result is **not** validated. The caller gates it through isValid(), and
// out-of-window inputs — an offset pushing a 2020-01-01 UTC instant back into
// 2019, or a u32 epoch reaching 2106 — yield the true calendar date for that
// instant so that isValid() can reject it. Clamping here would launder a
// rejectable value into a plausible wrong one.
DateTime localFromUnixEpoch(uint32_t utc_epoch_s, int16_t utc_offset_min);

// 0 = Sunday .. 6 = Saturday.
uint8_t dayOfWeek(const DateTime& dt);

// Seconds until the next minute boundary: 1..60, never 0.
//
// Zero would mean "alarm immediately", which on a periodic tick degenerates into
// a spin that never sleeps, so second == 0 yields a full 60.
uint8_t secondsToNextMinute(const DateTime& dt);

uint16_t minuteOfDay(const DateTime& dt);

int32_t minutesBetween(const DateTime& from, const DateTime& to);

// Beyond a month a delta stops being evidence of anything: the watch was off, the
// clock jumped, or the persisted reading is from another era. The measurement is
// abandoned in favour of the inferred one rather than credited.
constexpr uint32_t kMaxTrustedElapsedMinutes = 60u * 24u * 30u;

// How long since the previous wake, in minutes — measured from the clock when both
// readings can be trusted, and otherwise inferred from the tick interval the
// firmware asked for.
//
// This is a **decision**, not arithmetic, which is why it is here rather than in
// main.cpp: it decides what to believe about elapsed time when the PCF8563 cannot
// be trusted, and nearly every accumulating counter in the firmware is driven by
// its answer — the battery sample schedule, the ghosting timer, the step count's
// staleness clock, the UI idle timeout, and the BLE sync window's hourly timer.
//
// Both failure directions are live, and the fallback is what handles them:
//
//   * Returning 0 on an untrustworthy clock would **freeze** every one of those
//     counters. A watch whose RTC has died would never reach its next battery
//     sample and never reach the sync window that could set its clock — the
//     feature would be unavailable exactly when it is needed.
//   * Returning a raw difference between two readings that are not both
//     trustworthy would let a garbage timestamp **run them wild**, ageing the step
//     count by decades or making a stale screen look fresh.
//
// So: `now_valid` is the caller's verdict on the current reading (board::rtc::read()
// checks the transport, the oscillator's VL bit and isValid()), `last` is validated
// here — which also covers "there is no previous reading", since a default DateTime
// fails isValid() — and a delta that is negative or absurd is discarded. The
// fallback floors at 1, so a wake always advances the counters by something.
uint32_t elapsedMinutes(const DateTime& last, const DateTime& now, bool now_valid,
                        uint16_t tick_seconds);

// 24h -> 12h clock face value (1..12), reporting AM/PM through is_pm.
uint8_t to12Hour(uint8_t hour24, bool& is_pm);

// "H:MM" or "HH:MM": the minutes are zero-padded, the hour is not, so nine in the
// morning reads "9:03" and midnight reads "0:03". Needs cap >= 6, which is still
// the longest case plus its terminator. Returns false and writes nothing if the
// buffer is too small.
bool formatTime(char* out, size_t cap, const DateTime& dt, bool use_24h);

// "Wed 12 Aug", or "Thu 1 Jan" - the day is not padded, matching the hour in
// formatTime(). Needs cap >= 11, which is the two-digit case plus its terminator.
bool formatDate(char* out, size_t cap, const DateTime& dt);

// Three-letter names; "???" for out-of-range input rather than reading past the
// end of the table.
const char* weekdayName(uint8_t day_of_week);
const char* monthName(uint8_t month);

}  // namespace core
