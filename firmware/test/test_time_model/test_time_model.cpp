#include <unity.h>

#include "core/time_model.h"

using core::DateTime;

void setUp(void) {}
void tearDown(void) {}

// ── leap years ───────────────────────────────────────────────────────────────

void test_leap_year_rules(void) {
  TEST_ASSERT_TRUE(core::isLeapYear(2020));
  TEST_ASSERT_TRUE(core::isLeapYear(2024));
  TEST_ASSERT_FALSE(core::isLeapYear(2021));
  TEST_ASSERT_FALSE(core::isLeapYear(2023));
  TEST_ASSERT_TRUE(core::isLeapYear(2000));   // divisible by 400
  TEST_ASSERT_FALSE(core::isLeapYear(2100));  // divisible by 100, not 400
}

void test_days_in_month(void) {
  TEST_ASSERT_EQUAL_UINT8(31, core::daysInMonth(2026, 1));
  TEST_ASSERT_EQUAL_UINT8(28, core::daysInMonth(2026, 2));
  TEST_ASSERT_EQUAL_UINT8(29, core::daysInMonth(2024, 2));
  TEST_ASSERT_EQUAL_UINT8(30, core::daysInMonth(2026, 4));
  TEST_ASSERT_EQUAL_UINT8(31, core::daysInMonth(2026, 12));
}

void test_days_in_month_rejects_out_of_range(void) {
  // 0 rather than a plausible length, so callers cannot accept day 31 of month 13.
  TEST_ASSERT_EQUAL_UINT8(0, core::daysInMonth(2026, 0));
  TEST_ASSERT_EQUAL_UINT8(0, core::daysInMonth(2026, 13));
  TEST_ASSERT_EQUAL_UINT8(0, core::daysInMonth(2026, 255));
}

// ── validity: the gate that keeps garbage RTC reads out of the alarm ─────────

void test_valid_datetime_accepted(void) {
  const DateTime dt{2026, 8, 12, 14, 30, 45};
  TEST_ASSERT_TRUE(core::isValid(dt));
}

void test_boundary_datetimes_accepted(void) {
  TEST_ASSERT_TRUE(core::isValid(DateTime{2020, 1, 1, 0, 0, 0}));
  TEST_ASSERT_TRUE(core::isValid(DateTime{2099, 12, 31, 23, 59, 59}));
  TEST_ASSERT_TRUE(core::isValid(DateTime{2024, 2, 29, 12, 0, 0}));  // real leap day
}

void test_zeroed_struct_rejected(void) {
  // What an unconfigured or dead PCF8563 effectively hands us.
  TEST_ASSERT_FALSE(core::isValid(DateTime{}));
}

void test_out_of_range_fields_rejected(void) {
  TEST_ASSERT_FALSE(core::isValid(DateTime{2019, 8, 12, 0, 0, 0}));   // year too low
  TEST_ASSERT_FALSE(core::isValid(DateTime{2100, 8, 12, 0, 0, 0}));   // year too high
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 0, 12, 0, 0, 0}));   // month 0
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 13, 12, 0, 0, 0}));  // month 13
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 0, 0, 0, 0}));    // day 0
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 32, 0, 0, 0}));   // day 32
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 12, 24, 0, 0}));  // hour 24
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 12, 0, 60, 0}));  // minute 60
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 12, 0, 0, 60}));  // second 60
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 8, 12, 255, 255, 255}));
}

void test_impossible_calendar_dates_rejected(void) {
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 2, 29, 0, 0, 0}));  // 2026 not a leap year
  TEST_ASSERT_FALSE(core::isValid(DateTime{2024, 2, 30, 0, 0, 0}));  // never exists
  TEST_ASSERT_FALSE(core::isValid(DateTime{2026, 4, 31, 0, 0, 0}));  // April has 30
}

// ── the alarm interval ───────────────────────────────────────────────────────

void test_seconds_to_next_minute(void) {
  TEST_ASSERT_EQUAL_UINT8(59, core::secondsToNextMinute(DateTime{2026, 8, 12, 0, 0, 1}));
  TEST_ASSERT_EQUAL_UINT8(30, core::secondsToNextMinute(DateTime{2026, 8, 12, 0, 0, 30}));
  TEST_ASSERT_EQUAL_UINT8(1, core::secondsToNextMinute(DateTime{2026, 8, 12, 0, 0, 59}));
}

void test_seconds_to_next_minute_never_zero(void) {
  // Zero would mean "wake immediately" and degenerate into a wake loop that never
  // reaches deep sleep — a P0 hang, and a battery flat in hours.
  TEST_ASSERT_EQUAL_UINT8(60, core::secondsToNextMinute(DateTime{2026, 8, 12, 0, 0, 0}));
  TEST_ASSERT_EQUAL_UINT8(60, core::secondsToNextMinute(DateTime{2026, 8, 12, 0, 0, 200}));

  for (uint16_t second = 0; second <= 255; ++second) {
    DateTime dt{2026, 8, 12, 0, 0, static_cast<uint8_t>(second)};
    const uint8_t remaining = core::secondsToNextMinute(dt);
    TEST_ASSERT_GREATER_THAN_UINT8(0, remaining);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(60, remaining);
  }
}

// ── day arithmetic ───────────────────────────────────────────────────────────

void test_days_since_epoch(void) {
  TEST_ASSERT_EQUAL_INT32(0, core::daysSinceEpoch(DateTime{2000, 1, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT32(1, core::daysSinceEpoch(DateTime{2000, 1, 2, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT32(31, core::daysSinceEpoch(DateTime{2000, 2, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT32(9497, core::daysSinceEpoch(DateTime{2026, 1, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT32(9720, core::daysSinceEpoch(DateTime{2026, 8, 12, 0, 0, 0}));
}

void test_days_since_epoch_is_strictly_increasing(void) {
  // Sweeps every day across a leap year and a century-ish boundary.
  // Seed with the day before the loop starts, so the first iteration checks the
  // year boundary too.
  int32_t previous = core::daysSinceEpoch(DateTime{2023, 12, 31, 0, 0, 0});
  for (uint16_t year = 2024; year <= 2025; ++year) {
    for (uint8_t month = 1; month <= 12; ++month) {
      const uint8_t last = core::daysInMonth(year, month);
      for (uint8_t day = 1; day <= last; ++day) {
        const int32_t current = core::daysSinceEpoch(DateTime{year, month, day, 0, 0, 0});
        TEST_ASSERT_EQUAL_INT32(previous + 1, current);
        previous = current;
      }
    }
  }
}

// ── Unix epoch -> local wall clock ───────────────────────────────────────────
//
// The phone speaks seconds since 1970; the rest of this module counts days since
// 2000. These tests exist mostly to hold those two epochs against each other —
// a 10957-day error is invisible in a single spot check and catastrophic in the
// field. The protocol-level cases (offset range, rejection codes, golden vectors)
// live in test/test_protocol/.

void test_local_from_unix_epoch(void) {
  // 2026-08-17T12:34:56Z at UTC+3.
  const DateTime local = core::localFromUnixEpoch(1786970096u, 180);
  TEST_ASSERT_EQUAL_UINT16(2026, local.year);
  TEST_ASSERT_EQUAL_UINT8(8, local.month);
  TEST_ASSERT_EQUAL_UINT8(17, local.day);
  TEST_ASSERT_EQUAL_UINT8(15, local.hour);
  TEST_ASSERT_EQUAL_UINT8(34, local.minute);
  TEST_ASSERT_EQUAL_UINT8(56, local.second);
}

void test_local_from_unix_epoch_at_the_unix_epoch(void) {
  const DateTime local = core::localFromUnixEpoch(0, 0);
  TEST_ASSERT_EQUAL_UINT16(1970, local.year);
  TEST_ASSERT_EQUAL_UINT8(1, local.month);
  TEST_ASSERT_EQUAL_UINT8(1, local.day);
  TEST_ASSERT_EQUAL_UINT8(0, local.hour);
  TEST_ASSERT_EQUAL_UINT8(0, local.minute);
  TEST_ASSERT_EQUAL_UINT8(0, local.second);
  TEST_ASSERT_FALSE(core::isValid(local));  // and the caller's gate rejects it
}

// The one case where C++'s truncating division is wrong: a local time before
// 1970 has a negative seconds-since-epoch, and truncation toward zero would pick
// the following day and a negative seconds-of-day. The symptom would not be an
// obvious failure — it would be a plausible date one day out.
void test_local_from_unix_epoch_floors_below_1970(void) {
  const DateTime before = core::localFromUnixEpoch(0, -840);  // UTC-14
  TEST_ASSERT_EQUAL_UINT16(1969, before.year);
  TEST_ASSERT_EQUAL_UINT8(12, before.month);
  TEST_ASSERT_EQUAL_UINT8(31, before.day);
  TEST_ASSERT_EQUAL_UINT8(10, before.hour);
  TEST_ASSERT_EQUAL_UINT8(0, before.minute);
  TEST_ASSERT_EQUAL_UINT8(0, before.second);

  const DateTime after = core::localFromUnixEpoch(0, 840);  // UTC+14
  TEST_ASSERT_EQUAL_UINT16(1970, after.year);
  TEST_ASSERT_EQUAL_UINT8(1, after.month);
  TEST_ASSERT_EQUAL_UINT8(1, after.day);
  TEST_ASSERT_EQUAL_UINT8(14, after.hour);
}

// The far end of the u32 field. Not reachable through a valid sync — isValid()
// stops it at 2099 — but the arithmetic must not overflow on the way to being
// rejected.
void test_local_from_unix_epoch_at_the_u32_ceiling(void) {
  const DateTime local = core::localFromUnixEpoch(0xFFFFFFFFu, 0);
  TEST_ASSERT_EQUAL_UINT16(2106, local.year);
  TEST_ASSERT_EQUAL_UINT8(2, local.month);
  TEST_ASSERT_EQUAL_UINT8(7, local.day);
  TEST_ASSERT_EQUAL_UINT8(6, local.hour);
  TEST_ASSERT_EQUAL_UINT8(28, local.minute);
  TEST_ASSERT_EQUAL_UINT8(15, local.second);
  TEST_ASSERT_FALSE(core::isValid(local));

  // And the same instant shifted by the largest legal offsets stays sane.
  TEST_ASSERT_EQUAL_UINT16(2106, core::localFromUnixEpoch(0xFFFFFFFFu, 840).year);
  TEST_ASSERT_EQUAL_UINT16(2106, core::localFromUnixEpoch(0xFFFFFFFFu, -840).year);
}

// Walks every day of a leap year and the common year after it, checking the
// calendar against this module's *other* epoch. If the 1970 and 2000 origins ever
// disagree the offset shows up on the first iteration.
void test_local_from_unix_epoch_walks_the_calendar(void) {
  const uint32_t start = 1704067200u;  // 2024-01-01T00:00:00Z
  const int32_t start_days_since_2000 = 8766;
  const uint32_t span_days = 731;  // 2024 (366) + 2025 (365)

  for (uint32_t day = 0; day < span_days; ++day) {
    const DateTime dt = core::localFromUnixEpoch(start + day * 86400u, 0);
    TEST_ASSERT_TRUE(core::isValid(dt));
    TEST_ASSERT_EQUAL_UINT8(0, dt.hour);
    TEST_ASSERT_EQUAL_UINT8(0, dt.minute);
    TEST_ASSERT_EQUAL_UINT8(0, dt.second);
    TEST_ASSERT_EQUAL_INT32(start_days_since_2000 + static_cast<int32_t>(day),
                            core::daysSinceEpoch(dt));
  }

  // Spot-check the ends of the sweep by name, so an off-by-one in the loop cannot
  // hide behind a consistent-but-shifted calendar.
  const DateTime first = core::localFromUnixEpoch(start, 0);
  TEST_ASSERT_EQUAL_UINT16(2024, first.year);
  TEST_ASSERT_EQUAL_UINT8(1, first.month);
  TEST_ASSERT_EQUAL_UINT8(1, first.day);

  const DateTime last = core::localFromUnixEpoch(start + (span_days - 1) * 86400u, 0);
  TEST_ASSERT_EQUAL_UINT16(2025, last.year);
  TEST_ASSERT_EQUAL_UINT8(12, last.month);
  TEST_ASSERT_EQUAL_UINT8(31, last.day);

  // 2024-02-29 is day 59 of that sweep, and only exists if the leap rule holds.
  const DateTime leap_day = core::localFromUnixEpoch(start + 59u * 86400u, 0);
  TEST_ASSERT_EQUAL_UINT16(2024, leap_day.year);
  TEST_ASSERT_EQUAL_UINT8(2, leap_day.month);
  TEST_ASSERT_EQUAL_UINT8(29, leap_day.day);
}

// Every second of one day, so the hour/minute/second split has no gap or overlap
// anywhere — including second 0, second 59, and the midnight boundary itself.
void test_local_from_unix_epoch_splits_the_whole_day(void) {
  const uint32_t midnight = 1786924800u;  // 2026-08-17T00:00:00Z

  for (uint32_t second = 0; second < 86400u; ++second) {
    const DateTime dt = core::localFromUnixEpoch(midnight + second, 0);
    const uint32_t seconds_of_day = static_cast<uint32_t>(dt.hour) * 3600u +
                                    static_cast<uint32_t>(dt.minute) * 60u +
                                    static_cast<uint32_t>(dt.second);
    TEST_ASSERT_EQUAL_UINT32(second, seconds_of_day);
    TEST_ASSERT_EQUAL_UINT8(17, dt.day);
  }

  // One more second rolls the day, not the hour.
  const DateTime next = core::localFromUnixEpoch(midnight + 86400u, 0);
  TEST_ASSERT_EQUAL_UINT8(18, next.day);
  TEST_ASSERT_EQUAL_UINT8(0, next.hour);
  TEST_ASSERT_EQUAL_UINT8(0, next.minute);
  TEST_ASSERT_EQUAL_UINT8(0, next.second);
}

void test_day_of_week(void) {
  TEST_ASSERT_EQUAL_UINT8(6, core::dayOfWeek(DateTime{2000, 1, 1, 0, 0, 0}));   // Saturday
  TEST_ASSERT_EQUAL_UINT8(3, core::dayOfWeek(DateTime{2026, 8, 12, 0, 0, 0}));  // Wednesday
  TEST_ASSERT_EQUAL_UINT8(4, core::dayOfWeek(DateTime{2024, 2, 29, 0, 0, 0}));  // Thursday
}

void test_day_of_week_cycles(void) {
  uint8_t expected = core::dayOfWeek(DateTime{2026, 1, 1, 0, 0, 0});
  for (uint8_t day = 1; day <= 31; ++day) {
    TEST_ASSERT_EQUAL_UINT8(expected, core::dayOfWeek(DateTime{2026, 1, day, 0, 0, 0}));
    expected = static_cast<uint8_t>((expected + 1) % 7);
  }
}

void test_minutes_between(void) {
  const DateTime a{2026, 8, 12, 10, 0, 0};
  TEST_ASSERT_EQUAL_INT32(30, core::minutesBetween(a, DateTime{2026, 8, 12, 10, 30, 0}));
  TEST_ASSERT_EQUAL_INT32(0, core::minutesBetween(a, a));
  TEST_ASSERT_EQUAL_INT32(-30, core::minutesBetween(DateTime{2026, 8, 12, 10, 30, 0}, a));
}

void test_minutes_between_crosses_boundaries(void) {
  // Midnight.
  TEST_ASSERT_EQUAL_INT32(2, core::minutesBetween(DateTime{2026, 8, 12, 23, 59, 0},
                                                  DateTime{2026, 8, 13, 0, 1, 0}));
  // Month rollover.
  TEST_ASSERT_EQUAL_INT32(1, core::minutesBetween(DateTime{2026, 8, 31, 23, 59, 0},
                                                  DateTime{2026, 9, 1, 0, 0, 0}));
  // Year rollover.
  TEST_ASSERT_EQUAL_INT32(1, core::minutesBetween(DateTime{2026, 12, 31, 23, 59, 0},
                                                  DateTime{2027, 1, 1, 0, 0, 0}));
  // Across a leap day.
  TEST_ASSERT_EQUAL_INT32(1440, core::minutesBetween(DateTime{2024, 2, 28, 12, 0, 0},
                                                     DateTime{2024, 2, 29, 12, 0, 0}));
}

void test_minute_of_day(void) {
  TEST_ASSERT_EQUAL_UINT16(0, core::minuteOfDay(DateTime{2026, 8, 12, 0, 0, 0}));
  TEST_ASSERT_EQUAL_UINT16(1439, core::minuteOfDay(DateTime{2026, 8, 12, 23, 59, 0}));
}

// ── 12/24 hour ───────────────────────────────────────────────────────────────

void test_to_12_hour(void) {
  bool is_pm = true;
  TEST_ASSERT_EQUAL_UINT8(12, core::to12Hour(0, is_pm));  // midnight is 12 AM
  TEST_ASSERT_FALSE(is_pm);

  TEST_ASSERT_EQUAL_UINT8(11, core::to12Hour(11, is_pm));
  TEST_ASSERT_FALSE(is_pm);

  TEST_ASSERT_EQUAL_UINT8(12, core::to12Hour(12, is_pm));  // noon is 12 PM
  TEST_ASSERT_TRUE(is_pm);

  TEST_ASSERT_EQUAL_UINT8(1, core::to12Hour(13, is_pm));
  TEST_ASSERT_TRUE(is_pm);

  TEST_ASSERT_EQUAL_UINT8(11, core::to12Hour(23, is_pm));
  TEST_ASSERT_TRUE(is_pm);
}

// ── formatting ───────────────────────────────────────────────────────────────

void test_format_time_24h(void) {
  char buffer[8] = {};
  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 9, 5, 0}, true));
  TEST_ASSERT_EQUAL_STRING("9:05", buffer);

  TEST_ASSERT_TRUE(
      core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 23, 59, 0}, true));
  TEST_ASSERT_EQUAL_STRING("23:59", buffer);

  // Midnight is the hour the missing pad is most visible in, and it is still the
  // rule rather than an exception to it.
  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 0, 0, 0}, true));
  TEST_ASSERT_EQUAL_STRING("0:00", buffer);
}

void test_format_time_drops_only_the_hour_pad(void) {
  // The boundary in both directions: 9 loses its leading zero, 10 keeps both
  // digits, and no minute ever loses its own pad.
  char buffer[8] = {};
  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 9, 59, 0}, true));
  TEST_ASSERT_EQUAL_STRING("9:59", buffer);

  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 10, 0, 0}, true));
  TEST_ASSERT_EQUAL_STRING("10:00", buffer);

  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 1, 3, 0}, true));
  TEST_ASSERT_EQUAL_STRING("1:03", buffer);
}

void test_format_time_12h(void) {
  char buffer[8] = {};
  TEST_ASSERT_TRUE(
      core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 13, 45, 0}, false));
  TEST_ASSERT_EQUAL_STRING("1:45", buffer);

  // 12h has no single-digit midnight to worry about: to12Hour() maps 0 to 12.
  TEST_ASSERT_TRUE(core::formatTime(buffer, sizeof(buffer), DateTime{2026, 8, 12, 0, 7, 0}, false));
  TEST_ASSERT_EQUAL_STRING("12:07", buffer);
}

void test_format_time_rejects_small_buffer(void) {
  char buffer[5] = {'x', 'x', 'x', 'x', 'x'};
  TEST_ASSERT_FALSE(core::formatTime(buffer, 5, DateTime{2026, 8, 12, 9, 5, 0}, true));
  TEST_ASSERT_EQUAL_CHAR('x', buffer[0]);  // and wrote nothing

  // cap 6 is still the requirement: the check is against the longest case, not
  // against the one this call happens to write.
  char exact[6] = {};
  TEST_ASSERT_TRUE(core::formatTime(exact, 6, DateTime{2026, 8, 12, 23, 59, 0}, true));
  TEST_ASSERT_EQUAL_STRING("23:59", exact);

  TEST_ASSERT_FALSE(core::formatTime(nullptr, 32, DateTime{2026, 8, 12, 9, 5, 0}, true));
}

void test_format_date(void) {
  char buffer[16] = {};
  TEST_ASSERT_TRUE(core::formatDate(buffer, sizeof(buffer), DateTime{2026, 8, 12, 0, 0, 0}));
  TEST_ASSERT_EQUAL_STRING("Wed 12 Aug", buffer);

  TEST_ASSERT_TRUE(core::formatDate(buffer, sizeof(buffer), DateTime{2026, 1, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_STRING("Thu 1 Jan", buffer);
}

void test_format_date_drops_only_the_day_pad(void) {
  // The boundary in both directions, and the tail of the string moving with it:
  // a shortened day must not leave the old month behind it.
  char buffer[16] = {};
  TEST_ASSERT_TRUE(core::formatDate(buffer, sizeof(buffer), DateTime{2026, 9, 9, 0, 0, 0}));
  TEST_ASSERT_EQUAL_STRING("Wed 9 Sep", buffer);

  TEST_ASSERT_TRUE(core::formatDate(buffer, sizeof(buffer), DateTime{2026, 9, 10, 0, 0, 0}));
  TEST_ASSERT_EQUAL_STRING("Thu 10 Sep", buffer);

  TEST_ASSERT_TRUE(core::formatDate(buffer, sizeof(buffer), DateTime{2026, 12, 31, 0, 0, 0}));
  TEST_ASSERT_EQUAL_STRING("Thu 31 Dec", buffer);
}

void test_format_date_rejects_small_buffer(void) {
  // Ten bytes would in fact hold "Thu 1 Jan", and are still refused: the
  // contract is a buffer big enough for any date, not for this one.
  char buffer[10] = {};
  TEST_ASSERT_FALSE(core::formatDate(buffer, 10, DateTime{2026, 1, 1, 0, 0, 0}));
  TEST_ASSERT_FALSE(core::formatDate(buffer, 10, DateTime{2026, 8, 12, 0, 0, 0}));
  TEST_ASSERT_FALSE(core::formatDate(nullptr, 32, DateTime{2026, 8, 12, 0, 0, 0}));
}

void test_names_are_bounds_checked(void) {
  TEST_ASSERT_EQUAL_STRING("Sun", core::weekdayName(0));
  TEST_ASSERT_EQUAL_STRING("Sat", core::weekdayName(6));
  TEST_ASSERT_EQUAL_STRING("???", core::weekdayName(7));
  TEST_ASSERT_EQUAL_STRING("???", core::weekdayName(255));

  TEST_ASSERT_EQUAL_STRING("Jan", core::monthName(1));
  TEST_ASSERT_EQUAL_STRING("Dec", core::monthName(12));
  TEST_ASSERT_EQUAL_STRING("???", core::monthName(0));
  TEST_ASSERT_EQUAL_STRING("???", core::monthName(13));
}

// ── elapsed time, and what to believe when the clock cannot be trusted ───────
//
// The tick intervals are spelled as plain seconds here rather than pulled in from
// battery_model/health: these tests are about the arithmetic and its fallback, and
// coupling them to the tick constants would make an unrelated interval change look
// like a clock bug.

static DateTime at(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute) {
  DateTime dt;
  dt.year = year;
  dt.month = month;
  dt.day = day;
  dt.hour = hour;
  dt.minute = minute;
  dt.second = 0;
  return dt;
}

void test_elapsed_minutes_measures_a_trustworthy_clock(void) {
  const DateTime last = at(2026, 8, 17, 12, 30);
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(last, at(2026, 8, 17, 12, 31), true, 60));
  TEST_ASSERT_EQUAL_UINT32(5, core::elapsedMinutes(last, at(2026, 8, 17, 12, 35), true, 300));
  TEST_ASSERT_EQUAL_UINT32(90, core::elapsedMinutes(last, at(2026, 8, 17, 14, 0), true, 60));
  // Across midnight, which is where a naive hour-and-minute subtraction breaks.
  TEST_ASSERT_EQUAL_UINT32(60, core::elapsedMinutes(at(2026, 8, 17, 23, 30),
                                                    at(2026, 8, 18, 0, 30), true, 60));
}

void test_elapsed_minutes_reports_zero_within_one_minute(void) {
  // Two wakes inside the same minute — a button press right after a tick. No time
  // passed and the measurement says so; the floor belongs to the fallback, not to
  // a reading the clock actually made.
  const DateTime same = at(2026, 8, 17, 12, 30);
  TEST_ASSERT_EQUAL_UINT32(0, core::elapsedMinutes(same, same, true, 60));
}

void test_elapsed_minutes_infers_when_the_clock_is_unreadable(void) {
  // The case that keeps the BLE sync window honest on a watch with a dead PCF8563
  // (PROTOCOL.md §5.1): a counter frozen at zero would never reach the window that
  // could set the clock, so the interval the firmware asked for is used instead.
  const DateTime last = at(2026, 8, 17, 12, 30);
  const DateTime now = at(2026, 8, 17, 12, 31);

  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(last, now, /*now_valid=*/false, 60));
  TEST_ASSERT_EQUAL_UINT32(5, core::elapsedMinutes(last, now, false, 300));
  TEST_ASSERT_EQUAL_UINT32(15, core::elapsedMinutes(last, now, false, 900));
}

void test_elapsed_minutes_infers_when_there_is_no_previous_reading(void) {
  // A fresh persisted block holds a zeroed DateTime, which is not a time. This is
  // the first wake after a reflash or a flat battery.
  const DateTime never{};
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(never, at(2026, 8, 17, 12, 31), true, 60));
  TEST_ASSERT_EQUAL_UINT32(5, core::elapsedMinutes(never, at(2026, 8, 17, 12, 31), true, 300));
}

void test_elapsed_minutes_infers_when_neither_reading_is_usable(void) {
  const DateTime never{};
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(never, never, /*now_valid=*/false, 60));
  TEST_ASSERT_EQUAL_UINT32(5, core::elapsedMinutes(never, never, false, 300));
}

void test_elapsed_minutes_rejects_a_stored_time_outside_the_accepted_range(void) {
  // last_time lives in RTC memory, which is not zeroed and survives reflashing. A
  // year of 1999 or 2165 is not a reading, whatever the current clock says.
  const DateTime now = at(2026, 8, 17, 12, 31);
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(at(1999, 8, 17, 12, 30), now, true, 60));
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(at(2165, 8, 17, 12, 30), now, true, 60));
}

void test_elapsed_minutes_refuses_to_run_backwards(void) {
  // The clock went back: a sync corrected it, or a garbled read. A negative delta
  // must never reach a uint32 counter — as an unsigned value it would be billions
  // of minutes and would age every counter it feeds by millennia.
  const uint32_t elapsed =
      core::elapsedMinutes(at(2026, 8, 17, 14, 0), at(2026, 8, 17, 12, 0), true, 60);
  TEST_ASSERT_EQUAL_UINT32(1, elapsed);
}

void test_elapsed_minutes_clamps_an_absurd_gap(void) {
  // The 30-day sanity limit, at and past its boundary. Inside it the measurement
  // stands; at it the reading stops being evidence and the inferred value takes
  // over.
  const DateTime last = at(2026, 1, 1, 0, 0);

  // One minute inside the limit: 30 days is 43200 minutes, so 43199 is 2026-01-30
  // 23:59 — still measured.
  TEST_ASSERT_EQUAL_UINT32(core::kMaxTrustedElapsedMinutes - 1,
                           core::elapsedMinutes(last, at(2026, 1, 30, 23, 59), true, 60));

  // Exactly at the limit, and well past it: inferred.
  TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(last, at(2026, 1, 31, 0, 0), true, 60));
  TEST_ASSERT_EQUAL_UINT32(5, core::elapsedMinutes(last, at(2027, 6, 1, 0, 0), true, 300));
}

void test_elapsed_minutes_never_returns_zero_from_the_fallback(void) {
  // A sub-minute tick would infer zero, and zero is the freeze: every counter fed
  // by this — the battery schedule, the ghosting timer, step staleness, the UI
  // idle timeout, the sync window's hour — would stop advancing for good.
  const DateTime never{};
  for (uint16_t tick_seconds = 0; tick_seconds < 60; ++tick_seconds) {
    TEST_ASSERT_EQUAL_UINT32(1, core::elapsedMinutes(never, never, false, tick_seconds));
  }
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_leap_year_rules);
  RUN_TEST(test_days_in_month);
  RUN_TEST(test_days_in_month_rejects_out_of_range);

  RUN_TEST(test_valid_datetime_accepted);
  RUN_TEST(test_boundary_datetimes_accepted);
  RUN_TEST(test_zeroed_struct_rejected);
  RUN_TEST(test_out_of_range_fields_rejected);
  RUN_TEST(test_impossible_calendar_dates_rejected);

  RUN_TEST(test_seconds_to_next_minute);
  RUN_TEST(test_seconds_to_next_minute_never_zero);

  RUN_TEST(test_days_since_epoch);
  RUN_TEST(test_days_since_epoch_is_strictly_increasing);

  RUN_TEST(test_local_from_unix_epoch);
  RUN_TEST(test_local_from_unix_epoch_at_the_unix_epoch);
  RUN_TEST(test_local_from_unix_epoch_floors_below_1970);
  RUN_TEST(test_local_from_unix_epoch_at_the_u32_ceiling);
  RUN_TEST(test_local_from_unix_epoch_walks_the_calendar);
  RUN_TEST(test_local_from_unix_epoch_splits_the_whole_day);

  RUN_TEST(test_day_of_week);
  RUN_TEST(test_day_of_week_cycles);
  RUN_TEST(test_minutes_between);
  RUN_TEST(test_minutes_between_crosses_boundaries);
  RUN_TEST(test_minute_of_day);

  RUN_TEST(test_elapsed_minutes_measures_a_trustworthy_clock);
  RUN_TEST(test_elapsed_minutes_reports_zero_within_one_minute);
  RUN_TEST(test_elapsed_minutes_infers_when_the_clock_is_unreadable);
  RUN_TEST(test_elapsed_minutes_infers_when_there_is_no_previous_reading);
  RUN_TEST(test_elapsed_minutes_infers_when_neither_reading_is_usable);
  RUN_TEST(test_elapsed_minutes_rejects_a_stored_time_outside_the_accepted_range);
  RUN_TEST(test_elapsed_minutes_refuses_to_run_backwards);
  RUN_TEST(test_elapsed_minutes_clamps_an_absurd_gap);
  RUN_TEST(test_elapsed_minutes_never_returns_zero_from_the_fallback);

  RUN_TEST(test_to_12_hour);

  RUN_TEST(test_format_time_24h);
  RUN_TEST(test_format_time_drops_only_the_hour_pad);
  RUN_TEST(test_format_time_12h);
  RUN_TEST(test_format_time_rejects_small_buffer);
  RUN_TEST(test_format_date);
  RUN_TEST(test_format_date_drops_only_the_day_pad);
  RUN_TEST(test_format_date_rejects_small_buffer);
  RUN_TEST(test_names_are_bounds_checked);

  return UNITY_END();
}
