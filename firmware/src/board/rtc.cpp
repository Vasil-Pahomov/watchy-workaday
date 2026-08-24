#include "board/rtc.h"

#include "board/diag.h"
#include "board/i2c.h"

namespace board {
namespace rtc {
namespace {

// PCF8563 register map.
constexpr uint8_t kRegControlStatus2 = 0x01;
constexpr uint8_t kRegVlSeconds = 0x02;  // bit 7 is VL: oscillator integrity lost
constexpr uint8_t kRegTimerControl = 0x0E;
constexpr uint8_t kRegTimerValue = 0x0F;

// Control_status_2 bits.
constexpr uint8_t kBitTimerInterruptEnable = 0x01;  // TIE
constexpr uint8_t kBitAlarmInterruptEnable = 0x02;  // AIE
constexpr uint8_t kBitTimerFlag = 0x04;             // TF — cleared by writing 0
constexpr uint8_t kBitAlarmFlag = 0x08;             // AF — cleared by writing 0

// Timer_control bits: TE plus a 2-bit source select. 0b11 selects 1/60 Hz, so the
// countdown value is a whole number of minutes.
constexpr uint8_t kBitTimerEnable = 0x80;
constexpr uint8_t kTimerSourceOnePerMinute = 0x03;

// TIE set so the tick stays armed; TF and AF written as 0, which is how the
// PCF8563 clears them; AIE left clear because the countdown timer drives the tick,
// not the alarm — a stray armed alarm could also pull INT low and would then be
// misattributed as a tick.
constexpr uint8_t kFlagsClearedTimerArmed =
    static_cast<uint8_t>(kBitTimerInterruptEnable & ~(kBitTimerFlag | kBitAlarmFlag |
                                                      kBitAlarmInterruptEnable));

uint8_t bcdToBin(uint8_t value) {
  return static_cast<uint8_t>((value & 0x0Fu) + ((value >> 4) & 0x0Fu) * 10u);
}

uint8_t binToBcd(uint8_t value) {
  return static_cast<uint8_t>(((value / 10u) << 4) | (value % 10u));
}

}  // namespace

ReadResult read() {
  ReadResult result;

  // Registers 0x02..0x08: VL_seconds, minutes, hours, days, weekdays,
  // century_months, years.
  uint8_t raw[7] = {};
  if (!i2c::readRegisters(kI2cAddress, kRegVlSeconds, raw, sizeof(raw))) {
    WD_LOG("rtc: read failed");
    return result;
  }
  result.transport_ok = true;

  // VL set means the oscillator stopped at some point: whatever the registers now
  // contain is not the time.
  result.clock_integrity = (raw[0] & 0x80u) == 0;

  result.time.second = bcdToBin(static_cast<uint8_t>(raw[0] & 0x7Fu));
  result.time.minute = bcdToBin(static_cast<uint8_t>(raw[1] & 0x7Fu));
  result.time.hour = bcdToBin(static_cast<uint8_t>(raw[2] & 0x3Fu));
  result.time.day = bcdToBin(static_cast<uint8_t>(raw[3] & 0x3Fu));
  const uint8_t month = bcdToBin(static_cast<uint8_t>(raw[5] & 0x1Fu));
  result.time.month = month;
  // The century bit means "19xx"; this firmware's accepted range starts at 2020,
  // so a set century bit simply fails validation, which is the correct outcome.
  const uint8_t year_in_century = bcdToBin(raw[6]);
  result.time.year = static_cast<uint16_t>(2000u + year_in_century);

  result.valid = result.transport_ok && result.clock_integrity && core::isValid(result.time);
  if (!result.valid) {
    WD_LOG("rtc: rejected time %04u-%02u-%02u %02u:%02u:%02u (vl_ok=%d)",
           result.time.year, result.time.month, result.time.day, result.time.hour,
           result.time.minute, result.time.second, result.clock_integrity ? 1 : 0);
  }
  return result;
}

bool write(const core::DateTime& time) {
  if (!core::isValid(time)) {
    return false;
  }
  const uint8_t year_in_century = static_cast<uint8_t>(time.year - 2000u);
  const uint8_t weekday = core::dayOfWeek(time);

  // Writing seconds with bit 7 clear also clears VL, asserting that the clock is
  // now trustworthy.
  const bool ok = i2c::writeRegister(kI2cAddress, kRegVlSeconds, binToBcd(time.second)) &&
                  i2c::writeRegister(kI2cAddress, 0x03, binToBcd(time.minute)) &&
                  i2c::writeRegister(kI2cAddress, 0x04, binToBcd(time.hour)) &&
                  i2c::writeRegister(kI2cAddress, 0x05, binToBcd(time.day)) &&
                  i2c::writeRegister(kI2cAddress, 0x06, binToBcd(weekday)) &&
                  i2c::writeRegister(kI2cAddress, 0x07, binToBcd(time.month)) &&
                  i2c::writeRegister(kI2cAddress, 0x08, binToBcd(year_in_century));
  if (!ok) {
    WD_LOG("rtc: write failed");
  }
  return ok;
}

bool clearInterruptFlags() {
  // TF and AF are cleared by writing 0 to them. Keeping TIE set preserves the
  // armed tick; AIE stays off because the countdown timer, not the alarm, drives
  // the tick.
  return i2c::writeRegister(kI2cAddress, kRegControlStatus2, kFlagsClearedTimerArmed);
}

bool armTick(uint8_t minutes) {
  const uint8_t count = minutes == 0 ? 1 : minutes;

  // Order matters. Stop the timer, clear the stale flag so INT is released, load
  // the new count, then start it. Arming without clearing first leaves INT
  // asserted and the next deep sleep ends immediately.
  if (!i2c::writeRegister(kI2cAddress, kRegTimerControl, 0x00)) {
    return false;
  }
  if (!clearInterruptFlags()) {
    return false;
  }
  if (!i2c::writeRegister(kI2cAddress, kRegTimerValue, count)) {
    return false;
  }
  const uint8_t control = static_cast<uint8_t>(kBitTimerEnable | kTimerSourceOnePerMinute);
  return i2c::writeRegister(kI2cAddress, kRegTimerControl, control);
}

}  // namespace rtc
}  // namespace board
