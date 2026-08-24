#include "board/i2c.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board_v20.h"
#include "board/diag.h"

namespace board {
namespace i2c {

Session::Session() {
  ok_ = Wire.begin(kPinSda, kPinScl, kI2cFrequencyHz);
  // Bounds every transaction on this bus. Without it a device holding SDA low
  // blocks forever and the watchdog is the only way out.
  Wire.setTimeOut(kI2cTimeoutMs);
  if (!ok_) {
    WD_LOG("i2c: begin failed");
  }
}

Session::~Session() { Wire.end(); }

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* out, uint8_t length) {
  if (out == nullptr || length == 0) {
    return false;
  }

  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {  // keep the bus for the read
    WD_LOG("i2c: addr 0x%02X reg 0x%02X write-phase failed", address, reg);
    return false;
  }

  const uint8_t received = Wire.requestFrom(address, length, static_cast<uint8_t>(true));
  if (received != length) {
    WD_LOG("i2c: addr 0x%02X short read %u/%u", address, received, length);
    return false;
  }

  for (uint8_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t result = Wire.endTransmission(true);
  if (result != 0) {
    WD_LOG("i2c: addr 0x%02X reg 0x%02X write failed (%u)", address, reg, result);
    return false;
  }
  return true;
}

}  // namespace i2c
}  // namespace board
