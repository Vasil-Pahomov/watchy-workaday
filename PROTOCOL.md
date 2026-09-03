# Workaday — the watch ⇄ phone contract

This file is the **single source of truth** for everything the firmware and the
Android companion app must agree on: UUIDs, byte layouts, timeouts, and who is
allowed to do what when.

It exists because both projects forbid inventing this material locally —
`firmware/CLAUDE.md` Law 5 and `app/CLAUDE.md` Law 5 both say the contract lives
in **one** place and is never retyped from memory into a second one.

```
Workaday/
  PROTOCOL.md   ← you are here: the contract, in prose and tables
  firmware/     mirrors it in src/core/protocol.h   (pure, host-tested)
  app/          mirrors it in core/…/WatchProtocol.kt (pure Kotlin, JVM-tested)
```

**Rules for changing this file:**

1. A change here is a change to both implementations. Land the doc first,
   then both sides.
2. Any change to a byte layout, a UUID, or a semantic meaning **bumps
   `PROTO_VERSION`**. Reserved bytes may be given meaning without a bump only if
   an old receiver ignoring them is still correct.
3. The golden vectors in [§7](#7-golden-test-vectors) are load-bearing. Both
   sides have a unit test that reproduces them byte for byte. That test is what
   actually stops the two implementations from drifting — the prose does not.

Status: **v1**, defined 17 Aug 2026. **Find phone added 3 Sep 2026** as v1
material: one `Status` reserved byte became `flags`, and one write-only
characteristic was added. No `PROTO_VERSION` bump, under rule 2's exception — an
old receiver that ignores both still performs a correct time sync (§3.2, §3.3).
The find-phone half is **not yet verified on hardware**; `BRINGUP.md` stage 5.

**Verified on hardware 18 Aug 2026** — the watch's half only. A full §4 exchange
completed against a third, independent implementation of this document (a PC BLE
client written from §3 and §4, not from either codebase): `result = 0`, every
§3.2 field on its documented offset, `applied_utc_epoch_s` echoing the epoch
sent, and the PCF8563 holding the time afterwards. §2.2's advertisement is
correct on air — service UUID in the AD, name in the scan response, address
stable across reboots — and the advertising window measures 6000 ms to the
millisecond.

**Both halves verified together 19 Aug 2026.** The Android app paired over
CompanionDeviceManager, its foreground service holds the resting state, and it has
set the watch's clock repeatedly: one exchange per window, `result = 0`,
`Succeeded` and no spurious failure behind it. `MY_PACKAGE_REPLACED` restarts the
service, the association survives and is re-confirmed on start, and the watchdog
pokes the running service on schedule.

So the wire format has three independent readers and the exchange is real. What
is still unverified: the §5.1 idle-after-connect path (a central that connects and
writes nothing — the only ending that tears down with a live link), longevity of
any kind (this has run for hours, and the design is about months), the Android
14/15 `BOOT_COMPLETED` foreground-service restrictions (the test phone is Android
13, where they do not apply), and every energy figure in
`firmware/docs/power-budget.md`, which remains estimates with no meter reading
behind them. See `BRINGUP.md`.

---

## 1. Roles, and why they are this way round

| | Watch (Watchy v2.0) | Phone (Android) |
|---|---|---|
| GATT role | **Peripheral / server** | **Central / client** |
| Advertises | yes, in short windows | never |
| Scans | never | only once, at first pairing |
| Resting state | deep sleep, radio off | pending `connectGatt(autoConnect = true)` |

The watch owns the schedule and the phone owns the patience. This falls straight
out of the two Law 1s: the watch's radio is the single largest draw on a 200 mAh
cell, so it is off almost always and the watch alone decides when it appears;
the phone therefore cannot poll and must not scan, and hands the wait to the
Bluetooth controller with `autoConnect = true`, which costs the app nothing while
idle and survives Doze.

**A disconnect is normal.** The watch finished its window and went back to sleep.
Neither side treats it as an error, logs it as a fault, or backs off because of
it.

---

## 2. Identity

### 2.1 UUIDs — fixed forever

All 128-bit. Derived from one base with a varying first group; the base is
arbitrary and permanent.

| What | UUID |
|---|---|
| Service — Workaday Sync | `57444159-6461-4779-b0a3-1f4c7e25d908` |
| Characteristic — **Time** (write) | `57444101-6461-4779-b0a3-1f4c7e25d908` |
| Characteristic — **Status** (read + notify) | `57444102-6461-4779-b0a3-1f4c7e25d908` |
| Characteristic — **Find** (write) | `57444103-6461-4779-b0a3-1f4c7e25d908` |
| Descriptor — CCCD on Status | `00002902-0000-1000-8000-00805f9b34fb` (SIG standard) |

`0x57444159` is `'WDAY'`; `0x57444101` / `…02` / `…03` continue the family. This
is a mnemonic, not a mechanism — do not derive new UUIDs by incrementing without
adding them to this table.

`Find` was added 3 Sep 2026 for the find-phone feature (§4.1). Adding a
characteristic does not bump `PROTO_VERSION`: a phone that does not know it never
touches it, and a watch that lacks it fails the phone's dismiss write locally
(§6.2), leaving the time sync — the only thing v1 promised — intact either way.

### 2.2 Advertising

| Property | Value | Why |
|---|---|---|
| Type | connectable undirected (`ADV_IND`) | the phone must be able to connect |
| Interval | 100 ms | slow enough to matter for current, fast enough that a low-duty-cycle background scan finds it inside a 6 s window |
| AD payload | Flags (3 B) + complete list of 128-bit service UUIDs (18 B) = 21 B | the service UUID must be in the **advertisement**, not the scan response, because `autoConnect` and CDM filtering both work from it |
| Scan response | complete local name `Workaday` | 31 bytes cannot hold both the 128-bit UUID and a name; the name is diagnostic, the UUID is functional |
| Address | **public / factory-derived, stable across reboots** | CDM association and a pending `autoConnect` are keyed to the MAC. A resolvable-private or randomised address silently breaks reconnection after a reboot, and looks like "the app just stopped working". |
| TX power | default | not tuned; revisit only with a measurement |

### 2.3 Security — v1 has none, deliberately

v1 is **unbonded and unencrypted**. The GATT server accepts a Time write from
any central.

The accepted risk: someone within a few metres, during one of the watch's short
windows, could set the watch's clock — or, during a find-phone session (§4.1),
could write `Find` and end the search with "phone found". That is the whole
blast radius — there is nothing readable that is private, and the second write is
only accepted while the wearer is standing there watching the search run.

The cost avoided: bonding state in the watch's NVS, a re-pair path on both sides
for "the bond was dropped", and a longer first connection.

**This is a v2 item, not a permanent decision.** When it changes: LE Secure
Connections, Just Works, bond initiated from the phone during the CDM
association flow, and the Time characteristic gains an
"encryption required" permission. That bumps `PROTO_VERSION`.

---

## 3. Wire format

Little-endian throughout. All payloads are **fixed length** and a wrong length is
rejected without being parsed — a receiver never reads past what it was given,
and never guesses at a truncated packet.

All payloads are ≤ 20 bytes, so they fit the **default ATT MTU of 23** and
**neither side negotiates the MTU**. Keep it that way: an MTU exchange is one
more GATT operation, one more timeout, and one more failure mode, for no gain.

### 3.1 `Time` — phone → watch, write **with response**, exactly 12 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `proto_version` | `0x01` |
| 1 | 1 | `msg_type` | `0x01` = SetTime |
| 2 | 4 | `utc_epoch_s` | u32, seconds since 1970-01-01T00:00:00Z |
| 6 | 2 | `utc_offset_min` | **i16**, signed. `local = utc + offset·60` |
| 8 | 1 | `flags` | reserved, sender writes 0 |
| 9 | 3 | `reserved` | sender writes 0 |

Receiver **ignores** `flags` and `reserved` — that is what lets v1.x add a field
without a version bump. It does **not** ignore a wrong length.

`utc_offset_min` already includes DST. The watch has no timezone database and
must never try to derive one; a DST transition reaches it as a different offset
on the next sync, which is why the sync interval is measured in hours, not days.

**Validation, performed by the watch before anything is written to the RTC:**

| Check | Failure result |
|---|---|
| length == 12 | `BadLength` (1) |
| `proto_version` == 1 | `BadVersion` (2) |
| `msg_type` == 0x01 | `BadType` (5) |
| `utc_offset_min` ∈ [−840, +840] | `OutOfRange` (3) |
| resulting local `DateTime` passes `core::isValid()` (year 2020–2099) | `OutOfRange` (3) |

u32 epoch seconds overflow in 2106; the watch's own `kMaxYear` is 2099, so the
range check bites first and the overflow is unreachable. Stated so nobody
"fixes" it into a u64 and breaks the 12-byte layout.

### 3.2 `Status` — watch → phone, read + notify, exactly 12 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `proto_version` | `0x01` |
| 1 | 1 | `msg_type` | `0x81` = SyncResult |
| 2 | 1 | `result` | see table below |
| 3 | 1 | `battery_percent` | 0–100, or `0xFF` = unknown |
| 4 | 4 | `applied_utc_epoch_s` | u32, what the watch actually committed; 0 if nothing was |
| 8 | 2 | `fw_build` | u16, firmware build tag, diagnostic only |
| 10 | 1 | `flags` | bit 0 = `FIND_PHONE` (§4.1); bits 1–7 reserved, sender writes 0, receiver ignores |
| 11 | 1 | `reserved` | sender writes 0 |

`flags` was `reserved` until 3 Sep 2026 and is the one byte given meaning under
rule 2's exception: a receiver that ignores it still performs a correct time sync,
so `PROTO_VERSION` stays at 1. `FIND_PHONE` set means **the watch is running a
find-phone session and asks the phone to make itself heard** for as long as this
link lasts — it says nothing about `result`, which keeps its own meaning. The
sender puts only the defined bit on the wire; undefined bits are never set, so a
future receiver that gives one meaning cannot be triggered by this build.

`result` codes:

| Value | Name | Meaning |
|---|---|---|
| 0 | `Ok` | validated and written to the PCF8563 |
| 1 | `BadLength` | payload was not 12 bytes |
| 2 | `BadVersion` | `proto_version` mismatch |
| 3 | `OutOfRange` | epoch or offset outside the accepted range |
| 4 | `RtcWriteFailed` | validated, but the I2C write to the PCF8563 did not take |
| 5 | `BadType` | unknown `msg_type` |
| 6 | `Busy` | the watch is closing the window; try the next one |

On a **read** of Status before any write has happened in this connection, the
watch reports the result of the **previous** sync (from RTC-backed state) with
`applied_utc_epoch_s` set to the last successfully applied value. That makes the
characteristic useful to the app's diagnostic screen without requiring a write
first.

`battery_percent` comes from state the watch already keeps; it costs no extra ADC
read. Report `0xFF` rather than a stale guess if no sample has ever been taken.
The **sender normalises**: anything outside 0–100 is encoded as `0xFF`, so a
garbled sample never reaches the wire as a fourth kind of value the reader has to
guess at. The reader may therefore treat 0–100 and `0xFF` as the complete domain.

### 3.3 `Find` — phone → watch, write **with response**, exactly 4 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `proto_version` | `0x01` |
| 1 | 1 | `msg_type` | `0x02` = FindDismiss |
| 2 | 2 | `reserved` | sender writes 0, receiver ignores |

The phone writes this once, when the user silences the find-phone alarm on the
phone (§4.1). It means "the phone has been found, from the phone's side": the
watch ends its search, shows that the phone stopped it, and hangs up.

**Validation, performed by the watch:**

| Check | Failure result |
|---|---|
| length == 4 | `BadLength` (1) |
| `proto_version` == 1 | `BadVersion` (2) |
| `msg_type` == 0x02 | `BadType` (5) |

The codes are §3.2's, reused as a vocabulary; **a rejected `Find` write is not
answered.** `Status` is notified only after a Time write (§4), and there is no
other channel back, so a malformed dismiss is logged on the watch and ignored —
the search continues and the phone keeps ringing until it either writes a valid
frame, hangs up, or the watch's own cap ends the session. Outside a find-phone
session a `Find` write of any shape is ignored.

---

## 4. The exchange

```
watch                                                     phone
  │                                                         │
  │  (wakes: hourly boundary, or the user picked "Sync")     │  resting:
  │  BLE session opens ──► advertising                       │  pending autoConnect
  │                                                          │
  │ ◄──────────────────── connect ───────────────────────────┤  controller fires
  │  [feed watchdog: real progress]                          │
  │                                                          │
  │ ◄────────── discover services (may be cached) ───────────┤  op 1
  │ ◄────────── write CCCD(Status) = 0x0001 ─────────────────┤  op 2
  │ ◄────────── write Time (12 B, with response) ────────────┤  op 3
  │  validate → board::rtc::write() → encode Status          │
  │  [feed watchdog: real progress]                          │
  │ ─────────── notify Status (12 B) ───────────────────────►│
  │                                                          │  result == Ok
  │                                                          │  → exchange succeeded
  │                                                          │  → reset backoff + health
  │ ◄──────────────────── disconnect ────────────────────────┤  then close()
  │  tear down radio, deep sleep                             │  re-arm autoConnect
```

Ordering rules that are not negotiable:

- The phone enables notifications on Status **before** writing Time. Writing
  first races the notification and loses it.
- **The watch notifies Status only after processing a Time write.** Never on
  CCCD subscribe, never unprompted, never more than once per write. This is a
  firmware obligation and it is load-bearing on the phone side: Android delivers
  the write-response and the notification independently and the notification can
  win, so the phone must accept a notify from the moment it *issues* the write.
  A frame sent any earlier lands inside that window — and by §3.2 a Status read
  before this connection's write reports the **previous** sync, so an unprompted
  notify can carry a stale `result == 0` and wrongly reset the phone's backoff
  and health counters. A watch that notifies on subscribe would look correct on
  both benches and fail only when the two halves meet.
- The phone issues **one GATT operation at a time**, each through a single queue,
  each with its own timeout (`app/CLAUDE.md` Law 2). Android silently drops a
  write issued before the previous callback returns.
- The **notification with `result == 0` is the definition of a successful
  exchange.** A successful *connect* is not. Backoff and health counters reset on
  the notification and on nothing else.
- The phone pushes the time **once per connection**, on every connection. A
  second push in the same connection happens only if the first one failed.
- **One exchange per window, not merely one per connection.** After a successful
  exchange the phone must not begin another one inside the same window — it holds
  off until §5.2's whole-exchange budget has elapsed from its connect, which is by
  construction past the watch's §5.1 session cap.

  This is not pedantry, it is a defect that shipped. The watch keeps advertising
  for the remainder of its window, so a phone that re-arms `autoConnect` the
  instant it succeeds gets a fresh connection back in **16 ms** and starts a
  second exchange. Measured on hardware: twelve exchanges in one window, the
  window running to its 12 s cap instead of ending, twelve RTC writes instead of
  one, and — once the watch closed the window mid-way through the last one — a
  `DisconnectedMidExchange`, a 36 s backoff and a health increment charged against
  a sync that had already worked. Every successful sync was followed by a
  spurious failed one.

  Note that the failure path never had this problem: a failed exchange already
  waits ≥ 30 s, which outlasts any window. Success was the only path that re-armed
  with no delay, which is why the defect appeared only after a sync that worked.
- Either side may vanish at any point. Both sides' timeouts below are what makes
  that survivable rather than a hang.

### 4.1 Find phone

The wearer picks **Find phone** on the watch. The watch opens a **find session**:
the same radio, the same service, but a different shape of window — it
advertises continuously for up to **120 s** (§5.1), redraws its screen every few
seconds with the elapsed time and an attempt counter, and ends only on one of
the four endings below. The phone's half is the ordinary §4 exchange with one
bit set in the answer.

```
watch                                                     phone
  │  Find phone pressed → find session opens                 │  resting:
  │  advertising ─────────────────────────────────────────►  │  pending autoConnect
  │  (screen: "searching  0:05  try 2")                      │
  │ ◄──────────────────── connect ───────────────────────────┤
  │ ◄──── discover · CCCD(Status) · write Time ──────────────┤  §4, unchanged
  │ ─────────── notify Status, flags.FIND_PHONE = 1 ────────►│
  │  (screen: "phone ringing")                               │  alarm starts: sound + vibration
  │                                                          │  link stays OPEN
  │            … ringing, both sides waiting …               │
  │                                                          │
  │  ending (a): Back pressed, or 120 s cap                  │
  │ ─────────────────── disconnect ─────────────────────────►│  alarm stops, re-arm at once
  │                                                          │
  │  ending (b): the user silences it on the phone           │
  │ ◄──────────── write Find (4 B, FindDismiss) ─────────────┤  alarm already stopped
  │  (screen: "phone found")                            │
  │ ─────────────────── disconnect ─────────────────────────►│  close, re-arm at once
```

Rules:

- **The phone's exchange is §4 verbatim.** Discover, enable notifications, write
  Time once, wait for Status. A find session that reaches a phone therefore also
  sets the watch's clock, and the outcome is recorded on the phone exactly as any
  other exchange would be — health, backoff and the diagnostic record all see an
  ordinary sync. The find session **spends the hourly timer** when it opens,
  like the Sync item (§5.1).
- **`flags.FIND_PHONE` in a well-formed Status frame starts the alarm, whatever
  `result` says.** A clock that could not be set is no reason to leave the phone
  lost. The phone keeps the link open instead of hanging up, and starts sound and
  vibration on its alarm channel. A frame the phone cannot decode carries no
  flag and is a failed exchange as before.
- **The alarm sounds exactly while the find link is up.** It stops when the link
  ends, for any reason: the watch hanging up because the wearer pressed Back or
  the cap expired, or the link being lost. The phone does not need to know which.
  Behind that sits the phone's own **ring backstop** (§5.2), longer than the
  watch's cap, so the watch's hang-up is the normal ending and the backstop only
  catches a disconnect that never arrived.
- **Silencing on the phone writes `Find`, then closes.** The phone stops its alarm
  the moment the user asks, writes one FindDismiss frame with response, and closes
  the link when the write completes — success, failure or timeout alike. The watch
  processes the write before the response leaves it, so by the time the phone
  closes, the watch already knows the search ended on the phone and says so.
- **After a find link ends the phone re-arms immediately** — no §4 settle and no
  backoff. §4's one-exchange-per-window rule is about the watch's 12 s window,
  and a watch in a find session is deliberately still advertising: if the link
  was lost rather than ended, the watch is still looking and wants the phone back.
  If the search ended, the watch has gone to sleep and the pending `autoConnect`
  simply waits, as it always does.
- **The watch re-advertises after a lost link** and counts a new attempt. It hangs
  up explicitly when its session ends, and waits briefly for the link to drop so
  the phone hears a disconnect rather than a supervision timeout.
- **Only the wearer can start a search.** There is no phone-to-watch request, no
  scan, and nothing in the advertisement changes: a phone learns of the search
  from the Status frame and from nowhere else.

---

## 5. Timing — the load-bearing numbers

### 5.1 Watch

| Constant | Value | Why exactly this |
|---|---|---|
| Sync window interval | **60 min** | hourly; see the power budget below |
| Advertise-with-no-connection timeout | **6 s** | must be under the 10 s task watchdog, because nothing feeds it while the watch is merely advertising |
| Idle-after-connect timeout | **4 s** | connected but no valid write → tear down. The watchdog was fed on connect, so this is a fresh un-fed interval |
| Absolute session cap | **12 s** | measured from session open to teardown, regardless of any activity. Belt to the two braces above |
| User-initiated window | same limits | the Sync menu item opens a window immediately and resets the hourly timer |
| **Find session cap** | **120 s** | §4.1. Measured from the session opening; ends the search whatever the link is doing, then the watch returns to the watchface. Also resets the hourly timer when it opens |
| Find hang-up wait | ≤ 1 s | after requesting termination the watch waits for the link to drop, so the phone hears a disconnect and stops ringing at once. Watch-side only; not mirrored |

The find session is **not** one long un-fed interval. It is a sequence of bounded
waits, each no longer than the 6 s advertising timeout above, and each followed by
a completed step — a panel redraw, a connect, a processed write — at which the
watchdog is fed. So the 10 s watchdog margin below is the same for a two-minute
search as for a six-second window; what a search costs is energy (§5.3), not
reliability.

**The watchdog is the binding constraint here, and it is easy to get wrong.**
`board::power::kWatchdogTimeoutSeconds` is **10 s**. Firmware Law 2 forbids
feeding the watchdog from inside a wait loop, which is exactly what a BLE window
looks like. The resolution:

> **No un-fed interval may exceed 6 s of window time, plus a teardown tail
> measured at ~13 ms. Worst case ~6.05 s against the 10 s watchdog.**
>
> The watchdog is fed at three genuine progress points and nowhere else:
> (a) the NimBLE stack came up and the controller synced, (b) a central
> connected, (c) a valid Time write was processed through to the RTC. Each is a
> completed step, not a moment inside a wait — that distinction is the whole of
> Law 2's rule.

**This paragraph has been wrong twice, in opposite directions.** The first draft
omitted the teardown tail entirely. The second bounded it at ~2.05 s, from
`NimBLEDevice::deinit()` → `ble_hs_stop()` waiting on a semaphore armed by
`CONFIG_BT_NIMBLE_HS_STOP_TIMEOUT_MS`. That call is now gone: it panicked core 0
on **every** teardown, connection or not — `ble_hs_stop()` moves the host out of
its enabled state, after which `ble_hs_timer_reset()` frees the host timer
instead of re-arming it while GAP is still scheduling it. The watch therefore
stops advertising and lets `esp_deep_sleep_start()` drop the radio at the
hardware level.

So there is no host-stop wait any more, and the tail is the teardown's own work:

| Case | Un-fed interval |
|---|---|
| Advertising, nobody comes | 6.00 s + ~13 ms ≈ **6.05 s** |
| Connected but idle → timeout | 4.00 s + ~13 ms ≈ 4.05 s |
| Write → notify → hang-up wait | 4.00 s + ~13 ms ≈ 4.05 s |
| Advertising expires as a central connects | no longer distinct — nothing waits on the link |

Measured on hardware, advertise-timeout path: window closed at 9807 ms, teardown
done at 9820 ms, `deepSleep()` entered in the same millisecond.

**Margin to the watchdog is ~3.95 s, and the advertising timeout is what spends
it.** Raising `kAdvertiseTimeoutMs` above 6000 without re-deriving this table
walks toward a reset, and a reset during a sync window is the expensive kind: it
costs ~10 s at radio current on the wake that was already the most expensive of
the day.

**The cost of not calling `deinit()`, stated because it is invisible otherwise:**
between the destructor returning and `deepSleep()` being reached, the controller
is still enabled. Measured at 0 ms today. Anything inserted between the window
closing and the sleep — a confirmation screen, a retry, a second window — holds
the radio up for that whole stretch at tens of milliamps against a ~20 µA floor,
and nothing in the code will say so. On the idle-timeout and cap endings the link
is also no longer hung up, so the phone sees a supervision timeout rather than a
clean disconnect; §6.2's "disconnect mid-exchange" row already covers it, but it
is slower for the phone than §6.1's normal ending.

A window that opens must reset the hourly timer **when it opens**, not when it
succeeds. Otherwise a watch whose phone is out of range spends every wake
advertising, which is the "unbounded retry" failure Law 2 exists to prevent.

Gating — a window does **not** open when:

- run mode is `Safe` or `Recovery` (Firmware Law 1: no radio in degraded modes);
- battery level is `Low` or `Critical`;
- fewer than 60 minutes have elapsed since the last window **opened**.

That last gate applies **whether or not the clock is trustworthy**, and the
wording matters in both directions:

- If an unreadable RTC made the gate *inapplicable*, a watch with a dead clock
  would advertise on every wake — 1440 windows a day, roughly 26 mAh/day against
  a 9.5 mAh/day allowance. That is precisely the unbounded retry the paragraph
  above forbids, arriving through the back door.
- If the elapsed counter instead *froze* whenever the clock was bad, the watch
  could never reach 60 minutes, so it could never open the window that would set
  its clock — the one watch that most needs the time would be the one that can
  never ask for it.

So elapsed time is measured from the clock when it is trustworthy and from the
tick interval when it is not. The cadence stays hourly either way. The phone
cannot observe this: it only waits on a pending `autoConnect` and has no view of
the watch's schedule.

### 5.2 Phone

| Constant | Value | Why |
|---|---|---|
| Per-GATT-operation timeout | **5 s** | discovery, CCCD write, Time write — each individually |
| Whole-exchange timeout after connect | **15 s** | longer than the watch's 12 s cap, so the watch's own teardown is the normal ending and the phone's timeout is the backstop |
| Backoff base | 30 s | |
| Backoff growth / cap | ×2, capped at **15 min** | comfortably under the watch's hourly window, so the app is always armed again before the next one |
| Jitter | ±20 %, applied **after** the cap | so the worst case is 18 min, not 15. Still far under the hourly window, and the order is phone-side only — the watch cannot observe it, so it cannot desynchronise the two sides |
| Backoff reset | **on a Status notify with `result == 0`** | not on connect |
| **Find ring backstop** | **135 s** | §4.1. Longer than the watch's 120 s find cap, so the watch's hang-up is the normal ending and this is the safety net behind a disconnect that never arrived. When it fires the phone closes and re-arms |
| Find dismiss write | 5 s | the per-operation timeout above, applied to the `Find` write; the phone closes and re-arms when the write completes or times out |
| Re-arm after a find link | **immediate** | no settle, no backoff — see §4.1 for why the 12 s rule does not apply |

### 5.3 What this costs the watch — ⚠ estimates, not measurements

One window, 24/day. The window **extends an existing minute tick** rather than
adding a wake.

| Case | Estimate |
|---|---|
| No phone in range: 6 s advertising @ ~11 mA | ~0.018 mAh |
| Phone present: ~1 s connect + ~1.5 s exchange @ ~40 mA | ~0.028 mAh |
| BLE stack init + teardown | ~0.25 s @ ~40 mA ≈ 0.003 mAh |

Budget against the worse case: **~0.031 mAh × 24 ≈ 0.75 mAh/day**, call it
**~0.9 mAh/day** with margin. Against the 9.5 mAh/day allowance that is ~9 %,
taking the ledger total from ~6.0 to **~6.9 mAh/day** and the headroom from
~37 % to ~27 %.

Affordable, and it is the single most expensive thing in the firmware. Every one
of those numbers is an estimate and must be replaced with a meter reading — see
`firmware/docs/power-budget.md`, which owns the ledger and must be updated in the
same change that adds the radio.

For scale: continuous advertising would be ~20 mAh/day and would empty the cell
in about ten days. That is why the window exists.

**A find session (§4.1) is the most expensive single thing the wearer can ask
for**, and it is priced separately because it is user-initiated and rare rather
than scheduled:

| Case | Estimate |
|---|---|
| Nobody connects: 120 s advertising @ ~11 mA, plus ~24 partial redraws | ~0.4 mAh |
| Phone found at once and the search left to run out: 120 s connected, CPU awake @ ~40 mA | ~1.3 mAh |
| The realistic search — found and stopped in 20–30 s | ~0.1–0.3 mAh |

The worst case is about 14 % of a day's allowance, for one search. It is not in
the daily ledger: it is bounded by the cap, it never opens on its own, and it is
refused on a Low or Critical battery and in the degraded run modes exactly as a
sync window is. `firmware/docs/power-budget.md` carries the row.

---

## 6. Failure handling

### 6.1 Watch

| Situation | Required behaviour |
|---|---|
| Nobody connects | tear down at 6 s, sleep, try again next hour. **Not** a fault; does not touch `core::health` |
| Connected, no write | tear down at 4 s idle, sleep |
| Bad payload | notify Status with the failure code, then let the phone disconnect or time out. Do **not** hang up mid-notification |
| `board::rtc::write()` fails | `RtcWriteFailed` (4). The clock keeps its old value; nothing is half-written |
| BLE stack fails to init | log, skip the window, sleep normally. A radio that will not start must never cost a tick |
| Anything at all | the path still ends in `board::power::deepSleep()`. The RAII session guard tears the radio down on **every** exit including an early return |
| Find: nobody connects inside the cap | end the search, back to the watchface. **Not** a fault |
| Find: link lost mid-search | re-advertise, count a new attempt, keep going until the cap |
| Find: malformed `Find` write | ignore it, keep searching (§3.3) |
| Find: Back pressed, or the cap | hang up, wait ≤ 1 s for the drop, then sleep. Back returns to the menu, the cap to the watchface |
| Find: battery Low/Critical, or Safe/Recovery mode | refused before the radio comes up; the screen says why. Same gates as a sync window |
| Find: BLE stack fails to init | the screen says the radio failed; sleep normally |

### 6.2 Phone

| Situation | Required behaviour |
|---|---|
| Disconnect after a successful notify | normal. `close()`, re-arm `autoConnect`, no backoff, no log-as-fault |
| Disconnect mid-exchange | `close()`, backoff one step, re-arm |
| `status 133` | `close()`, backoff one step, re-arm. Commonly a stale `BluetoothDevice` or a leaked client |
| Operation timeout | `close()` — **never** just `disconnect()` — backoff one step, re-arm |
| Notify with `result != 0` | this is a **failed** exchange: backoff is not reset. Surface the code in the diagnostic UI. `BadVersion` in particular means the two sides have drifted and retrying will not help — show it, do not hot-loop |
| Adapter off / airplane mode / permission revoked / unbonded | handled transition with a test. End state is always "armed and waiting" once the condition clears |
| Any path whatsoever | ends armed and waiting. There is no terminal error state (`app/CLAUDE.md` Law 2) |
| Find: disconnect while ringing | stop the alarm, `close()`, **re-arm immediately**. Normal, not a fault — the watch ended the search, or lost the link and is still looking |
| Find: ring backstop fires | stop the alarm, `close()`, re-arm immediately |
| Find: adapter off / permission revoked while ringing | stop the alarm, `close()`, park in the blocked state as for any other exchange |
| Find: `Find` characteristic missing (an older watch) | the dismiss write fails locally; `close()`, re-arm. The alarm was already stopped by the user's tap |
| Find: the phone is in "Total silence" Do Not Disturb | **platform limit, not handled.** The alarm uses the ALARM audio usage, which the default "priority only" mode lets through; a mode that blocks alarms mutes it, and no app can override that without notification-policy access the user would have to grant separately |

---

## 7. Golden test vectors

**Both sides must have a unit test that reproduces these byte for byte.** This is
the mechanism that keeps the implementations in sync; everything above is prose
and prose drifts.

### 7.1 `Time`

Input: `2026-08-17T12:34:56Z`, UTC offset `+180` minutes (local `15:34:56`).

```
utc_epoch_s    = 1786970096  = 0x6A82FFF0
utc_offset_min = 180         = 0x00B4
```

Encoded, 12 bytes:

```
01 01 F0 FF 82 6A B4 00 00 00 00 00
```

Decoded by the watch to local `DateTime{ 2026, 8, 17, 15, 34, 56 }`.

### 7.2 `Status`

Result `Ok`, battery 78 %, applied epoch as above, `fw_build` 1.

```
01 81 00 4E F0 FF 82 6A 01 00 00 00
```

### 7.3 Payloads that must be rejected

| Bytes | Expected `result` |
|---|---|
| *(empty)* | `BadLength` (1) |
| `01 01 F0 FF 82 6A B4 00 00 00 00` (11 B) | `BadLength` (1) |
| `01 01 F0 FF 82 6A B4 00 00 00 00 00 00` (13 B) | `BadLength` (1) |
| `02 01 F0 FF 82 6A B4 00 00 00 00 00` | `BadVersion` (2) |
| `01 7F F0 FF 82 6A B4 00 00 00 00 00` | `BadType` (5) |
| `01 01 00 00 00 00 B4 00 00 00 00 00` (epoch 0 → 1970) | `OutOfRange` (3) |
| `01 01 F0 FF 82 6A 59 03 00 00 00 00` (offset +857) | `OutOfRange` (3) |

### 7.4 `Status` with `FIND_PHONE`

§7.2's frame, sent from inside a find session (§4.1). Only byte 10 differs.

```
01 81 00 4E F0 FF 82 6A 01 00 01 00
```

The phone reads `findPhoneRequested = true`, and every other field exactly as in
§7.2. A receiver that ignores byte 10 decodes it identically to §7.2 — which is
what rule 2's exception requires.

### 7.5 `Find`

The one frame the phone sends on `Find` — FindDismiss, reserved bytes zero:

```
01 02 00 00
```

Decoded by the watch as a valid dismiss (`Ok`, 0).

### 7.6 `Find` payloads that must be rejected

| Bytes | Expected `result` |
|---|---|
| *(empty)* | `BadLength` (1) |
| `01 02 00` (3 B) | `BadLength` (1) |
| `01 02 00 00 00` (5 B) | `BadLength` (1) |
| `02 02 00 00` | `BadVersion` (2) |
| `01 01 00 00` (a Time `msg_type` on the Find characteristic) | `BadType` (5) |
| `01 7F 00 00` | `BadType` (5) |

And one that must be **accepted**: `01 02 DE AD` — the reserved bytes are ignored,
as everywhere else in this document.

---

## 8. Where each side mirrors this

Exactly one file per side. Nothing else in either codebase may contain a UUID
literal, a field offset, or a timeout from §5.

| Side | File | Tested by |
|---|---|---|
| `firmware/` | `src/core/protocol.h` / `.cpp` — pure C++17, no hardware headers | `test/test_protocol/`, `pio test -e native` |
| `app/` | `core/…/protocol/WatchProtocol.kt` — pure Kotlin, no `android.*` | `core/src/test/…/WatchProtocolTest.kt`, `gradlew test` |

Both sit in the pure/testable half of their project by construction: parsing and
validating a byte array is a **decision**, and both projects put decisions where
a host test can reach them.

---

## 9. Not in v1 — do not build these yet

Listed so that "it's obviously needed" does not quietly become scope.

- Bonding / encryption (§2.3) — the first thing to add.
- Watch → phone data: step counts, battery history, health/fault counters.
- Phone → watch: notifications, calendar, weather. (`Find` is not this: it is
  the phone answering a request the wearer made on the watch, not the phone
  pushing anything of its own.)
- Phone → watch "find my watch": the reverse of §4.1 would need the watch to
  vibrate on a phone's say-so, which means a writable characteristic that costs a
  motor pulse from any central within range while the watch is unbonded (§2.3).
  After bonding, perhaps.
- Any second peripheral, any device registry (`app/CLAUDE.md` Law 5).
- MTU negotiation, long writes, indications instead of notifications.
- OTA over BLE. `firmware/docs/backlog.md` item 9 reserves the slots; the
  transport for it is undecided and is not this service.
