#include "board/accel.h"

#include <Arduino.h>
#include <Wire.h>

// SensorLib's headers do not compile clean under the project's warning set: they
// shadow declarations in inline members and widen float literals to double. Those
// are the vendor's business, not ours, but they arrive through OUR translation
// unit, where build_src_flags applies. Left alone they bury a genuine
// -Wdouble-promotion in our own float math under 28 identical third-party ones,
// which is precisely the signal that flag exists to give.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include <SensorBMA423.hpp>
#pragma GCC diagnostic pop

#include "board/board_v20.h"
#include "board/diag.h"
#include "board/i2c.h"

namespace board {
namespace accel {
namespace {

// Mirrored from the vendor driver's bma4_defs.h / bma423.h so the hot path can read
// registers directly without dragging the driver object in. Kept adjacent to the
// names they came from, so a version bump is easy to re-check.
constexpr uint8_t kI2cAddress = 0x18;          // BMA4_I2C_ADDR_PRIMARY
constexpr uint8_t kRegChipId = 0x00;           // BMA4_CHIP_ID_ADDR
constexpr uint8_t kRegStepCountOut0 = 0x1E;    // BMA4_STEP_CNT_OUT_0_ADDR
constexpr uint8_t kRegInternalStatus = 0x2A;   // BMA4_INTERNAL_STAT
constexpr uint8_t kRegPowerCtrl = 0x7D;        // BMA4_POWER_CTRL_ADDR
constexpr uint8_t kChipIdBma423 = 0x13;        // BMA423_CHIP_ID
constexpr uint8_t kInternalStatusMask = 0x0F;  // BMA4_CONFIG_STREAM_MESSAGE_MSK
constexpr uint8_t kAsicInitialized = 0x01;     // BMA4_ASIC_INITIALIZED
constexpr uint8_t kAccelEnableMask = 0x04;     // BMA4_ACCEL_ENABLE_MSK (bit 2)

// 50 Hz is Bosch's recommendation for step counting: enough to resolve a stride,
// low enough to stay in the sensor's low-power budget.
constexpr float kStepOdrHz = 50.0f;

// 0 disables the watermark interrupt while leaving the counter running, which is
// exactly what we want — nothing is wired to the INT pin for this feature.
constexpr uint16_t kNoWatermarkInterrupt = 0;

}  // namespace

core::AccelProbe probe() {
  uint8_t chip_id = 0;
  if (!i2c::readRegisters(kI2cAddress, kRegChipId, &chip_id, 1)) {
    WD_LOG("accel: no response");
    return core::AccelProbe::Absent;
  }
  if (chip_id != kChipIdBma423) {
    WD_LOG("accel: unexpected chip id 0x%02X", chip_id);
    return core::AccelProbe::Absent;
  }

  uint8_t internal = 0;
  if (!i2c::readRegisters(kI2cAddress, kRegInternalStatus, &internal, 1)) {
    return core::AccelProbe::Absent;
  }
  if ((internal & kInternalStatusMask) != kAsicInitialized) {
    return core::AccelProbe::Unconfigured;
  }

  // The config stream is loaded — but that bit is set by the first half of
  // configure() and says nothing about the rest of it. If configAccelerometer()
  // failed, or the chip dropped back to suspend, acc_en is clear and the step
  // engine has no samples to work with: the counter is frozen and every read
  // returns the same plausible number for ever. Report that as its own state and
  // let core::accelPlan() decide what it is worth.
  uint8_t power = 0;
  if (!i2c::readRegisters(kI2cAddress, kRegPowerCtrl, &power, 1)) {
    return core::AccelProbe::Absent;
  }
  if ((power & kAccelEnableMask) == 0) {
    WD_LOG("accel: configured but not running (pwr=0x%02X)", power);
    return core::AccelProbe::Idle;
  }
  return core::AccelProbe::Ready;
}

bool configure() {
  // Deliberately a local, not a static or a global. The driver object owns a
  // heap buffer and a couple of unique_ptrs; confining it to this rarely-taken
  // branch keeps that allocation off the once-a-minute path entirely, and its
  // destructor only frees that buffer — it does not put the sensor back to sleep,
  // so the configuration we just uploaded survives the object going away.
  SensorBMA423 sensor;

  // The whole cost of this feature lives on this line: begin() soft-resets the
  // chip, waits 20 ms, streams 6 KB in 192 chunks and waits a further 150 ms
  // inside the vendor driver. Those delays are the vendor's, not ours, and they
  // are affordable only because core::accel_policy caps how often this runs.
  // The soft reset also zeroes the step total; core::step_counter sees that as a
  // counter restart and re-baselines rather than crediting it.
  if (!sensor.begin(Wire, kI2cAddress, kPinSda, kPinScl)) {
    WD_LOG("accel: begin failed");
    return false;
  }

  // Watchy is worn on a wrist; the smartphone profile mis-detects strides.
  if (!sensor.selectPlatform(SensorBMA423::Platform::WRISTBAND)) {
    WD_LOG("accel: platform select failed");
    return false;
  }

  // CIC averaging rather than continuous sampling: the step engine does not need
  // low-latency raw samples, and averaging is the cheaper of the two.
  if (!sensor.configAccelerometer(OperationMode::NORMAL, AccelFullScaleRange::FS_2G,
                                  kStepOdrHz, AccelBandwidth::NORMAL_AVG4,
                                  AccelPerfMode::CIC_AVG_MODE)) {
    WD_LOG("accel: config failed");
    return false;
  }

  // reset_counter = true so the total starts from a known zero on a fresh
  // configuration. core::step_counter treats the first reading as a baseline
  // anyway, so this is belt and braces rather than load-bearing.
  if (!sensor.enableStepCounter(true, kNoWatermarkInterrupt, /*reset_counter=*/true)) {
    WD_LOG("accel: step counter enable failed");
    return false;
  }

  WD_LOG("accel: configured");
  return true;
}

bool suspend() {
  // Read-modify-write rather than a blind store: bit 0 of this register is the
  // auxiliary-interface enable, which the config stream may have set and which is
  // not ours to clear.
  uint8_t power = 0;
  if (!i2c::readRegisters(kI2cAddress, kRegPowerCtrl, &power, 1)) {
    return false;
  }
  const uint8_t parked = static_cast<uint8_t>(power & static_cast<uint8_t>(~kAccelEnableMask));
  return i2c::writeRegister(kI2cAddress, kRegPowerCtrl, parked);
}

bool readStepCount(uint32_t& out) {
  uint8_t raw[4] = {};
  if (!i2c::readRegisters(kI2cAddress, kRegStepCountOut0, raw, sizeof(raw))) {
    return false;
  }
  out = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
        (static_cast<uint32_t>(raw[2]) << 16) | (static_cast<uint32_t>(raw[3]) << 24);
  return true;
}

}  // namespace accel
}  // namespace board
