// When to touch the BMA423, and when to stop trying. Pure logic.
//
// The sensor needs Bosch's ~6 KB feature-engine configuration uploaded before it
// counts anything. That upload is the single most expensive thing this firmware
// does on the I2C bus: 192 chunks, plus a 150 ms delay inside the vendor driver
// and a 20 ms one after its soft reset — call it ~0.85 s against a wake budgeted
// at ~360 ms. It is meant to happen once in the life of a battery charge.
//
// "Meant to" is not an enforcement mechanism, which is the whole reason this
// module exists. The upload can fail in ways that leave the chip looking like it
// still needs one:
//
//   * the config stream loads but the ASIC never reports itself initialised;
//   * the stream loads and a *later* step of the configuration fails, leaving the
//     chip's "initialised" bit set while nothing is actually counting;
//   * the transactions time out rather than NACK, and the wake dies on the
//     watchdog partway through the upload.
//
// Every one of those repeats on the next wake. Without a bounded attempt count
// the watch re-uploads 6 KB every minute for the rest of its life — roughly
// +10 mAh/day against a 9.5 mAh/day allowance, which is the entire power budget
// spent on a step counter that does not work. The third case is worse still: the
// wake never reaches deep sleep, so it is ~10 s at full current per iteration.
//
// So: the attempt count is persisted, the give-up is latched, and both live here
// where `pio test -e native` can reach them rather than in main.cpp between two
// board:: calls where the missing limit went unnoticed.
#pragma once

#include <cstdint>

namespace core {

// What the sensor looks like right now. These are observations, not verdicts —
// board::accel::probe() reports what it read and this module decides what it
// means, so the "should we spend 0.85 s?" question stays testable.
enum class AccelProbe : uint8_t {
  Absent,        // no answer, or the wrong chip ID — treat the sensor as missing
  Unconfigured,  // present, feature engine holds no configuration
  Idle,          // configured, but the accelerometer is not running, so nothing
                 // is being counted — a configuration that stopped half way
  Ready,         // configured and running
};

enum class AccelAction : uint8_t {
  Skip,       // do not touch the sensor further this wake
  Probe,      // read the cheap status registers, then come back with the verdict
  Read,       // read the step total (one 4-byte register read)
  Configure,  // run the expensive upload, then report whether it worked
  Suspend,    // park the sensor, then report whether that worked
};

// Three tries, ~0.85 s each: ~2.6 s of I2C once, ever, rather than 0.85 s every
// minute forever. Deliberately small — a sensor that has not configured in three
// attempts is not going to on the fourth, and the cost of being wrong (steps
// stop) is far below the cost of retrying for ever (the battery budget).
constexpr uint8_t kMaxAccelConfigAttempts = 3;

// Giving up means parking the sensor, and that park is one I2C read-modify-write
// which can itself NACK or time out. If it does, the BMA423 keeps drawing ~14 uA
// — a fifth of the sleep floor — for a feature that is switched off, so the park
// is retried on later wakes. Bounded for the same reason as everything else here:
// each failed attempt is at most two transactions, and three of them is the end
// of it.
constexpr uint8_t kMaxAccelSuspendAttempts = 3;

// Persisted across deep sleep as part of the caller's RTC-backed block, so the
// attempt counts survive the watchdog reset that a wedged transaction causes.
// Cleared only by a fresh persisted block — that is, a power cycle or a version
// bump.
struct AccelState {
  // Attempts since the sensor was last seen healthy at the start of a wake.
  uint8_t config_attempts = 0;
  // Attempts at parking the sensor after giving up on it.
  uint8_t suspend_attempts = 0;
  // An upload was started and has not been seen to finish. Set before the attempt
  // runs precisely so an attempt that never returns is still remembered.
  bool config_incomplete = false;
  // Latched. The expensive path is off for good.
  bool gave_up = false;
  // The sensor still needs parking. Cleared when a park is confirmed, or when the
  // retry budget runs out — never left set to be retried indefinitely.
  bool suspend_pending = false;
};

void resetAccelState(AccelState& state);

// First call of the wake, before any bus traffic at all. Returns Probe in normal
// operation; Suspend if a park is owed and affordable; Skip once we have given up
// and the sensor is parked (or the park budget is spent), which is what keeps a
// dead sensor from costing anything at all on every subsequent wake.
AccelAction accelBeginWake(AccelState& state);

// A probe verdict becomes the next action. Counts the attempt *before* the upload
// runs, because an upload that ends in a watchdog reset never comes back to
// report anything.
AccelAction accelPlan(AccelState& state, AccelProbe probe);

// The expensive path has returned. `ok` is what board::accel::configure() said.
AccelAction accelAfterConfigure(AccelState& state, bool ok);

// The park has returned. `ok` is what board::accel::suspend() said; a false here
// is what buys the retry on a later wake.
void accelAfterSuspend(AccelState& state, bool ok);

}  // namespace core
