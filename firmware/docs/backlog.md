# Backlog

Work the skeleton does not do yet, roughly in dependency order. Each item is
scoped to be a single `/feature` run.

Every entry names where its logic belongs, because getting that wrong is the most
common defect in this project: **decisions in `core/`, effects in `board/`.**

## First priority — what the first field run left standing

The watch is usable: it boots, keeps time from the phone, counts steps, answers
its buttons and sleeps between wakes. These three are what stand between that and
a watch that meets its own targets.

### 1. Set the time from the watch, without a phone
The BLE sync window sets the PCF8563 now (`PROTOCOL.md`), so a fresh board is no
longer stuck at `--:--` — but only if there is a phone running the companion app.
A watch on its own still has no way to be set, and "Set Time" is still a stub in
the menu.

- `core/`: a `TimeEditor` — which field is selected, what Up/Down do to it,
  field-wise clamping, rollover (31 Jan → Feb clamps the day), commit/cancel.
- `board/`: nothing new; `board::rtc::write()` is already wired up and exercised
  by the sync path.
- Tests: every field's wrap, invalid intermediate states, cancel restores.
- Fold in the 12/24-hour toggle (`use_24h`), which is persisted with no UI.

### 2. Hardware bring-up pass — mostly done
The firmware runs on a physical watch. Confirmed in the field: it boots, the
watchface draws, the clock advances, the buttons wake it, and the BLE window sets
the PCF8563 from the phone. **Deep sleep is entered** — not watched directly, but
a watch that skipped it would be flat inside a working day and this one ran about
a week on one charge.

What the week is **not** is a consumption figure: the cell's real capacity is
unknown, and without it runtime does not divide into mAh/day at all. See "First
field run" in `docs/power-budget.md`.

Two more things the run did not answer, both still worth doing:

- **Is the minute tick coming from the PCF8563 alarm or from the timer backstop?**
  On the face they are identical; they differ only in cadence — 60 s means `ext0`
  works, ~90 s means the countdown timer never fires and the backstop is carrying
  the watch on its own. "The time is right" does not distinguish them, because the
  backstop keeps the time right. Sit with a reference clock and time one update.
  This is still the first thing to check.
- The ⚠ estimates in `docs/power-budget.md` are still estimates. Running is not
  measuring.

The radio's own list: sync working end to end answers the first-order question —
the watch advertises, the phone finds it, connects, writes, and the RTC takes the
epoch. These are the ones it does not answer, because each fails quietly rather
than visibly:

- Does the watch appear as `Workaday`, with the service UUID in the
  **advertisement** and not the scan response? A scanner app that filters by
  service UUID is the quickest check; if filtering fails but a plain scan finds
  it, the payload split is wrong and `autoConnect`/CDM will not work. A sync that
  works today does not settle this: the app can find the watch by other means.
- **Is the advertised MAC the same after a reboot?** `PROTOCOL.md` §2.2 —
  a changing address breaks reconnection silently and looks like an app bug. Reboot
  the watch, then sync again without re-pairing.
- Does the window really end at ~6 s with no phone, and does the watch reach deep
  sleep afterwards? The failure to look for is a window that never tears down: the
  10 s task watchdog would catch it, so the symptom is a reboot loop that degrades
  into Safe mode, not a hang.
- Sleep current **after** a window, compared with a tick that had none. This is the
  measurement that proves the RAII teardown works; everything else about the
  guard is an argument.
- What the 6 s of advertising actually draws. The 11 mA in the budget is an
  estimate carried over from `PROTOCOL.md` §5.3 and has never been metered.

### 3. Measure the sleep current
Every number in the power budget is an estimate. Until one real measurement
exists, "energy efficiency" is an aspiration. Needs a µA-capable meter; see the
measurement notes in `docs/power-budget.md`.

A week on a wrist does not shortcut this, and the reason is worth keeping: it
divides into mAh/day only against a known capacity, and the cell's is not known.
A meter on the rail sidesteps that entirely — current does not depend on what the
cell holds. Two things to establish, in either order:

- the sleep current and the cost of one wake (item 8 puts a GPIO around the wake,
  which is the cheap half and reaches the lever the budget says matters most:
  duration, not count);
- what the cell in this watch actually holds, since every runtime figure in the
  budget is quoted against a rating nobody has checked.

## Features

### 4. ~~Daily step counter (BMA423)~~ — done
Shipped: `core::step_counter` (36 tests) and `core::accel_policy` (16 tests) plus
`board::accel`. The sensor counts autonomously and the total is collected on the
existing minute tick, so the feature costs the BMA423's ~14 µA and **no additional
wakes**. The interrupt stays disarmed.

Counting on hardware, and plausible: across the first week the daily total
tracked a commercial watch worn in the same conditions closely enough that a gross
error would have shown. That is worth what it is worth — it rules out the failure
modes that read a constant zero or count wildly, so `probe()` is reaching `Ready`,
the config blob is landing, and `acc_en` is set. It does not rule out a systematic
scale error, which is exactly the kind that agrees with another watch to within a
glance.

Still worth doing:
- **A hand count over 100 paces.** The only check that catches a scale error, and
  the cheapest thing on this list.
- Are the low-power settings (`CIC_AVG_MODE`, 50 Hz, `NORMAL_AVG4`) actually
  yielding ~14 µA? That is item 3's meter. It is ~0.34 mAh/day either way, so it
  is worth checking alongside the sleep floor rather than on its own.
- Does `probe()` ever return `Idle` (configured but `acc_en` clear)? Not seen so
  far, but "not seen in one week on one watch" is not the same as "cannot happen";
  it is the case that would otherwise read a constant zero for ever.
  `WORKADAY_DIAG=1` prints the attempt count and the give-up.

Still open, split out below: a UI toggle (item 11), step goals/history (item 12)
and a bounded daily retry after the sensor is given up on (item 13).

### 11. Make step counting switchable
`kStepCounterEnabled` is a compile-time constant. It should live in
`PersistedState` with a menu toggle, since turning it off reclaims ~14 µA — about
a fifth of the sleep floor — for someone who does not want the feature. Bump
`kPersistVersion` when the struct changes.

### 12. Step goals and history
Only today and yesterday are kept. A goal, a streak, or a week of history all
belong in `core::step_counter` with tests, not in the render path.

### 13. Daily retry after the accelerometer is given up on
`core::accel_policy` latches `gave_up` for the rest of the power cycle, and on
v2.0 a power cycle means opening the case (see the rough edges at the end). One
retry per 24 h costs 0.85 s/day ≈ **0.007 mAh/day**, under 0.1 % of the
allowance, and would turn a permanent loss into a day-long one.

- `core/`: `accelBeginWake()` gains a "day changed" input — probably
  `daysSinceEpoch` from the already-read clock, which also makes it a no-op while
  the RTC is untrustworthy rather than a free-running timer. Refund exactly one
  config attempt, never the whole budget, so a chip that is genuinely dead settles
  back into the latch after one wasted upload instead of three.
- `board/`: nothing new — `configure()` soft-resets, so it recovers a suspended
  sensor without help.
- Tests: the refund is exactly one attempt; a second refund needs a second day
  boundary; an invalid clock never refunds; the give-up still holds within a day.

### 5. Vibration alerts
- `board/`: a `vibe` module with a bounded pulse. Note that `power::silenceMotor()`
  holds GPIO 13 low across sleep, so the hold must be released before driving it.
- Never a busy-wait: pulse length has to come from a timer, not `delay()`.

### 6. ~~Time sync over the radio~~ — done, over BLE rather than WiFi
Shipped: `core::protocol` + `core::sync_policy` (host-tested) and
`board::ble::Session`, against `PROTOCOL.md`. One window an hour, 6 s of
advertising, ~0.9 mAh/day — about 9 % of the allowance and the most expensive
thing in the firmware.

The original entry proposed **WiFi + NTP**, which would have cost ~0.1 mAh per
sync against BLE's ~0.031 mAh, needed credentials on the watch, and needed a
timezone the watch has no database for. The phone has all three, so it pushes
`utc_epoch_s` and a `utc_offset_min` that already includes DST, and the watch does
no timezone arithmetic at all. WiFi is not planned.

Still open, and deliberately not built (`PROTOCOL.md` §9): bonding and encryption
(the first thing to add), step/battery history in the other direction, and
notifications from the phone.

### 14. ~~Find the phone from the watch~~ — done, unverified on hardware
Shipped: the fourth menu item, `core::find_session` (32 tests) driving
`board::ble::Session::findWait()`, a `Find` characteristic and a `flags` byte in
`PROTOCOL.md` (§3.3, §4.1), and the phone's half in the companion app. The watch
advertises for up to two minutes, redrawing the elapsed time and an attempt
counter every five seconds; the phone connects, does the ordinary sync, reads the
flag in the answer, and rings until the link ends or the user silences it — which
writes `Find`, and the watch then says "phone found".

Two things about it are worth knowing before touching it:

- **It is the one path that stays awake for minutes**, at radio current, and it is
  affordable only because it is user-initiated, bounded by the cap, and refused on
  a low battery or in a degraded mode by the same gates as a sync window. The
  worst case — phone found at once and the search left to run out — is ~1.3 mAh,
  about 14 % of a day; `docs/power-budget.md` has the row. A search also spends the
  hourly sync timer, since it performs the sync if a phone turns up.
- **The Back button is read by a GPIO interrupt during the search**
  (`board::buttons::attachPressInterrupt()`), the only place in the firmware a pin
  is read by anything other than an ext1 wake. `pinMode()` first, because ext1
  leaves the pad muxed to the RTC domain where a digital interrupt never fires;
  detached before sleep, because `deepSleep()` re-arms the same pin as a wake
  source.

Not yet done on a wrist — `BRINGUP.md` stage 5. The things only hardware settles:
that the phone's `autoConnect` actually fires inside the first few rounds rather
than tens of seconds in (the controller's background scan sets that, not us);
that `NimBLEServer::disconnect()` ends the phone's ring promptly; that a
two-minute session with the stack up does not trip anything the twelve-second
window never reached; and what the search really costs on a meter.

### 7. ~~Watchface layout worth looking at~~ — done, and the rule held
The face is now Gilroy ExtraBold throughout (three sizes, `tools/make_time_font.py`),
the charge is a gauge hard against the top-right corner instead of a percentage, and
the step count sits hard against the bottom-left corner as digits alone. `tools/preview_face.py`
renders any of it to a PNG from the generated font header and the layout constants
in `board/display.cpp`, so the next layout change can be looked at without flashing
it.

The `compose()`/`draw()` split survived, and the hash got *cheaper* rather than
richer: it keys on `core::gaugeFillPixels()` — 35 distinct pictures on a 34 px
track — rather than on the 101 percentages behind them, so a percentage that moves
without moving a pixel no longer costs a refresh.

Still open: a second face to switch between, and anything that needs a glyph
outside `0x20..0x7A` (the generated faces carry no more than that).

## Infrastructure

### 8. Wake-duration instrumentation
Toggle a spare GPIO high at the top of `setup()` and low immediately before
`esp_deep_sleep_start()`, then scope it. Cheap, needs no special equipment, and it
directly measures the number that dominates the battery budget. Should be
compile-time gated like `WORKADAY_DIAG`.

### 9. OTA update with rollback
`partitions.csv` already reserves two 1.625 MB OTA slots and nothing uses them.
Must verify before marking a slot valid, so a bad image rolls back instead of
bricking the watch.

### 10. Read the coredump partition
A panic already writes a core dump to flash; there is currently no way to get it
out. A menu entry or a serial command that reports the last crash would make
field faults diagnosable.

## Known rough edges in the skeleton

- **The pad hold bricked a watch, and the shape of it is worth keeping.** Fixed
  now (`board::power::releaseSleepHold()`), recorded because the *sequence* is the
  reusable part and it will recur in a different costume.

  `deepSleep()` calls `gpio_deep_sleep_hold_en()` so nothing floats while the chip
  sleeps. Nothing released it. The hold lives in `RTC_CNTL`, which a core reset, a
  watchdog reset and a brownout all leave alone — only a power-on reset clears it.
  So the first sleep froze every pad permanently, U0TXD and U0RXD included: no
  serial output, and the ROM download loader could not answer esptool, which
  reported "No serial data received" through `default_reset`, `no_reset` and a
  hand-held EN-low-with-IO0-low sequence alike. v2.0 has no power switch, so the
  recovery was opening the case to disconnect the battery.

  Three things made it invisible for as long as it was:

  1. **It was hidden behind another defect.** Before `kFlashBus`, `quiesceGpios()`
     killed the chip partway through the sweep, so `gpio_deep_sleep_hold_en()` had
     never once executed. Fixing the first defect ran the second for the first
     time. Expect this: a masked defect is not a rare thing, it is what fixing a
     crash *does*.
  2. **The exclusion lists did not protect what they looked like they protected.**
     `kDiagUart` keeps GPIO 1 and 3 out of the sweep — and the hold latched them
     anyway. A never-touch list protects a pin from `pinMode()`, not from a hold.
  3. **The blast radius was the diagnostic channel itself.** With U0TXD frozen,
     `Serial.begin()` still succeeds and emits nothing, so the failure erases its
     own evidence. `releaseSleepHold()` is therefore called *before*
     `WD_DIAG_BEGIN()` rather than merely early.

  Also fixed in passing: the sweep was setting GPIO 0, 2 and 15 — three of the
  ESP32's five boot straps — to `pinMode(INPUT)`, which disables both internal
  pulls and leaves them floating, and it was that floating level the hold latched.
  They are in `kStrappingPins` now. GPIO 5 (display CS) and GPIO 12 (MTDI, the
  accelerometer's INT2) are the other two straps and were already in
  `kLeaveAlone`; 12 is the one to watch, since MTDI high at reset puts 1.8 V on the
  PICO-D4's 3.3 V embedded flash.

- **The GPIO sweep leaves pins as floating inputs with the input buffer on.**
  `pinMode(pin, INPUT)` enables the input buffer and disables both pulls, which is
  the classic floating-CMOS-input leak — an unconnected pad can oscillate and burn
  microamps against a ~20 uA budget. `pinMode(pin, ANALOG)` (`GPIO_MODE_DISABLE`)
  would turn the buffer off instead. Not changed here because it belongs with the
  sleep-current measurement (item 3) that would show whether it matters, and
  changing the sweep untested is what started this.

- **BCD conversion in `board/rtc.cpp` is untested.** It is register formatting
  rather than policy, so it sits in `board/` by design, and `core::isValid()`
  catches garbage downstream — but a subtle BCD bug could produce a *plausible*
  wrong time that passes validation. Worth extracting if it ever misbehaves.
- **`minutes_since_full` does not accumulate on wakes that skip the display.**
  Harmless today, since the only such wakes are accelerometer wakes (disabled) and
  non-power-on Recovery wakes (where it does not matter). It would become a real
  gap if either changes.
- **The step plausibility limit assumes the tick interval is honest.** If
  `elapsedMinutes()` falls back to an inferred value after a long clock outage, a
  burst of real steps can be rejected. Bounded by design (`kMaxCreditedGapMinutes`)
  and preferable to crediting garbage, but it does mean steps can be silently lost
  when the RTC is unhealthy. Rejection is now a pause rather than a permanent
  stop: `kMaxConsecutiveRejections` in a row drops the baseline and re-primes from
  the next reading, crediting nothing for the gap.
- **The day does not roll over while step readings are stale.** The rollover lives
  inside `updateSteps()`, which only runs when a reading arrives, so an outage
  spanning midnight leaves `day_epoch` on the old day. Deliberate: nothing is
  displayed during the outage (`stepsDisplayFor()` reports `Stopped`).

  On recovery, `today` reads 0 — which is the right answer, but **not** because
  the rollover runs first. `updateSteps()` credits the delta and rolls the day
  *afterwards*; that ordering is deliberate and has its own test
  (`test_steps_across_the_midnight_tick_belong_to_the_old_day`). `today` is 0
  because the roll zeroes it after the credit, not because the credit was skipped.
  **Do not "simplify" `updateSteps()` by moving the roll ahead of the credit** —
  it would break cross-midnight attribution on every ordinary tick.

  The real residue is on `yesterday`, and it is larger than it looks: the recovery
  delta is credited to the old day first, so `yesterday` ends up holding that day's
  genuine total **plus** whatever was walked after midnight, and it keeps it until
  the next real midnight. Note the asymmetry — a *long* outage is the clean case,
  because the oversized delta is rejected outright and `yesterday` stays exact;
  only a short midnight-spanning outage inflates it, bounded by
  `maxPlausibleDelta(elapsed)`. Visible on the steps detail screen only.
- **A sensor that dies in service is detected by a clock, not by an error.** It
  never gives up (an unanswering chip is not charged a configuration attempt, so
  that a transient can recover) and stays primed, so `core::kStaleStepMinutes`
  worth of expected-but-missing readings is what moves the face to "no step data".
  Ten minutes at the normal tick, two missed wakes on the low-battery tick.
- **`elapsed` is measured since the last wake, not since the last step read.** The
  two differ only when a wake skips the sensor — Safe mode, or after the accel
  policy has given up — and the effect is that the first reading afterwards can be
  rejected for a gap it did not actually have. Self-correcting within
  `kMaxConsecutiveRejections` readings, at the cost of the steps in that window.
- **Giving up on the accelerometer lasts until a power cycle, and on this
  hardware that is a real cost.** `AccelState` is in RTC memory, so `gave_up`
  survives deep sleep and reboots by design — that is what bounds the ~0.85 s
  upload. But Watchy v2.0 has no power switch and the cell is inside the case, and
  reflashing does **not** clear RTC RAM (that is the whole premise of the version
  check). So the recovery path available to a wearer is: open the case and unplug
  the battery, or let the cell go flat. A transient bus wedge costs the step
  counter permanently, and the face correctly shows "no step data" for the rest of
  the charge.

  This is deferred, not settled. The reason is *not* that any retry is unbounded —
  that would be a false dichotomy, and it would be used later to reject a cheap
  fix. **One attempt per 24 h is trivially bounded: 0.85 s/day ≈ 0.007 mAh/day,
  under 0.1 % of the allowance.** It is deferred only because it needs a clock
  input in `core::accel_policy`, which today is a pure state machine with no
  concept of time, and because there is still nothing to say how often this
  failure actually occurs: one watch has now run about a week without the sensor
  being given up on, which is one data point against a failure this is meant to
  recover from, not evidence that it does not happen. Item 13 below.
- **BCD conversion in `board/rtc.cpp` is untested** (see above), and
  `board::accel` mirrors six register constants from the vendor headers. Both are
  places where a library version bump could drift without the build noticing.
- **12/24-hour mode is persisted but has no UI** to change it (`use_24h` in
  `main.cpp`). Fold it into item 1.
- **`NimBLEDevice::init()` spins and can abort, and neither is ours to fix.** Two
  things inside the library sit awkwardly against Laws 1 and 2:

  1. It ends in `while (!m_synced) { taskYIELD(); }` — an untimed spin waiting for
     the controller to sync with the host. In practice that is a few milliseconds.
     If the controller never syncs it never returns.
  2. Controller and HCI bring-up go through `ESP_ERROR_CHECK`, so a failure
     (realistically `ESP_ERR_NO_MEM`) panics rather than returning an error.
     `PROTOCOL.md` §6.1 asks for "log, skip the window, sleep normally", and the
     library gives us no way to do that for those specific calls.

  Both are **bounded at the system level rather than at the call**, which is why
  this is a rough edge and not a defect: the 10 s task watchdog is armed and was
  fed immediately before the window, so a spin ends in a reset; a panic also ends
  in a reset with a coredump; `core::health` counts either as a fault; three
  consecutive faults put the watch in `Safe` mode, where `core::sync_policy`
  refuses the radio outright. So the worst case is three reboots and then a watch
  that stops trying — degraded, still telling the time, not flat and not frozen.
  Every failure the library *does* report (`createServer`, `createService`,
  `createCharacteristic`, `service->start()`, `startAdvertising()`) is handled the
  way §6.1 asks.

  Fixing it properly means vendoring a patched `NimBLEDevice::init()`, which costs
  the pinned-dependency guarantee in Law 4. Worth revisiting only if bring-up
  shows it actually happens.

  The **teardown** was suspected of the same problem and is not: `deinit()` →
  `nimble_port_stop()` does wait on a semaphore with `BLE_NPL_TIME_FOREVER`, but
  `ble_hs_stop()` arms `ble_hs_stop_terminate_tmo` for `BLE_HOST_STOP_TIMEOUT_MS`
  first and releases the semaphore on expiry, explicitly for peers that have gone
  out of range mid-link. That resolves to
  `CONFIG_BT_NIMBLE_HS_STOP_TIMEOUT_MS` = **2000 ms** in the vendored
  `nimconfig.h`. It is a bare `#define`, so it cannot be lowered from
  `platformio.ini` without a redefinition warning — 2 s is what we have, and it is
  a bound. The second `sem_pend` is a host-task event round trip and does not
  involve the peer.
- **A disconnect ends the window; the watch does not resume advertising.** If a
  central connects and drops before writing, the remainder of the 6 s is not spent
  looking for another one, and `NimBLEServer::advertiseOnDisconnect(false)` makes
  that deliberate — the library's default would leave the radio running past the
  teardown. `PROTOCOL.md` §6.1 says "tear down, sleep", and the phone's own backoff
  brings it back. The cost is that a stray scanner connecting during a window can
  waste that hour's sync. Unbonded and unencrypted by design (§2.3), so anything
  can connect; if this turns out to happen in practice, §2.3's v2 bonding item is
  the answer, not re-advertising.
