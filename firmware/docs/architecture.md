# Architecture

## The shape of the firmware: no `loop()`

The watch is a **stateless, one-shot program that runs ~1440 times a day.**

```
wake  →  setup()  →  decide  →  act minimally  →  deep sleep
```

`loop()` exists but is never reached: `setup()` ends in
`board::power::deepSleep()`, which does not return. Deep sleep is not a power
optimisation bolted on afterwards — it is the control flow. Each wake is a fresh
boot with no heap history, which is also what keeps the firmware stable over
months: nothing accumulates because nothing survives except the small,
version-tagged block in `RTC_DATA_ATTR`.

## The core/board split

This split serves Law 1 and Law 3 simultaneously.

```
src/core/    pure C++17 · no hardware headers · host-tested · DECISIONS
src/board/   Watchy 2.0 registers and pins · thin · no policy · EFFECTS
src/app/     screens and rendering
```

The rule: **decisions in `core/`, effects in `board/`.**

An `if` inside `board/` that decides *whether* to do something is misplaced —
move the condition to `core/` where a unit test can reach it, and leave `board/`
with an unconditional "do the thing". This is what makes an energy-critical
codebase testable: the expensive choices (power up the ADC? refresh the panel?
enter Safe mode?) are exactly the choices that are pure functions of state, so
they can all be tested on the host.

The boundary is mechanically enforced: the `native` environment compiles only
`src/core/**`, so an `<Arduino.h>` in `core/` breaks the test build immediately.

## Core modules

| Module | Owns | Energy/reliability role |
|---|---|---|
| `time_model` | calendar/clock arithmetic, validity, formatting | next-alarm computation; rejects garbage RTC time (P0) |
| `battery_model` | mV → %, smoothing, hysteresis, level state | triggers low-battery 5-minute mode; `batterySaving()` is that mode stated on the face, so the wearer can tell it from a fault |
| `refresh_policy` | partial vs full vs **skip** | skipping a refresh removes ~85 % of a wake's cost |
| `wake_router` | wake reason → what to power up | the module that decides what *not* to do |
| `health` | boots, faults, run mode across resets | crash-loop escalation to Safe/Recovery |
| `ui_state` | screen/menu state machine, idle timeout, which item a press activated, whether a press flips the display theme | keeps UI logic out of the render path; `activatedMenuItem()` is the only way a user sync request reaches the radio, and `themeAfterButton()` is what makes a theme flip force a full refresh instead of ghosting a partial one |
| `step_counter` | daily totals from the sensor's raw counter, and whether they are current enough to show | rejects garbage reads; no wake cost of its own |
| `accel_policy` | whether to configure the BMA423, and when to stop | caps the ~0.85 s config upload at 3 attempts per power cycle |
| `sync_policy` | whether a BLE sync window opens on this wake, and the state a window leaves behind | the gate on the most expensive thing the firmware can do; the hourly timer is spent when a window *opens*, so a phone that is never there costs 24 windows a day and not 1440 |
| `sync_window` | how long the watch may wait at each step of a window that *has* opened, and what the radio's signals mean | holds the invariant the task watchdog depends on — no wait exceeds 6 s and none outlives the 12 s cap, whatever the radio reports |
| `find_session` | the same for a find-phone search (`PROTOCOL.md` §4.1): rounds, the attempt counter, the three early endings, where the wearer lands afterwards | the one path that stays awake for minutes; every wait it hands out is still under the 6 s bound, and the 120 s cap ends it whatever the radio or the wearer does |

`step_counter` is a good illustration of the split. `board::accel` hands over one
number — the BMA423's free-running total — and every awkward question about it is
answered in `core/`, where each has a test: the counter restarting when the sensor
is reconfigured, midnight arriving, the watch having been off for two days, a
corrupted I2C read looking like 40 000 steps. None of that needs a wrist.

`accel_policy` is the same split applied to the *cost* of that sensor rather than
its data. `board::accel::probe()` reports what three registers say; the policy
decides whether those readings justify a 6 KB upload, and — the part that only
exists because it is testable — how many times it may decide that before giving
up. An unbounded version of that decision is a +10 mAh/day defect against a
9.5 mAh/day allowance, and it would sit in `main.cpp` where no test could see it.

`wake_router` is the heart of Law 1. Given a wake reason and a state snapshot it
returns a `WakePlan` — a plain struct saying which peripherals to bring up. The
minute tick does not read the accelerometer; it does not read the battery unless
the sample is due. Because it is a pure function, "does a button wake power up the
ADC?" is a unit test rather than a bench measurement.

## Board modules

Thin wrappers, one concern each: `power` (sleep, wake-source detection, GPIO
quiescing), `display` (GxEPD2 + hibernate), `rtc` (PCF8563 alarm), `battery`
(ADC), `buttons` (active-high reads), `accel` (BMA423), `ble` (NimBLE peripheral),
`vibe`, `watchdog`.

Peripheral lifetime uses **RAII session guards** — `board::i2c::Session`,
`board::display::Session` and `board::ble::Session`. Constructing one powers the
peripheral up; its destructor powers it down. This makes the leak that Law 1 cares
about structurally impossible: an early return or an error path cannot skip the
destructor, whereas it trivially skips a manual `powerDown()` call at the end of a
function. Most real-world sleep-current bugs are exactly that skipped call on an
error path.

`board::ble` is the guard that matters most, because the radio is the largest
consumer on the board by two orders of magnitude. Its session opens one sync
window and reports what happened; it decides nothing — `core::sync_policy` already
said whether a window may open, `core::sync_window` owns how long each step may
wait, and `core::protocol` judges the bytes that arrive. What is left in `board/`
is a genuine effect and nothing else: bring the stack up, block on a FreeRTOS
event group for however long it was told, hand back what the callbacks reported,
tear the radio down. It returns to `main.cpp` at each of `PROTOCOL.md` §5.1's
progress points so that the watchdog is fed there and nowhere else. NimBLE rather
than Bluedroid: roughly half the flash and ~100 KB less RAM, and the central and
observer roles are compiled out because the watch never scans and never connects
out.

The split between `sync_policy`, `sync_window` and `ble` is worth reading as one
example. The timeout bookkeeping started inside `board::ble` and was the one part
of that file which was not a pure effect — a running total, two deadlines and a
priority order over three signals. It carried a defect a bench would never find
(a terminal signal that was never consumed, so every later wait returned instantly
and a looping caller span at radio current until the watchdog reset it) and a unit
test found in a second once it moved. That is the whole argument for the core/board
line, in the module where the stakes are highest.

The find-phone session (`PROTOCOL.md` §4.1) reuses that split rather than
extending it: `core::find_session` is a second classifier over the same radio
signals plus the session's own — a write on the `Find` characteristic, the phone
subscribing to it, and the wearer's Back and Menu presses — and
`board::ble::Session` gains a second wait that blocks on all of those bits where
the sync wait blocks on three. The button presses are the inputs that are neither
the radio's nor the wake's: each arrives as a GPIO edge, through
`board::buttons::attachPressInterrupt()`, into the same FreeRTOS event group the
wait sleeps on — so the search wakes on it within a millisecond and never polls a
pin. Menu is the one that needs judgement, and the judgement is in `core/`: an edge
only starts a 40 ms settle clock, and the press counts if the board still reads
the pin high when it runs out (`FindSignals::menu_held`, sampled by `main.cpp`
after the wait). Without that, the release of the press that opened the search
bounces straight into the tone toggle. `main.cpp`'s `runFindSession()` is the
loop, and its shape is the sync window's: one event per pass, a redraw and a
watchdog feed after each completed step, `StillWaiting` feeding nothing.

The ADC deliberately has **no** guard: the one-shot driver powers the SAR ADC per
conversion, so there is nothing to release, and a guard would advertise protection
that does not exist.

One honest wrinkle: the I2C session is opened on *every* wake regardless of the
plan, because the PCF8563's tick has to be re-armed and its interrupt flag cleared
each time. `WakePlan::need_i2c` therefore means "a device must be **read**", not
"the bus comes up".

## State across sleep

Persistent state lives in **`RTC_NOINIT_ATTR`**, and the distinction from
`RTC_DATA_ATTR` is the one thing to get right here. Both put a variable in RTC
slow memory, which the RTC power domain keeps alive through deep sleep. They
differ in what the *bootloader* does to them:

```
.rtc.data    (RTC_DATA_ATTR)    CONTENTS, ALLOC, LOAD   <- in the image, restored on boot
.rtc_noinit  (RTC_NOINIT_ATTR)  ALLOC                   <- never written by anything
```

`.rtc.data` is a loadable segment, so the bootloader refills it from flash on
every boot that runs the bootloader. A deep-sleep wake skips the bootloader; **a
panic, a watchdog reset and a brownout do not.** State placed there therefore
survives the ordinary case and is silently erased by exactly the faults it is
usually there to outlive — `core::health`'s consecutive-fault count, the sync
window's hourly timer, the accelerometer's configuration budget. This was a real
defect in this firmware, not a hypothetical: it made Safe and Recovery mode
unreachable by any reset-type fault, because the count never got past one.

`.rtc_noinit` is `ALLOC` only, so nothing ever writes it — which also means it
**survives reflashing**, and on the first boot of new firmware it holds the
previous build's bytes. So each block carries a magic number and a schema
version and is validated before use; failing validation means re-initialising to
defaults, not trusting it. Under `RTC_DATA_ATTR` those checks were nearly
decorative (a fresh block arrived pre-zeroed on every non-deep-sleep boot); under
noinit they are load-bearing.

One consequence worth stating: a variable in `.rtc_noinit` must not acquire a
runtime initialiser. The blocks here are aggregates with constexpr implicit
default constructors, so they are statically initialised and the linker's NOLOAD
drops the bytes — nothing runs at boot to overwrite what the last run left.
Adding a user-declared constructor would emit a dynamic initialiser and quietly
restore the defect. Check `objdump -h` for `.rtc_noinit` staying `ALLOC`-only, and
the absence of a `_GLOBAL__sub_I` for the translation unit.

Anything larger or longer-lived belongs in NVS.

## Reliability layers

1. **Task watchdog** armed early in `setup()`, fed only at genuine progress
   points — never inside a wait loop, which would defeat it. The BLE window is the
   case that tests this rule, because a radio window is exactly what a wait loop
   looks like: it is resolved by bounding every un-fed interval below the 10 s
   timeout (6 s advertising, 4 s connected-idle, 12 s cap) rather than by feeding
   from inside the wait. The teardown tail that used to sit on that interval —
   ~2.05 s of NimBLE host-stop wait — is gone with the `NimBLEDevice::deinit()`
   call that caused it (a core-0 panic on every window; `board/ble.cpp` has the
   mechanism), so the worst un-fed interval is ~6.05 s against the 10 s timeout.
   The three feeds it does take — the stack up and synced, a
   central connected, a Time write processed — are genuine progress, and all three
   happen in `main.cpp` on the task the watchdog is subscribed to, not in a NimBLE
   callback.

   The find-phone session is the same rule applied for two minutes instead of
   twelve seconds: `core::find_session` never hands out a wait over 5 s, and each
   wait ends in a completed step — a connect, a processed write, or a redraw of
   the elapsed counter — that `main.cpp` feeds after. Twelve watchdog periods,
   never one un-fed interval longer than the sync window's. The cost of staying
   awake that long is energy, priced in `docs/power-budget.md`, not reliability.
2. **Timeouts everywhere.** Every I2C transaction and BUSY wait is bounded; a
   timeout degrades and sleeps rather than retrying forever.
3. **A timer wake as a backstop.** If the RTC alarm is ever missed or misset, the
   ESP32's own timer still wakes the watch, so a bad alarm cannot become a
   permanent freeze.
4. **Fault escalation.** `core::health` counts consecutive faults; 3 → `Safe`
   (minimal draw, no radio), 6 → `Recovery` (long sleep, static screen). A boot
   that survives to sleep clears the counter. This turns a crash loop — which
   would flatten the battery in hours — into a degraded but living watch.
5. **Coredump partition.** A panic leaves a post-mortem in flash instead of
   vanishing into a reboot.
6. **Two OTA slots**, so a bad future update can roll back.

## Deliberately not portable

Law 5. No revision `#if`s, no HAL for hypothetical hardware. `board_v20.h`
`#error`s unless the build is configured for v2.0. Portability would cost flash,
RAM and wake time — budgets being spent on battery life instead.

We depend on GxEPD2 for the panel, **not** on the upstream Watchy library, whose
structure exists largely to span four hardware revisions.
