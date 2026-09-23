# Review checklist

The rubric the `reviewer` agent works from, and the list the `coder` agent should
self-check against before declaring a change finished. Priorities are ordered by
how badly the defect hurts a watch on someone's wrist.

## P0 — hangs and lockups

The failure we refuse to ship: a frozen watch needing a manual reset.

- [ ] Every path out of `setup()` reaches `board::power::deepSleep()`. Trace the
      early returns and the error paths, not just the happy path.
- [ ] Task watchdog armed early in `setup()`.
- [ ] The watchdog is **not** fed inside a wait loop — that defeats it entirely.
      The BLE window is where this is hardest: its feeds must be the three genuine
      progress points `PROTOCOL.md` §5.1 names (the stack up and the controller
      synced, a central connected, a Time write processed) and no others, and they
      must happen on the task the watchdog is subscribed to rather than in a radio
      callback. Deleting the first of the three is not a tidy-up — it puts
      NimBLE's stack init back inside the 6 s advertising interval, which is what
      stops 6 s being 6 s. The find-phone session (`runFindSession()`) is the same
      rule over two minutes: every wait `core::find_session` hands out is under
      that bound, and the feed comes *after* the redraw or the processed event
      that ended the wait — never on `StillWaiting`. A change that feeds inside
      `findWait()`, or lengthens `kFindRoundMs` past `kAdvertiseTimeoutMs`, has
      made the watchdog blind for the one path that stays awake long enough to
      need it.
- [ ] Every blocking hardware call has a timeout: I2C transaction, e-paper BUSY
      wait, radio connect, any `while` on a hardware flag.
- [ ] Every timeout is *handled* — degrade and sleep. Not an assert, not a retry
      forever.
- [ ] No unbounded retry loops, no recursion, no unbounded allocation.
- [ ] **Anything latched on the way into sleep is released on the way out.** For
      every `*_hold_en()`, `*_force_*()`, `*_enable_*()` or one-way register write
      on the sleep path, ask the two questions: **what releases this, and when?**
      An answer of "the reset does" is wrong for anything living in the RTC power
      domain — a core reset, a watchdog reset and a brownout all leave it set, and
      only a power-on reset clears it. This board has no power switch, so a latch
      with no release is not a bug that costs a wake, it is a **P0 that costs
      opening the case**: `gpio_deep_sleep_hold_en()` shipped without its
      counterpart and froze U0TXD, which took serial output and the ROM download
      loader with it. Two specifics worth checking by name, because both have
      already caught someone out: the supported "disable" call may not release a
      latch that is already applied (on ESP32 `gpio_deep_sleep_hold_dis()` clears
      the enable bit and nothing else), and a pin being on a never-touch list
      protects it from a GPIO sweep but **not** from a hold.
- [ ] A panic reboots. It does not halt.
- [ ] State that must outlive a **fault** is in `RTC_NOINIT_ATTR`, not
      `RTC_DATA_ATTR`. `.rtc.data` is a loadable segment the bootloader refills on
      every boot that is not a deep-sleep wake, so a panic or watchdog reset
      erases it — which is exactly the case fault counters, retry budgets and the
      sync window's schedule exist for. Check the attribute, and check that
      the struct has no user-declared constructor (a dynamic initialiser would
      overwrite noinit memory at boot and reintroduce the same defect).
- [ ] That state is version-tagged and validated before use. Noinit RTC RAM is
      not zeroed and survives reflashing — on the first boot of new firmware it
      contains the *previous* build's garbage.
- [ ] RTC time is gated through `core::time_model::isValid()` before use.
- [ ] The next alarm is always set to a reachable time. An alarm that never fires
      with no timer fallback is an unrecoverable freeze.
- [ ] `core::health` fault escalation is intact — a crash loop degrades into
      Safe/Recovery mode instead of looping forever.

## P1 — energy waste

Battery dies in days instead of weeks. See `docs/power-budget.md` for the cost of
each item.

- [ ] Every peripheral powered up is powered down on **every** exit path. Check
      early returns and error paths specifically — that is where the leak hides.
- [ ] RAII session guards used rather than manual on/off pairs.
- [ ] No `delay()`, no spin loop, no untimed poll in a production path.
- [ ] No WiFi/BLE on a routine wake. Radio use is user-initiated **or scheduled**
      (Law 1's wording), never incidental, and always has a hard timeout. For the
      BLE sync window that means: the gate is `core::sync_policy` and nothing
      bypasses or re-implements it; `noteSyncWindowOpened()` is called *before*
      the radio comes up, never on a success path; and the window is bounded by
      `PROTOCOL.md` §5.1's 6/4/12 s timeouts rather than by a peer. The find-phone
      session goes through the same `evaluateSyncWindow()` gate with
      `user_requested` set, spends the same hour, and is bounded by
      `kFindPhoneTimeoutMs` — a search that bypasses the battery gate, or one
      whose request outlives the wake it arrived on, is the unbounded radio use
      this law forbids wearing a new name.
- [ ] **Anything that stays awake past a panel refresh is priced.** Today that is
      one path — the find-phone session, up to two minutes at ~40 mA — and it is
      in the ledger with the reasons it is affordable. A second such path needs
      the same row and the same reasons.
- [ ] No display refresh when the rendered content is unchanged.
- [ ] Partial refresh preferred; full refresh only for ghosting cleanup, within
      the documented cap (≤ 1 per 60 partials or 12 h).
- [ ] `display.hibernate()` before sleep on every path, including the
      refresh-skipped path.
- [ ] Battery ADC sampled on its schedule, not every wake.
- [ ] New wake sources justified and recorded in the `docs/power-budget.md`
      ledger. Flag drift between that ledger and the code.
- [ ] GPIOs quiesced before sleep — nothing floating, nothing driving a
      powered-down peripheral.
- [ ] Accelerometer interrupt still opt-in (it costs ~25 % of the sleep floor).
- [ ] No work done eagerly "in case". The cheapest wake decides to do nothing.
- [ ] No `float` → `double` promotion (no double-precision FPU on this chip).
- [ ] CPU frequency not raised beyond what the wake needs.

## P2 — untested logic

- [ ] New decision logic lives in `src/core/`, not `src/board/`. Decision logic
      buried in `board/` is unreachable by tests and is the most common
      architectural defect here.
- [ ] `src/core/**` contains no `<Arduino.h>`, no `<esp_*.h>`, no `Wire`.
- [ ] Every new `core/` function has tests in the **same** change.
- [ ] Boundary cases covered, not just the happy path:
      second 0 and 59 · midnight · month rollover · year rollover · leap year ·
      0 % and 100 % battery · invalid/garbage RTC time · first-ever boot ·
      counter overflow and wrap · buffer exactly full · empty input.
- [ ] No test was loosened, skipped, or deleted to make the suite pass.
- [ ] `pio test -e native` passes — actual output, not an assumption.

## P3 — v2.0 correctness and hygiene

- [ ] Pin numbers match `docs/hardware-v2.0.md` / `board_v20.h`.
- [ ] No v1.5 values leaked in: Up is **35** (not 32), battery ADC is **34**
      (not 35).
- [ ] No `INPUT_PULLUP` on GPIO 34–39 — input-only, no internal pull resistors,
      silently does nothing.
- [ ] Buttons treated as **active HIGH**; sleep wake uses
      `ESP_EXT1_WAKEUP_ANY_HIGH`.
- [ ] RTC interrupt treated as active **LOW** on `ext0`, trigger level 0.
- [ ] Battery voltage includes the ×2.0 divider and uses
      `analogReadMilliVolts()`, not `analogRead()`.
- [ ] No hardware-revision `#if`s and no portability abstractions (Law 5).
- [ ] Dependencies and the platform version stay pinned (Law 4).
- [ ] `pio run -e watchy_v20` builds; flash/RAM headroom reported for sizeable
      changes.

## Verdict

**APPROVE** — no P0, no P1, `pio test -e native` green, build green.

**CHANGES REQUIRED** — anything else. State the count by priority.

Do not pad a review to look thorough: a clean change should be approved, and
inflated findings train the coder to ignore real ones. Equally, never soften a
P0.
