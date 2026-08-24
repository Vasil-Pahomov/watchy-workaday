#include "core/time_model.h"

namespace core {
namespace {

constexpr uint8_t kMonthLengths[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

constexpr const char* kWeekdayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

constexpr const char* kMonthNames[13] = {"???", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

inline char digit(unsigned value) { return static_cast<char>('0' + (value % 10)); }

}  // namespace

bool isLeapYear(uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return kMonthLengths[month];
}

bool isValid(const DateTime& dt) {
  if (dt.year < kMinYear || dt.year > kMaxYear) {
    return false;
  }
  const uint8_t month_length = daysInMonth(dt.year, dt.month);
  if (month_length == 0) {
    return false;
  }
  if (dt.day < 1 || dt.day > month_length) {
    return false;
  }
  return dt.hour <= 23 && dt.minute <= 59 && dt.second <= 59;
}

int32_t daysSinceEpoch(const DateTime& dt) {
  // Howard Hinnant's days_from_civil, re-based from 1970-01-01 to 2000-01-01
  // (719468 + 10957 = 730425).
  int32_t y = static_cast<int32_t>(dt.year);
  const int32_t m = static_cast<int32_t>(dt.month);
  const int32_t d = static_cast<int32_t>(dt.day);
  y -= (m <= 2) ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const int32_t yoe = y - era * 400;                                  // [0, 399]
  const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 730425;
}

DateTime localFromUnixEpoch(uint32_t utc_epoch_s, int16_t utc_offset_min) {
  // 64-bit because the sum leaves the u32 range at both ends: a negative offset
  // against a near-zero epoch goes below 0, and a positive one against 0xFFFFFFFF
  // goes above. Both are values the phone can legitimately put on the wire, and
  // both must survive as far as isValid() to be rejected there.
  const int64_t local_s =
      static_cast<int64_t>(utc_epoch_s) + static_cast<int64_t>(utc_offset_min) * 60;

  // Floor division, not C++'s truncation-toward-zero: for a local time that lands
  // before 1970 the truncating form picks the wrong day and produces a negative
  // seconds-of-day, which would then be cast into an absurd hour.
  int64_t days = local_s / 86400;
  int32_t seconds_of_day = static_cast<int32_t>(local_s % 86400);
  if (seconds_of_day < 0) {
    seconds_of_day += 86400;
    --days;
  }

  // Howard Hinnant's civil_from_days — the exact inverse of daysSinceEpoch()
  // above, on the 1970-01-01 origin (719468 days from the 0000-03-01 era start)
  // rather than that function's 2000-01-01 one.
  const int32_t z = static_cast<int32_t>(days) + 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int32_t doe = z - era * 146097;                                       // [0, 146096]
  const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const int32_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
  const int32_t day = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
  const int32_t month = mp + (mp < 10 ? 3 : -9);                              // [1, 12]
  const int32_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);

  DateTime dt{};
  dt.year = static_cast<uint16_t>(year);
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);
  dt.hour = static_cast<uint8_t>(seconds_of_day / 3600);
  dt.minute = static_cast<uint8_t>((seconds_of_day / 60) % 60);
  dt.second = static_cast<uint8_t>(seconds_of_day % 60);
  return dt;
}

uint8_t dayOfWeek(const DateTime& dt) {
  // 2000-01-01 was a Saturday (6 with Sunday == 0).
  const int32_t days = daysSinceEpoch(dt);
  int32_t dow = (days + 6) % 7;
  if (dow < 0) {
    dow += 7;
  }
  return static_cast<uint8_t>(dow);
}

uint8_t secondsToNextMinute(const DateTime& dt) {
  if (dt.second == 0 || dt.second > 59) {
    return 60;
  }
  return static_cast<uint8_t>(60 - dt.second);
}

uint16_t minuteOfDay(const DateTime& dt) {
  return static_cast<uint16_t>(dt.hour * 60 + dt.minute);
}

int32_t minutesBetween(const DateTime& from, const DateTime& to) {
  const int32_t day_delta = daysSinceEpoch(to) - daysSinceEpoch(from);
  const int32_t minute_delta =
      static_cast<int32_t>(minuteOfDay(to)) - static_cast<int32_t>(minuteOfDay(from));
  return day_delta * 1440 + minute_delta;
}

uint32_t elapsedMinutes(const DateTime& last, const DateTime& now, bool now_valid,
                        uint16_t tick_seconds) {
  if (now_valid && isValid(last)) {
    const int32_t delta = minutesBetween(last, now);
    // A negative delta means the clock went backwards — a sync that corrected it,
    // or a garbled read. An absurd one means it jumped or the watch was off for a
    // very long time. Neither is a measurement; both fall through to the inferred
    // value rather than being clamped into a plausible-looking lie.
    if (delta >= 0 && delta < static_cast<int32_t>(kMaxTrustedElapsedMinutes)) {
      return static_cast<uint32_t>(delta);
    }
  }
  // What the firmware asked the alarm for is the best available guess at how long
  // it actually slept. Floored at 1: a sub-minute tick would otherwise infer zero
  // and stall every counter this feeds, which is the freeze case above.
  const uint32_t inferred = tick_seconds / 60u;
  return inferred == 0 ? 1u : inferred;
}

uint8_t to12Hour(uint8_t hour24, bool& is_pm) {
  is_pm = hour24 >= 12 && hour24 <= 23;
  const uint8_t wrapped = static_cast<uint8_t>(hour24 % 12);
  return wrapped == 0 ? 12 : wrapped;
}

bool formatTime(char* out, size_t cap, const DateTime& dt, bool use_24h) {
  if (out == nullptr || cap < 6) {
    return false;
  }
  uint8_t hour = dt.hour;
  if (!use_24h) {
    bool is_pm = false;
    hour = to12Hour(dt.hour, is_pm);
  }
  out[0] = digit(static_cast<unsigned>(hour) / 10u);
  out[1] = digit(hour);
  out[2] = ':';
  out[3] = digit(static_cast<unsigned>(dt.minute) / 10u);
  out[4] = digit(dt.minute);
  out[5] = '\0';
  return true;
}

bool formatDate(char* out, size_t cap, const DateTime& dt) {
  if (out == nullptr || cap < 11) {
    return false;
  }
  const char* weekday = weekdayName(dayOfWeek(dt));
  const char* month = monthName(dt.month);
  out[0] = weekday[0];
  out[1] = weekday[1];
  out[2] = weekday[2];
  out[3] = ' ';
  out[4] = digit(static_cast<unsigned>(dt.day) / 10u);
  out[5] = digit(dt.day);
  out[6] = ' ';
  out[7] = month[0];
  out[8] = month[1];
  out[9] = month[2];
  out[10] = '\0';
  return true;
}

const char* weekdayName(uint8_t day_of_week) {
  if (day_of_week > 6) {
    return "???";
  }
  return kWeekdayNames[day_of_week];
}

const char* monthName(uint8_t month) {
  if (month < 1 || month > 12) {
    return "???";
  }
  return kMonthNames[month];
}

}  // namespace core
