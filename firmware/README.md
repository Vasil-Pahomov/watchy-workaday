# Workaday — Watchy 2.0 firmware

Firmware for the **SQFMI Watchy v2.0** (ESP32-PICO-D4, 200×200 e-paper, PCF8563
RTC, BMA423 accelerometer, 200 mAh LiPo). Deliberately **not** portable to other
Watchy revisions.

## Getting started

Requires [PlatformIO](https://platformio.org/install) and VS Code with the
PlatformIO IDE extension. Open this folder; PlatformIO resolves the toolchain and
libraries on first build.

```bash
pio test -e native
```

```bash
pio run -e watchy_v20
```

```bash
pio run -e watchy_v20 -t upload
```

Host tests need only a C++17 compiler — no hardware, no board attached.

## What makes this firmware what it is

Five non-negotiable rules, spelled out in [CLAUDE.md](CLAUDE.md):

1. **Energy first.** Deep sleep is the control flow, not an afterthought. No
   `loop()`, no `delay()`, no spin loops. Peripherals off unless in use, enforced
   by RAII guards rather than discipline. Budget: [docs/power-budget.md](docs/power-budget.md).
2. **Never hang.** Watchdog armed on every wake, timeouts on every hardware call,
   a timer backstop behind the RTC alarm, and cross-boot fault escalation into
   Safe and Recovery modes so a crash loop degrades instead of flattening the
   battery in five hours.
3. **Logic is host-tested.** `src/core/**` is pure C++17 with no hardware headers,
   covered by 143 Unity tests running on the developer's machine.
4. **VS Code + PlatformIO out of the box.** Pinned platform, pinned dependencies.
5. **Watchy 2.0 only.** No revision `#if`s, no abstraction for hypothetical
   hardware.

## Layout

```
src/core/     pure logic — decisions, host-tested, no hardware headers
src/board/    Watchy 2.0 hardware access — effects, thin, no policy
src/app/      screen composition and rendering
test/         one Unity suite per core module
docs/         hardware reference, power budget, architecture, review checklist
.claude/      the two-agent development workflow
```

The `core`/`board` split is the load-bearing decision: **decisions live in
`core/`, effects in `board/`.** Because the expensive choices (power up the ADC?
refresh the panel? enter Safe mode?) are pure functions of state, they are all
unit-testable — no current probe required to answer "does a button wake spin up
the accelerometer?".

## Development workflow

Two subagents with deliberately asymmetric powers: a **coder** that writes code
and tests, and a **reviewer** that audits and **cannot edit**. Full rationale in
[docs/workflow.md](docs/workflow.md).

| Command | Does |
|---|---|
| `/feature <what to build>` | coder → reviewer → coder, until approved (max 3 rounds) |
| `/review [files]` | Audit only, no fixes |
| `/power-audit` | Whole-firmware energy audit |

## Hardware notes

[docs/hardware-v2.0.md](docs/hardware-v2.0.md) has the verified pin map and the
traps. The ones that bite hardest:

- Buttons are **active HIGH**; sleep wake is `ESP_EXT1_WAKEUP_ANY_HIGH`.
- The RTC interrupt is **active LOW** on `ext0`, trigger level 0.
- **GPIO 34 and 35 have no internal pull resistors** — and v2.0 puts the battery
  ADC on 34 and the Up button on 35. `INPUT_PULLUP` there silently does nothing.
- v1.5 uses different pins for both (Up on 32, ADC on 35). Do not copy v1.5 code.
- The PlatformIO `watchy` board defines `ARDUINO_WATCHY`, not
  `ARDUINO_WATCHY_V20`; the revision is pinned in `platformio.ini` and
  `board_v20.h` `#error`s without it.

## Status

Runs end to end: wake → classify → plan → read RTC → read steps → decide refresh →
draw → sleep, with health escalation and the timer backstop in place. Daily step
counting is implemented against the BMA423's hardware counter and costs no extra
wakes. See [docs/backlog.md](docs/backlog.md) for what is not built yet — **setting
the time** is the blocking gap, followed by vibration and NTP sync.

**Not yet validated on hardware.** Every power figure in the budget is an
engineering estimate until measured; see the measurement notes in
[docs/power-budget.md](docs/power-budget.md).
