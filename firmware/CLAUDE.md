# Workaday — Watchy 2.0 firmware

Firmware for the **SQFMI Watchy v2.0** (ESP32-PICO-D4, 200×200 e-paper, PCF8563 RTC,
BMA423 accelerometer, 200 mAh LiPo).

Development runs as a two-agent loop: a **coder** subagent writes changes, a
**reviewer** subagent audits them against the rules below and cannot edit code.
See `docs/workflow.md`. The five project laws are non-negotiable — they outrank
convenience, brevity, and "it works on my bench".

---

## Law 1 — Energy is the primary design currency

The watch runs off 200 mAh. Every awake millisecond is a withdrawal from the
battery. The CPU is **asleep by default**; being awake is the exception that must
be justified.

Required, not optional:

- **Deep sleep is the resting state.** There is no `loop()` that spins. Every
  wake path must terminate in `board::power::deepSleep()`. A code path that can
  return from a wake without sleeping is a bug, however rare.
- **No busy-waiting.** `delay()`, spin loops, and `while (!flag) {}` are
  forbidden in production paths. Wait on an interrupt, an RTC alarm, or a
  peripheral's own BUSY line via a blocking-with-timeout helper — never a
  poll loop that burns cycles.
- **Race to sleep.** Do the minimum work, then sleep. Do not "keep things warm
  in case" — re-initialising a peripheral is cheaper than holding it powered
  across a sleep cycle.
- **Peripherals are off unless in use, and the code that turns one on owns
  turning it off.** I2C bus, SPI bus, e-paper panel, vibration motor, radio.
  Use the RAII guards in `src/board/` (`board::i2c::Session`,
  `board::display::Session`) so the "off" path cannot be skipped by an early
  return. The ADC needs no guard — the one-shot driver powers it per conversion,
  and a guard implying otherwise would be worse than none.
- **Never enable WiFi or BLE on a routine wake.** The radio is the single
  largest consumer on this board — hundreds of milliamps against a ~20 µA sleep
  floor. Radio use is user-initiated or scheduled, never incidental, and always
  has a hard timeout.
- **The display is the second largest cost.** Prefer partial refresh; skip the
  refresh entirely when the rendered content is unchanged (see
  `core::refresh_policy`). A full refresh happens only to clear ghosting.
  `display.hibernate()` before sleeping, always.
- **Quiesce GPIOs before sleeping.** Floating inputs and pins driving a
  powered-down peripheral leak current. Only the wake sources stay live.
- Do not read the battery ADC on every wake — it is sampled on a schedule.

When adding a feature, state its energy cost in the PR/commit description:
how often it wakes the CPU, for how long, and which peripherals it powers.

## Law 2 — Reliability: it must never hang, and it must heal itself

The watch is expected to run for months unattended. The failure mode we refuse
to ship is a **frozen watch requiring a manual reset**. Rebooting is always
preferable to hanging.

Required:

- **Watchdog armed on every wake.** The task watchdog is enabled early in
  `setup()` and fed only at genuine progress points — never inside a wait loop,
  which would defeat it.
- **Every blocking hardware call takes a timeout**, and a timeout is a handled
  error, not an assertion. Peripheral absent or wedged → log, degrade, sleep.
  Never retry forever.
- **No unbounded retries and no unbounded allocation.** Prefer fixed-size
  buffers and `static` storage; the heap fragments over months of uptime.
- **A panic must reboot, not sit at a prompt.** A crash leaves a coredump in
  flash and the watch comes back up.
- **Fault escalation is tracked across boots.** `core::health` counts
  consecutive faults in RTC-backed memory and degrades the firmware into
  `Safe` and then `Recovery` mode rather than crash-looping. A boot that
  survives to sleep clears the counter.
- **RTC time is validated, never trusted.** A dead or unconfigured PCF8563
  returns garbage; `core::time_model::isValid()` gates it, and an invalid time
  degrades gracefully instead of propagating into a bogus alarm that never
  fires.
- State that must survive deep sleep lives in `RTC_DATA_ATTR` and must be
  version-tagged and validated on read — deep sleep memory is not zeroed and
  survives firmware flashes.

## Law 3 — The logic is unit-tested on the host

- `src/core/**` is **pure C++17**: no `<Arduino.h>`, no `<esp_*.h>`, no `Wire`,
  no globals with I/O side effects. It is compiled and tested on the developer's
  machine with `pio test -e native`. This is enforced by the `native`
  environment's `build_src_filter` — if you include Arduino headers in `core/`,
  the host build breaks.
- `src/board/**` is the only place hardware headers appear. It stays thin —
  register pokes and a narrow function, no policy or decision-making.
- **Decisions belong in `core/`, effects belong in `board/`.** If you catch
  yourself writing an `if` that decides *whether* to do something inside
  `board/`, that `if` belongs in `core/` where a test can reach it.
- Every new function in `core/` ships with tests in the same change, including
  boundary cases (midnight, leap year, 0 %, empty buffer, clock rollover).
- `pio test -e native` passing is a **merge gate**. A change that breaks it is
  not done.

## Law 4 — VS Code + PlatformIO must work out of the box

- `git clone` → open the folder in VS Code → PlatformIO resolves and builds.
- All dependencies are declared with version constraints in `platformio.ini`.
  Nothing is installed by hand, nothing lives in a global library folder.
- The platform version is pinned. Do not float it.
- Both `pio run -e watchy_v20` and `pio test -e native` must succeed from a
  clean checkout.

## Law 5 — Watchy 2.0 only, deliberately

This firmware is **not** portable and must not become portable. Portability
costs flash, RAM, wake time and clarity — all four are budgets we are spending
on battery life instead.

- No `#if` on hardware revision. No abstraction layer "in case of v3".
- Exploit v2.0's specifics freely: its exact pin map, its PCF8563, its
  active-**high** buttons, its 2.0× battery divider.
- `src/board/board_v20.h` is the single source of truth for pins, and it
  `#error`s if the build is not configured for v2.0.
- Reject upstream Watchy-library patterns that exist only to support several
  revisions. We depend on GxEPD2 for the panel, not on the Watchy library.

---

## Verified hardware facts (v2.0 — do not "fix" these from memory)

These were taken from the upstream `sqfmi/Watchy` `config.h` and `Watchy.cpp`.
v2.0 differs from v1.5 in ways that are easy to get wrong:

| Function | Pin | Notes |
|---|---|---|
| I2C SDA / SCL | 21 / 22 | RTC + accelerometer share the bus |
| Buttons: Menu / Back / Up / Down | 26 / 25 / **35** / 4 | **active HIGH** |
| Battery ADC | **34** | multiply `analogReadMilliVolts()` by **2.0** |
| RTC interrupt (PCF8563) | 27 | **active LOW** |
| Accelerometer INT1 / INT2 | 14 / 12 | BMA423 |
| Vibration motor | 13 | |
| Display CS / DC / RES / BUSY | 5 / 10 / 9 / 19 | 200×200, GxEPD2 |

Traps:

- **GPIO 34 and 35 are input-only** and have **no internal pull-up/pull-down**.
  The Up button on 35 relies on the board's external resistor. Calling
  `pinMode(35, INPUT_PULLUP)` is silently wrong.
- Buttons are **active high** → deep-sleep wake uses
  `esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH)`.
  The RTC alarm is **active low** → `ext0` with trigger level `0`.
- v1.5 puts the battery ADC on 35 and Up on 32. Do not copy v1.5 code.
- The PlatformIO `watchy` board defines `ARDUINO_WATCHY`, not
  `ARDUINO_WATCHY_V20`; the revision is pinned in `platformio.ini`.

## Commands

```bash
pio test -e native            # host unit tests — the merge gate
pio run -e watchy_v20         # build firmware
pio run -e watchy_v20 -t upload
pio run -e watchy_v20 -t size # check flash/RAM budget
pio device monitor            # exception decoder is enabled

python tools/make_time_font.py         # regenerate the Gilroy faces after a size change
python tools/preview_face.py --all      # rebuild the PNGs README.md shows
python tools/preview_face.py --battery 8 --time 9:05   # one ad-hoc render
```

`preview_face.py` reads the generated font header and the layout constants in
`board/display.cpp`, so it shows the framebuffer the panel would get. Look at a
layout change there before spending a flash cycle on it.

**A layout change is not finished until `--all` has been re-run in the same
commit.** `docs/preview/*.png` is checked in and README.md shows it; stale renders
advertise a watch that no longer exists. Ad-hoc renders go to `preview/`, which is
git-ignored.

`flash.bat` is the Windows wrapper around the upload line — same `pio`, plus port
selection and an optional host-gate-first run: `flash.bat test COM7 monitor`.

## Layout

```
src/core/     pure logic, host-tested, no hardware headers
src/board/    Watchy 2.0 hardware access — thin, no policy
src/app/      screens and rendering
test/         one directory per core module, Unity
tools/        font generation and the desktop preview; fonts/ holds the TTF
docs/         hardware notes, power budget, review checklist, workflow
docs/preview/ checked-in renders of every screen, rebuilt with --all
```
