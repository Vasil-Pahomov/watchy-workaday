// BMA423 accelerometer — hardware step counter only. Requires an active
// i2c::Session.
//
// ── Why this module looks the way it does ────────────────────────────────────
//
// The BMA423 counts steps autonomously in its own feature engine. That engine is
// blank silicon until Bosch's ~6 KB configuration stream is written into it, and at
// 100 kHz that upload takes several hundred milliseconds — comparable to an entire
// e-paper refresh. Doing it once per wake would roughly double the wake budget for
// a feature that needs no CPU attention at all.
//
// So the module is split by cost:
//
//   probe()          ~3 register reads. Runs every wake.
//   configure()      the expensive upload. Runs only when the policy says so.
//   readStepCount()  one 4-byte read. The hot path.
//
// The chip keeps its configuration as long as it stays powered, which it does
// across our deep sleep, so in practice configure() runs once at power-on and never
// again.
//
// Note the division of labour: this module reports what it *read* and does not
// decide what to do about it. Whether an upload is worth its energy, and when to
// stop trying, is core::accel_policy's job — a decision that expensive belongs
// somewhere `pio test -e native` can reach it.
//
// ── The energy point ─────────────────────────────────────────────────────────
//
// Step counting adds NO wake sources. The accelerometer interrupt stays disarmed:
// the chip accumulates on its own and we collect the total during the minute tick
// we were taking anyway, on the I2C session we were opening anyway. The marginal
// cost is the sensor's own ~14 uA plus about a millisecond of bus traffic — not a
// single extra wake per day. Arming the interrupt would only be needed for
// motion-triggered features, which this module deliberately does not provide.
#pragma once

#include <stdint.h>

#include "core/accel_policy.h"

namespace board {
namespace accel {

// Cheap. Safe to call on every wake.
//
// Reports core::AccelProbe::Ready only when the chip is both configured AND
// actually running. The "configured" bit alone is not enough: it is set by the
// first half of the upload, so a configuration that failed after that point
// leaves a chip that looks initialised and counts nothing, for ever.
core::AccelProbe probe();

// Expensive: soft-resets the chip, uploads the configuration stream and enables
// the step counter. ~0.85 s of I2C including the vendor driver's own delays, so
// call it only when core::accelPlan() asks for it. Returns false on any failure —
// the caller must not retry in a loop; the attempt budget is in the policy.
bool configure();

// Clear acc_en, which parks the accelerometer in suspend. Nothing is sampled, so
// the feature engine stops counting and the sensor drops out of the sleep-current
// budget. For use when the policy has given up: a step counter we have stopped
// reading must not keep charging us its ~14 uA, a quarter of the sleep floor.
// Only the configuration can bring it back, so this is not a power-saving idle.
bool suspend();

// The hot path: reads the chip's free-running 32-bit total. Interpreting it —
// deltas, restarts, midnight — is core::step_counter's job, not this one's.
bool readStepCount(uint32_t& out);

}  // namespace accel
}  // namespace board
