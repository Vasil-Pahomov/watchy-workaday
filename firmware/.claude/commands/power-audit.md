---
description: Whole-firmware energy audit — sleep paths, peripheral leaks, wake budget. No code changes.
---

Spawn the `reviewer` subagent for an **energy-only** audit of the entire
firmware, not just the current diff. Tell it to skip P2/P3 and go deep on P1,
plus P0 sleep-path defects.

Have it work through, with `file:line` for every claim:

1. **Sleep-path completeness.** Trace every path out of `setup()`. Enumerate any
   that can return, early-return, or throw without reaching
   `board::power::deepSleep()`.
2. **Peripheral balance.** For each of I2C, SPI, ADC, display panel, vibration
   motor, radio: every power-up site, and whether *every* exit path from that
   scope powers it down. Error paths and early returns specifically.
3. **Wake inventory.** Every wake source, its frequency, and its measured or
   estimated cost. Cross-check against `docs/power-budget.md` and report drift
   between the documented budget and what the code actually does.
4. **Display cost.** Refresh decisions: unnecessary refreshes, full-where-partial
   would do, missing `hibernate()`.
5. **Busy-waiting.** Every `delay()`, spin loop, and untimed poll.
6. **Sleep-current hazards.** GPIOs left floating or driving dead peripherals;
   pins not quiesced before sleep; wake sources left enabled that should not be.
7. **CPU time at wake.** Work that could be skipped or deferred; float→double
   promotion; anything done eagerly "in case".

Ask it to close with the top three changes by expected battery-life gain,
ordered by impact, each with a rough estimate and the reasoning behind it.

Report the findings to me. Do not fix anything.
