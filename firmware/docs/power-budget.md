# Power budget

Battery: **200 mAh** LiPo — the marked rating, which is not a verified one. Cells
shipped with AliExpress Watchy 2.0 boards are widely reported to fall short of
what is printed on them, and this watch's has never been checked. Everything below
is therefore *at 200 mAh*, not *on this watch*. Target: **≥ 21 days** on a charge
in normal mode.

That gives a daily allowance of **≤ 9.5 mAh/day**. Every feature is measured
against that allowance. This document is the shared ledger — a change that adds a
wake source or a peripheral must update it, and the reviewer checks for drift
between this file and the code.

## Where the allowance goes

The numbers below are **engineering estimates, not measurements.** They are the
right order of magnitude and good enough to design against; they must be replaced
with meter readings (see `docs/hardware-v2.0.md`). Marked ⚠ until measured.

### Sleep floor ⚠

| Contributor | Estimate |
|---|---|
| ESP32 deep sleep, RTC + RTC RAM retained | ~10 µA |
| PCF8563 RTC | ~0.3 µA |
| BMA423 counting steps, low-power mode | ~14 µA |
| LDO quiescent + leakage | ~40 µA |
| **Total sleep** | **~65 µA → ~1.6 mAh/day** |

The LDO quiescent current dominates and **no amount of firmware work reduces
it.** This is the floor. Roughly 15 % of the daily allowance is spent before we
execute a single instruction.

### Minute tick ⚠

One wake per minute = **1440 wakes/day**. Per wake:

| Phase | Estimate |
|---|---|
| Boot from deep sleep to `setup()` | ~40 ms |
| Read RTC over I2C | ~3 ms |
| Render into the framebuffer | ~10 ms |
| E-paper **partial** refresh | ~300 ms @ ~15 mA |
| Quiesce + enter sleep | ~5 ms |
| **≈ per wake** | **~360 ms, ~0.0030 mAh** |

**1440 × 0.0030 ≈ 4.3 mAh/day.** Together with sleep: ~5.7 mAh/day → **~35 days**
theoretical. Real-world lands lower; the 21-day target carries the margin.

### Full refresh ⚠

~2 s at ~20 mA ≈ 0.011 mAh — about **3.7 minute-ticks each**. Cheap individually,
so ghosting cleanup is affordable; ruinous if triggered per minute. Budget: at
most one every 60 partials or 12 hours, whichever comes first.

**Inverting the display costs one of these, on the press that does it.** The
theme menu item swaps ink and paper, so every one of the 200×200 pixels
transitions in the same frame — the extreme case of the residue the cadence above
exists to clear, and the one frame where a partial refresh would leave the whole
previous screen behind. `core::ThemeChange::changed` is true only on the press
that flipped it, and `main.cpp` passes it as `force_full`.

**The counterfactual is a partial refresh, not a skipped one**, and getting that
right is the difference between the marginal cost and the whole cost. `app/screens.cpp`
folds the theme into the content hash unconditionally, so the flip changes the
hash on every screen: without `force_full` that press would still have repainted,
as a **partial** — the ~0.0030 mAh minute-tick above. Forcing it full makes it
3.7 of those instead of 1, so the marginal cost of the ghosting decision is
**~+0.008 mAh per toggle**. The ledger row below prices the whole press at
0.011 mAh, which is right; ~0.0030 of that is a repaint this press was going to
pay anyway, and only ~0.008 is the choice of full over partial.

It adds no wake, no peripheral and nothing to the sleep floor, and the theme
itself is free to keep — it is one bit in the persisted block and one pair of
colour constants at draw time. Ten toggles in a day would be ~0.08 mAh, under 1 %
of the allowance; the realistic number is a handful in the life of the watch. Not
carried as a recurring cost for that reason — the ledger row below prices one
event, and the frequency column says what it is.

### Radio — the thing that breaks the budget

A single WiFi association plus an NTP exchange is ~3 s at ~120 mA ≈ **0.1 mAh** —
roughly **33 minute-ticks**, from one sync. Hence Law 1: the radio is never
enabled on a routine wake. A daily NTP sync costs ~1 % of the allowance and is
affordable; a *five-minute* poll is 28 mAh/day and empties the battery in a week.

WiFi is not what the watch actually uses. The clock is set over **BLE from the
phone** (`PROTOCOL.md`), which is an order of magnitude cheaper — and the numbers
below are the reason the design is shaped the way it is.

#### The BLE sync window ⚠

One window an hour, **24/day**. It **extends a minute tick the watch was already
taking**; it does not add a wake of its own. Per window (`PROTOCOL.md` §5.3):

| Phase | Estimate |
|---|---|
| Stack init + teardown | ~0.25 s @ ~40 mA ≈ 0.003 mAh |
| No phone in range: 6 s advertising | ~11 mA ≈ 0.018 mAh |
| Phone present: ~1 s connect + ~1.5 s exchange | ~40 mA ≈ 0.028 mAh |
| **Worst ordinary case per window** | **~0.031 mAh** |

**These figures were aspirational until the §4 completion rule landed, and are now
real.** Measured on hardware with a paired phone: a *successful* exchange did not
end the window. `runSyncWindow()` looped on any `TimeWritten`, but §4 licenses a
second push in the same connection **only if the first one failed**, so a phone
that kept pushing after `result=0` held the radio open to the 12 s cap — twelve
writes, ~780 ms apart, every one succeeding, twelve RTC commits and twelve I2C
sessions where §4 asked for one. That window cost roughly **0.13 mAh**, about 4×
the row above, and the row was not conservative, it was wrong.

`core::SyncWindow::noteWriteResult()` now ends the window at the first thing that
happens after a successful write. On the trace above that is the phone's next push
at ~4.1 s of window time instead of 12.0 s — **~7.9 s of radio saved on a window
that worked**, and one RTC write instead of twelve. The row is conservative again:
a successful window now usually ends sooner than the 6 s no-phone case, because
the phone hanging up (§6.1's normal ending) arrives well inside it.

The ceiling below is unchanged in value but no longer reachable the same way. A
peer can still hold the window to the cap, but only by **failing** repeatedly —
which is the retry §4 licenses and the loop exists to serve. Repeated *successes*
can no longer do it.

The exchange figure includes the ~100–300 ms the watch spends letting the phone
hang up after the Status notification (`PROTOCOL.md` §6.1). That wait is not
optional — a notification is fire-and-forget, so tearing the radio down straight
after sending one loses it, and §4 makes that notification the only thing that
resets the phone's backoff. It is inside the 1.5 s already budgeted.

The row above is the worst *ordinary* case, and it is what the ledger carries.
The ceiling is higher: a peer that connects and then keeps the watch busy until
§5.1's absolute session cap expires costs **~0.14 mAh**, a bounded **~4.5×** on one
window an hour. The 12 s cap is not the whole of that figure — §5.1 measures it
"from session open to teardown", and `board::ble` stamps `opened_ms_` *after*
`startAdvertising()` returns, so the ~0.2 s of NimBLE init sits before the cap
starts. 12 s + ~0.2 s at ~40 mA is ~0.14 mAh.

**The teardown tail is gone, and with it the 2.05 s that used to sit after the
cap.** `~Session()` no longer calls `NimBLEDevice::deinit()`: that call panics core
0 on every window (`ble_hs_stop()` disables the host, after which
`ble_hs_timer_reset()` frees the host timer while GAP is still scheduling it, and
the FreeRTOS timer daemon then services a queued command against the freed block).
The destructor stops advertising, which is what actually ends the transmitting, and
`esp_deep_sleep_start()` drops the RF domain. The old ~2.05 s NimBLE host-stop wait
no longer happens, so both the energy ceiling above and §5.1's un-fed watchdog
interval improve — `PROTOCOL.md` §5.1's teardown-tail row is now conservative
rather than wrong, and should be re-derived the next time that document is opened.

**What it costs, and it belongs here as much as in `board/ble.h`: the BLE
controller stays enabled between the destructor returning and `deepSleep()` being
reached.** Today that is microseconds and the cost is nil. It is only nil because
Law 1 makes `deepSleep()` the terminus of every path — a change that opens a window
and then does anything before sleeping (a confirmation screen, a retry, a second
window) holds the controller up for that whole stretch at tens of milliamps against
a ~20 uA floor. Neither the compiler nor a host test can hold that ordering; the
`deepSleepIsATerminus()` probe in `main.cpp` holds the half that keeps `deepSleep()`
a terminus, and the interval between `ble: teardown 4/4` and `sleep:` in a
`WORKADAY_DIAG` log is what shows the other half.

That ceiling cannot compound, for the same reason the ordinary case is
affordable: the hour is recorded when a window **opens**, so a pathological
window costs its hour and nothing more. It is deliberately **not** budgeted.
Doing so would assume an adversarial peer in range every hour of every day, and
24 of them would put this row at ~3.8 mAh/day and the daily total at ~9.8 — past
the 9.5 mAh/day allowance outright, for a case no wearer has. The common case,
with nobody in range, is *cheaper* than the row above.

**24 × 0.031 ≈ 0.75 mAh/day**, carried in the ledger as **~0.9 mAh/day** with
margin — about **9 %** of the allowance, and roughly **10 minute-ticks per
window**. It is the single most expensive feature in the firmware and it is still
affordable, because of three limits that are each load-bearing:

1. **The schedule is spent when a window OPENS, not when one succeeds.**
   Reset-on-success sounds helpful and is the defect: a watch whose phone is out
   of range never succeeds, so it would advertise on every wake — 1440 windows a
   day, **~26 mAh/day**, a flat cell in about a week, with no upper bound
   anywhere. `core::sync_policy` owns this and `main.cpp` calls
   `noteSyncWindowOpened()` *before* the radio comes up, so a window that dies to
   a watchdog reset still costs the full hour.
2. **One window per hour, and the hour is the clock's.** The window opens on the
   wake at which the wall clock's hour differs from the hour the last one opened
   in (`PROTOCOL.md` §5.1), so the count is 24 a day by construction rather than
   by a timer that happens to run for 60 minutes. The old free-running form cost
   exactly the same; what it could not do was tell anybody *when*.
3. **The window is bounded in hard time, not by a peer.** 6 s advertising with
   nobody there, 4 s connected but idle, 12 s absolute cap — `PROTOCOL.md` §5.1.
   A phone that connects and then stops talking costs 4 s, not a wake.
4. **It never opens in a degraded mode, never on a low battery unless the wearer
   asked, and never on an accelerometer or unclassified wake.** The gates are in
   `core::sync_policy`, where a host test can reach them. Note the clause in the
   middle: below `kLowEnterPercent` the watch stops reaching for the phone *on its
   own*, which is what removes this ~0.9 mAh/day row from a nearly flat cell. A
   Sync or Find phone press still works, and is priced with the presses below
   rather than in the daily ledger — the wearer is standing there, and ~0.03 mAh
   is not what is going to finish that cell.

For scale: continuous advertising is ~20 mAh/day and empties the cell in about
ten days. That is what the window exists to avoid.

The user-initiated window (the Sync menu item) costs the same ~0.031 mAh of radio
and **records the same hour**, so a press cannot multiply the scheduled windows:
whatever the wearer does, the schedule still opens at most one window per hour.
What a press does add is its own window each time it is made — §5.1's "opens a
window immediately" — so the honest ceiling is 24 scheduled plus one per press.

Two things it now also costs, both on the press and neither in the daily ledger:
**three or four partial panel refreshes** (~0.003 mAh each) to narrate the window
on the Sync screen — searching, connected, then how it ended — and, at a low
battery, the window itself, which the schedule would have refused. Call the press
~0.045 mAh all in, about four minute-ticks. The redraws are free on the 24
scheduled windows: they compose a frame whose Sync line is not on screen, so
`core::refresh_policy` hashes it to the frame already showing and skips.

#### The find-phone session ⚠

The one thing in this firmware that stays awake for minutes rather than
milliseconds, and the most expensive single press the wearer can make
(`PROTOCOL.md` §4.1, §5.3). It is user-initiated, never scheduled, bounded by a
120 s cap, and refused in Safe or Recovery mode by the same gate a user-requested
sync window is — and it records the hour when it opens, since a phone that turns
up performs the sync exchange on the way.

A low battery does **not** refuse it, and that is a deliberate reversal: the
battery gate stops the *schedule*, and a wearer looking for their phone is not the
schedule. The worst case below is ~14 % of a day's allowance on a cell that may
have little left — spent because the wearer asked for it, at the one moment the
feature exists for.

| Case | Estimate |
|---|---|
| Nobody connects: 120 s advertising @ ~11 mA, plus ~24 partial redraws @ 0.003 mAh | ~0.4 mAh |
| Phone found at once, search left to run out: 120 s of live link, CPU awake, @ ~40 mA | ~1.3 mAh |
| The realistic search — found and stopped inside 20–30 s | ~0.1–0.3 mAh |
| The radio idle between the session closing and the final frame (~0.3 s) | ~0.003 mAh, inside the rows above |

The worst case is ~14 % of a day's allowance, in one press. It is **not carried
as a recurring cost** in the ledger, for the reason the pathological sync window
is not: budgeting it would assume a wearer who loses their phone every day and
never presses Back, and the realistic search costs about what ten minute-ticks
do. What keeps it from compounding is the same pair of limits the sync window
has — the cap is absolute, and the hour is recorded on opening — plus one
of its own: a search does not re-run on the next wake, because the request lives
on the wake that made it (`core::SyncContext::user_requested`) and the outcome
the screen shows afterwards is a message, not a queued retry.

Where the ~40 mA comes from: the ESP32 with the BLE controller enabled and the CPU
awake in a FreeRTOS wait. The Arduino framework does not light-sleep the CPU
between events, so a connected-and-waiting watch draws roughly what an
exchanging one does. Un-metered, like every figure in this file.

The redraws are partial refreshes and count against the ghosting cadence in
`core::refresh_policy`; a full-length search spends about a third of the
60-partials budget, and the next full refresh arrives that much sooner. Priced
above, and small.

## First field run — what it settles, and what it cannot

One watch ran about a week on a charge and came back with roughly a third of the
gauge showing. That settles one thing, and settles it firmly: **deep sleep is
being entered.** A watch that stayed awake would be flat inside a working day, so
a week clears that bar by a margin no measurement error comes near.

It does **not** yield a consumption figure, and the attempt is a trap worth naming
so nobody walks into it twice. Runtime becomes mAh/day only by dividing into a
known capacity, and this capacity is not known — see the rating caveat at the top
of this file. At 200 mAh that week reads as ~19 mAh/day; at half that rating it
reads as ~9.5, which is exactly the allowance. One observation, two conclusions
that differ by 3×, and nothing in the observation to choose between them. It
cannot tell a firmware that overspends from a cell that under-delivers.

That is an argument *for* item 3 rather than a substitute for it: **a meter on the
rail does not care what the cell holds.** Current is measurable without knowing
capacity; runtime is not. Detailed consumption analysis waits for that
measurement, and for a cell whose real capacity has been established.

## Design consequences

These follow from the numbers, and are why the code looks the way it does:

1. **Wake duration is the lever, not wake count** — at minute resolution the wake
   count is fixed by the product. Shaving 100 ms off a wake saves ~1.2 mAh/day,
   which is more than the entire sleep floor.
2. **The panel refresh is ~85 % of an average wake.** Optimising CPU-side
   rendering is close to pointless; avoiding a refresh entirely is what pays.
3. **Skip the refresh when content is unchanged.** No gain for a plain HH:MM face
   (it changes every minute) but a large one for any screen that does not, and it
   makes button wakes nearly free. `core::refresh_policy` owns this decision.
4. **Low-battery mode drops to 5-minute resolution** — 288 wakes/day instead of
   1440, cutting tick cost to ~0.9 mAh/day and roughly tripling the remaining
   runtime. Owned by `core::battery_model` + `core::wake_router`, and entered at
   `kLowEnterPercent` (10 %, ~3.60 V), left at 15 %. Late on purpose: at the old
   20/28 the watch spent hours on a five-minute face with a third of its useful
   charge still in it, where 10 % is the knee of the curve and the point at which
   tripling what is left is worth a stale clock.

   It is the one saving the wearer can feel — the clock is up to five minutes
   behind, and the watch stops meeting the phone on its own — so
   `core::batterySaving()` puts a `!` beside the gauge to say so. That costs
   nothing per wake: the mark is drawn inside refreshes that were already
   happening, and the hysteresis means crossing the threshold, which is the only
   thing that can force a refresh of its own, happens about twice per discharge.

   Two things it does **not** cost. Button presses still land at once — they are
   an `ext1` wake, not a tick, and the router gives them the panel on the wake
   they arrive on. And the Sync and Find phone items still work: §5.1's battery
   gate stops the schedule, not the radio.

   The stretched tick is also **aligned** — `core::alignedTickMinutes()` picks the
   PCF8563 count so the wake lands on a wall-clock minute divisible by five, which
   is why the face reads 14:35 and never 14:33. It costs nothing: the count is one
   subtraction on a register write the wake was making anyway. It also keeps a
   tick at the top of every hour, which is where §5.1's sync window is due.
5. **The battery ADC is sampled on a schedule, not per wake.** The reading is
   slow-moving; sampling it every minute buys nothing.
6. **Step counting adds sensor current, not wakes.** The BMA423 accumulates steps
   in its own feature engine, so the total is collected during the minute tick we
   already take, on the I2C session we already open. That is the difference
   between +0.34 mAh/day and a feature that would have been unaffordable: waking
   the CPU to sample acceleration at 25–50 Hz is ~36 000 wakes an hour and is not
   remotely within budget.

   The corollary is that the **accelerometer interrupt stays disarmed**
   (`kAccelWakeEnabled` in `main.cpp`). It is needed only for genuinely
   motion-triggered features — wake-on-wrist-turn, tap — and each of those must
   justify both the wake rate and its share of the sleep floor on its own.

7. **The config blob is uploaded once, not per wake — and the "once" is
   enforced, not assumed.** Bosch's ~6 KB feature-engine stream is 192 chunks over
   I2C at 100 kHz plus a 150 ms delay inside the vendor driver and a 20 ms one
   after its soft reset: **~0.85 s**, against a wake budgeted at ~360 ms.
   `board::accel::probe()` costs three register reads and reports what it sees;
   `core::accel_policy` decides whether the expensive path is worth taking.

   The arithmetic that makes this a hard limit rather than an expectation: a
   sensor stuck in "needs configuring" would upload on every tick, and
   1440 × 0.85 s ≈ 20 minutes a day at ~30 mA ≈ **+10 mAh/day** — more than the
   entire 9.5 mAh/day allowance, taking the watch from 21+ days to about a week.
   If the upload instead wedges the bus, each transaction costs the 50 ms I2C
   timeout, the wake overruns the 10 s task watchdog and never reaches sleep, and
   the cost is ~10 s at full current per reset.

   So `core::kMaxAccelConfigAttempts` caps it at **3 attempts per power cycle**,
   counted in RTC-backed state *before* each attempt runs — a counter incremented
   afterwards would be lost to exactly the watchdog reset it needs to survive.

   That only became true when `AccelState` moved with the rest of `PersistedState`
   into `RTC_NOINIT_ATTR`. Under `RTC_DATA_ATTR` the block sat in `.rtc.data`,
   which the bootloader reloads from flash on any boot that is not a deep-sleep
   wake — so the counter *was* lost to the watchdog reset it was written to
   survive, and a sensor wedging the bus would have retried three uploads, reset,
   and started again from zero for ever. Counting before the attempt is necessary
   but was not sufficient; the storage had to hold as well. See
   `docs/architecture.md`, "State across sleep".
   After that the sensor is left alone and the face shows "no step data", which is
   the correct trade: a dead step counter costs a feature, an unbounded retry
   costs the battery. Recovery needs the RTC block cleared, which on v2.0 means
   opening the case — see `docs/backlog.md` item 13 for the bounded daily retry
   that would avoid that, at 0.007 mAh/day.

   Giving up also **suspends the sensor** (`board::accel::suspend()` clears
   `acc_en`). A BMA423 that is running but not counting would otherwise keep its
   ~14 µA — ~0.34 mAh/day, a fifth of the sleep floor — for a feature that has
   been switched off. One read-modify-write, and because that write can NACK its
   result is checked: a park that did not happen is retried on a later wake, up to
   `core::kMaxAccelSuspendAttempts` (3). Bounded for the same reason as everything
   else here — two transactions per wake for ever is ~1 mAh/day on a wedged bus,
   which is 10 % of the allowance.

8. **The radio's cost is set by how often the window opens, not by whether anyone
   answers.** A window that reaches nobody is ~0.018 mAh and a successful one is
   ~0.028 mAh — within a factor of two. So there is nothing to gain by trying
   harder when a sync fails, and everything to lose: the whole spread between
   0.9 mAh/day and 26 mAh/day is the *number of windows*, which is why the hour
   is recorded on opening and never refunded. This is the same shape as the
   BMA423 config cap in item 7 — a cheap operation made ruinous by repetition.

## Ledger

Update this table in the same change that adds or alters a wake source.

| Wake source | Frequency | Cost/event ⚠ | mAh/day ⚠ | Notes |
|---|---|---|---|---|
| Sleep floor (excl. BMA423) | continuous | — | ~1.2 | irreducible |
| BMA423 counting steps | continuous | — | ~0.34 | sensor quiescent only, no wakes |
| RTC minute alarm | 1440/day | 0.0030 | ~4.3 | 288/day in low-battery mode |
| Step read on the tick | 1440/day | ~0 | ~0.02 | 4 probe/read bytes on an open bus; also on timer and unknown wakes |
| Full refresh | ≤ 2/day | 0.011 | ~0.02 | ghosting cleanup; see the fault-path note below |
| Button press | ~50/day | 0.0015 | ~0.08 | no refresh if content unchanged |
| Display invert | rare, user-initiated | 0.011 | ~0 | a button press that forces a full refresh; no wake of its own |
| Accelerometer INT | disarmed | — | 0 | not needed for step counting |
| BMA423 config upload | ≤ 3 per power cycle | ~0.007 | ~0 | ~0.02 mAh once, then never; capped by `core::kMaxAccelConfigAttempts` |
| BMA423 park after give-up | ≤ 3 per power cycle | ~0 | 0 | 2 register transactions; reclaims the sensor's ~0.34 mAh/day |
| BLE sync window | 24/day | 0.031 | ~0.9 | extends an existing tick, adds no wake; hour-boundary gate in `core::sync_policy`, hard 6/4/12 s timeouts |
| Find-phone session | rare, user-initiated | 0.1–1.3 | ~0 | `PROTOCOL.md` §4.1; up to 120 s awake at radio current, 5 s redraw cadence; same gates as the sync window and spends its hour; not carried as recurring — see the section above |
| Radio — WiFi/NTP | none | 0.1 | 0 | not built; the clock comes from the phone over BLE |
| **Total** | | | **~6.9** | allowance 9.5 → **~27 % headroom** |

**A fault path can spend the full-refresh budget, and one did.** `core::wake_router`
sets `force_full_refresh` on any non-deep-sleep reset (`wake_router.cpp:94-98`), so
anything that reboots the watch on a schedule also drives a full refresh on that
schedule. The core-0 panic in `NimBLEDevice::deinit()` did exactly that: a panic per
sync window, roughly **24 forced full refreshes a day against the ≤ 2 budgeted
above** — about 0.26 mAh/day, small against the 9.5 mAh/day allowance, but 12× a
line item whose whole purpose is to ration ghosting cleanup.

**Resolved by removing the `deinit()` call**, not by anything in this table; the
reboots stopped and the drift went with them. The observation stays because the
next fault path to cause hourly reboots will spend the same budget the same way,
and the ledger is where that should be noticed.

## How to measure

Replace the ⚠ estimates with real numbers:

- **Sleep current** needs a µA-capable meter in series with the battery — a
  multimeter's mA range cannot resolve 60 µA. A current-sense amp or a
  purpose-built meter (PPK2, Joulescope) is the practical route.
- **Wake duration** does not need special equipment: toggle a spare GPIO high at
  the top of `setup()` and low immediately before `esp_deep_sleep_start()`, then
  scope it. Cheap, and it directly measures the number that matters most.
- **Charge per wake** is the integral of current over that window; a
  current-sensing tool that reports charge gives it directly.
- **The BLE window** needs both cases measured separately, because they are
  budgeted separately: a window with no phone in range (the common one — it is
  what every hour costs when the wearer is away from their phone) and a window
  with a phone that connects and writes. Both are visible on the same trace as
  the tick they extend, so the figure to record is the *difference* between a
  tick with a window and one without. `WORKADAY_DIAG=1` prints when a window
  opens and what it achieved, which is enough to line the trace up.

Record the date and firmware revision alongside any measurement added here.
