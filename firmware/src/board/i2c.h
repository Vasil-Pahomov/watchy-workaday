// I2C bus lifetime as an RAII guard.
//
// Constructing a Session powers the bus up; its destructor shuts it down. The
// point is that an early return or an error path cannot skip the shutdown, which
// is exactly how a manual powerDown() at the end of a function gets missed. A bus
// left up across deep sleep leaks current for the whole sleep window — a defect
// that costs days of battery life and is invisible on a bench where the watch is
// plugged in.
#pragma once

#include <stdint.h>

namespace board {
namespace i2c {

class Session {
 public:
  Session();
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

// Both return false on a NACK or a timeout rather than blocking. A wedged device
// must degrade the wake, not hang it.
bool readRegisters(uint8_t address, uint8_t reg, uint8_t* out, uint8_t length);
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);

}  // namespace i2c
}  // namespace board
