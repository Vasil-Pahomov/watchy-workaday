# First contact — bringing the watch and the phone up together

Written 18 Aug 2026, when the time-sync feature was finished and **neither side had
ever run.** The firmware has never been flashed to a watch; the app has never been
installed on a phone; no BLE packet has ever been exchanged between them.

Both sides are green on their own gates — 308 host tests in `firmware/`, 219 JVM
tests in `app/` — and that is exactly why this document exists. Every test on both
sides runs against a fake. The two implementations were tested separately and
agree only because they were built against the same `PROTOCOL.md`.

## Do the stages in order, and do not skip ahead

The temptation is to flash the watch, install the app, and see if the clock sets.
Don't. That test has about fifteen candidate causes when it fails, spread across
two codebases, a radio, and an OEM battery manager — and its failure mode is
silence.

Each stage below adds **one** new thing that has never worked before.

---

## Stage 0 — the watch alone, no radio

This is `firmware/docs/backlog.md` item 2 and it is unchanged by the sync work.
Nothing below matters until this holds.

It boots · the panel draws · the RTC ticks once a minute · buttons wake it · deep
sleep is actually entered.

**The trap named in the backlog is still the first thing to check:** if the watch
updates every 90 s instead of 60 s, the PCF8563 countdown timer is not firing and
the ESP32 timer backstop is masking it. `ext0` is not working. Everything about
the sync window's hourly cadence rides on that tick — §5.1 puts the window on the
top of the hour, and it can only ride a wake that happens there.

Then fill in the ⚠ estimates in `docs/power-budget.md`. Until one real
measurement exists, every energy number on both sides is an engineering estimate —
including the ~0.9 mAh/day this feature was budgeted at.

---

## Stage 1 — the watch advertises, with no app involved

Use a generic BLE scanner (nRF Connect or similar) on any phone. **Do not install
the companion app yet.** This checks `PROTOCOL.md` §2.2 with none of our Android
code in the picture, which means a failure here is unambiguously the firmware's.

Press Menu → Sync on the watch and look for:

| Check | Why it matters |
|---|---|
| A connectable advertisement appears at all | The whole feature. If nothing appears, `NimBLEDevice::init()` failed or the window never opened |
| The **service UUID** `57444159-6461-4779-b0a3-1f4c7e25d908` is in the **advertisement**, not the scan response | CDM's filter and the phone's `autoConnect` both read it from the AD. In the scan response it would be invisible to them. §2.2 |
| The name `Workaday` appears (scan response) | Diagnostic only |
| It stops after ~6 s | The advertise timeout. If it never stops, the window is not bounded and the watchdog is about to become load-bearing |
| **The MAC is identical after a reboot** | §2.2. A changing address silently breaks CDM association *and* the pending `autoConnect` — and the symptom is "it worked once, then never again" |
| The watch is still alive afterwards | If it resets, the un-fed watchdog interval overran. §5.1's margin is ~1.95 s and has never been measured |

Then let it sit for an hour and confirm the window opens **once**, on its own,
without a button press — and, once the clock has been set, **on the minute the
hour turns**. That is `core::sync_policy`'s hour boundary doing its job.

Two distinct failures to tell apart here:

- **It advertises on every minute tick.** The elapsed-minutes counter is not
  surviving — check `RTC_NOINIT_ATTR` is intact and that nothing gave
  `PersistedState` a user-declared constructor (see below).
- **It opens once an hour but at some arbitrary minute, and keeps drifting.** The
  boundary is not being reached: the watch is running on §5.1's fallback, which
  means `board::rtc::read()` is not returning `valid`. Expected on a watch whose
  clock has never been set, and a bug on one whose face shows the right time.

**A reset loop here is the expensive failure.** ~10 s at radio current every ~70 s
is ~130 mAh/day against a 200 mAh cell — flat in under two days. If the watch is
resetting, stop and read `WORKADAY_DIAG=1` output before continuing.

---

## Stage 2 — the app alone, with no watch

Install, open, grant Nearby devices and notifications, and confirm before pairing:

- the foreground service notification is present and stays present;
- reboot the phone → the notification comes back **without opening the app**;
- **do not test recovery with `force-stop`** — that check was in an earlier draft
  of this document and it is wrong. Verified on the S20 FE: `am force-stop` sets
  the package's `stopped=true` flag, which cancels its scheduled work and blocks
  every broadcast, and Android intends that to hold until the user launches the
  app again. Nothing recovers from it and nothing is supposed to. The WorkManager
  watchdog exists for **process death** — a crash, an OOM kill, an OEM task-killer
  that kills the process without setting that flag — which is a different thing
  that looks identical from the outside. To test the watchdog, kill the process
  (`adb shell am kill`, or let it crash), not the package;
- the diagnostic screen's task list names what is still outstanding.

Then work the battery-optimisation exemption and whatever the per-vendor advice
says for this phone. `app/docs/background-execution.md` §2 is blunt about this:
OEM battery managers are the real adversary, no API detects them, and the app can
only tell the user what to do and show whether it looks like it worked.

---

## Stage 3 — pairing, and the one place a design risk is waiting

Open the app, tap Pair, and press Menu → Sync on the watch while the system's
device picker is up.

**This is the step most likely to disappoint, and it is a design risk rather than
a bug.** CDM's scan runs for up to 60 s, but the watch only advertises for **6 s**
per window. So the user has to be holding the watch, in the dialog, and press Sync
inside that minute — and if they press it a moment too early, the 6 s expires
before the picker is listening.

If pairing is fiddly in practice, the fix belongs in the firmware and is cheap: a
longer advertising window **for the user-initiated case only**, which is
`kAdvertiseTimeoutMs`'s sibling rather than `kAdvertiseTimeoutMs` itself. Do not
simply raise `kAdvertiseTimeoutMs` — `PROTOCOL.md` §5.1's table shows that spends
the ~1.95 s watchdog margin, and the race case walks into a reset. A separate
constant for the user-initiated window is the honest change, and it costs nothing
on the scheduled path.

Also unverified here: that a `BluetoothLeDeviceFilter` carrying only a 128-bit
service-UUID `ScanFilter` matches this advertisement at all, and which
`EXTRA_DEVICE` the result actually carries (the code accepts `ScanResult`,
`BluetoothDevice` and `AssociationInfo` defensively, and refuses anything it
cannot normalise — so a mismatch shows up as "pairing failed" with no diagnosis).

After a successful pairing, reboot the phone and confirm the app re-arms from the
surviving association **without asking to pair again**.

---

## Stage 4 — the exchange

Set the watch's clock deliberately wrong, then wait for a window (or press Sync).

Expect: connect → notifications enabled → 12-byte Time write → RTC written →
12-byte Status notify with `result == 0` → the phone disconnects → the watch
sleeps. The diagnostic screen should show a recent sync and "Synced".

**If the exchange fails, it is not the byte layout.** Both sides reproduce
`PROTOCOL.md` §7's golden vectors byte for byte in unit tests, and both tests were
verified to actually fail when the constants were corrupted. Look instead at:

| Symptom | Where to look |
|---|---|
| Connects, nothing else happens | GATT discovery. The phone serialises one operation at a time with a 5 s timeout each; a silent stall means a callback never arrived |
| Clock sets, but the app keeps retrying | The Status notify is not reaching the phone. §4 makes that notification the *only* thing that resets backoff — a successful connect is explicitly not enough |
| App reports a failure code | Read it literally; §3.2's table says exactly what the watch decided. `BadVersion` (2) means the two sides have drifted and retrying will not help |
| Works once, then never again | Classic leaked GATT client, or a changing MAC. Check stage 1's reboot test again |
| Watch resets during the exchange | The un-fed watchdog interval. §5.1's table, and note the teardown tail is bounded by NimBLE's 2 s host-stop timeout, not by us |

---

## Stage 5 — find the phone

Added 3 Sep 2026 with the feature. The search itself ran against hardware the
same day — the phone connected within seconds, rang, and stopped on both Stop and
Back. The Menu tone toggle was added afterwards and **its rows have not yet been
run**. Do the stage only after stage 4 has completed at least once: it is stage 4
with the link held open, and a failure here has all of stage 4's candidate causes
plus its own.

On the watch, Menu → Find phone. Expect, in order:

| Check | Why it matters |
|---|---|
| The screen shows "searching", `0:00  try 1`, and then advances every ~5 s | The find session is running and redrawing on its round clock. A screen that never advances means the loop is stuck in `StillWaiting` with no round expiring — check `WORKADAY_DIAG` for `find:` lines |
| The attempt counter climbs while nobody connects | `core::FindSession` counting rounds. If the phone connects inside the first round the counter stays at 1, which is correct |
| The phone connects within a few rounds, not tens of seconds | The controller's background scan behind `autoConnect` sets this, not us. If it is routinely slow, the fix is on the phone side (a faster scan mode is a design change) and `PROTOCOL.md` §4.1's "few seconds" wording needs the measured figure |
| "connected", then "phone vibrating", and the phone vibrates — no tone | The §4 exchange ran and the Status carried `FIND_PHONE`. §4.1's default is vibration only. "connected" that never becomes "phone vibrating" means the Time write or the notify failed — the ordinary stage 4 table applies |
| The phone's screen lights up over the lock screen with a Stop button | The full-screen intent. If instead a heads-up notification appears with Stop, `USE_FULL_SCREEN_INTENT` has been revoked — `app/docs/background-execution.md` §2 — and everything else still works |
| Menu on the watch: the watch says "phone ringing" and the tone starts on the phone; Menu again: "phone vibrating", the tone stops, the vibration does not | The `FindMode` notify over the phone's `Find` subscription (§4.1). A press the watch shows but the phone ignores means the subscription failed — the phone's log says whether `EnableFindNotifications` completed, and an older app has no subscription at all. A press the watch ignores too: the pin was low again 40 ms after the edge (a bounce), or nobody was connected — Menu does nothing while "searching" |
| With the tone on, sound rises from quiet to full over ~20 s, afresh each time it is switched on | The `FindAlarm` ramp. A phone in "total silence" Do Not Disturb stays silent and only vibrates; that is the platform, not a bug |
| The link drops with the tone on and the phone reconnects: the tone is back with no further press | The mode travels in the Status `flags` (`FIND_SOUND`) as well as in the notify, so a fresh link starts in the mode the wearer chose |
| Back on the watch: the phone stops **at once** and the watch shows the menu | `hangUp()` terminated the link and the phone heard a disconnect. A stop that takes several seconds means the terminate did not go out and the phone waited for its supervision timeout — look at `find: ended … link dropped 0` |
| Stop on the phone: the phone goes quiet and the watch says "phone found" | The `Find` write reached the watch. A watch that keeps searching means the write did not arrive or was malformed — `find: dismiss write result=` says which |
| Left alone: the phone stops at two minutes and the watch returns to the watchface | The cap, on both sides. The phone's own 135 s backstop should never be what ends it; if `adb logcat -s Workaday` shows the ring ending on an operation timeout rather than a disconnect, the watch's hang-up is not reaching the phone |
| Afterwards the minute tick still arrives on the minute | `armTick()` was re-run after the session. A watch that wakes the instant it sleeps has INT still low — the tick fired during the search and the flag was not cleared |
| The watch is still alive after every ending | Twelve watchdog periods went by. A reset during the session means an un-fed interval overran — the feed after each round is what prevents it, and `core::kFindRoundMs` is the number to look at |

Then a **stopwatch on a full-length search** with the phone connected, and a
meter if there is one: `firmware/docs/power-budget.md` prices it at ~1.3 mAh worst
case and every figure in that row is arithmetic.

## Three traps that will not announce themselves

**The pad hold, which is the one that costs you the case.** `deepSleep()` calls
`gpio_deep_sleep_hold_en()` to stop pins floating while the chip sleeps.
`board::power::releaseSleepHold()` is its counterpart and is the **first** line of
`setup()`, ahead of even `WD_DIAG_BEGIN()`. Do not move it, do not fold it into
something else, and do not add a second hold anywhere without adding its release
there too.

The hold lives in the RTC power domain. A core reset does not clear it, a watchdog
reset does not clear it, a brownout does not clear it — **only a power-on reset
does**. So an unreleased hold freezes every pad after the first sleep, U0TXD and
U0RXD included, and then there is no serial output *and* the ROM download loader
cannot answer esptool. It reports "No serial data received" no matter what you try:
`default_reset`, `no_reset`, or holding EN low by hand with IO0 forced low. On v2.0
that means opening the case to disconnect the battery. This has already happened
once, to a watch that was working ten minutes earlier.

Two details that make it worse than it sounds, and both are why this is in the trap
list rather than the changelog:

- **The exclusion lists do not protect against it.** `kDiagUart` keeps GPIO 1 and 3
  out of the GPIO sweep, and the hold latched them regardless. A never-touch list
  protects a pin from `pinMode()`, not from a hold.
- **It erases its own evidence.** `Serial.begin()` on a held pad returns success and
  emits nothing, so the watch looks hung with no way to see that it is not.

A bring-up session is exactly when someone re-enables a sweep, adds a hold "so the
motor cannot twitch", or moves the first few lines of `setup()` around. If the watch
goes silent after its first sleep and esptool cannot reach it, this is what
happened, and no amount of reset-sequence cleverness will help — it needs the
battery disconnected. When you power it back up, **be ready to flash immediately**:
the old firmware re-arms the hold within a second or two of booting, so you have to
be in the download loader before it sleeps again.

**The RTC storage attribute.** `g_health` and `g_persist` live in `.rtc_noinit`
because `RTC_DATA_ATTR` is a *loadable* segment — the bootloader restores it on
every reset that runs the bootloader, which is every reset except a deep-sleep
wake. That defect was live in this codebase and invisible: it made the fault
escalation ladder unreachable and would have turned any crash in the sync window
into a ~130 mAh/day loop.

It stays fixed only while both structs remain aggregates. **Giving either one a
user-declared non-`constexpr` constructor re-emits a dynamic initialiser and
restores the defect** — and it does so with no warning, no error, the section
still `ALLOC`-only, and the symbols still at the same addresses. The only
signature is one extra entry in `.init_array`. This is recorded in `main.cpp`,
`docs/architecture.md` and `docs/review-checklist.md`; it is repeated here because
a bring-up session is exactly when someone adds a constructor "to tidy up".

**The permission auto-reset.** Android resets runtime permissions for apps the
user never opens — and never opening the app is this app's entire design premise.
The recovery path exists (the watchdog pokes the running service so a granted
permission is re-observed) but it has never been exercised on a device. If the app
goes quiet after weeks, check Nearby devices before anything else.

---

## What a good first day looks like

Stage 0 and stage 1 in one sitting, on the bench, with `WORKADAY_DIAG=1` and a
serial monitor open. Stage 2 in parallel on the phone — it needs no watch. Stages
3 and 4 only once both halves are independently boring.

Record what you measure in `firmware/docs/power-budget.md`, and if anything
contradicts `PROTOCOL.md`, **change the document first** and then both sides. It
is the only thing keeping the two implementations honest with each other.
