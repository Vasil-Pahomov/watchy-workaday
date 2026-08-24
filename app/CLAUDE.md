# Workaday — Android companion app

Android app for the **Workaday watch** (SQFMI Watchy v2.0, ESP32-PICO-D4). Its
main function is talking to that watch over BLE **in the background**, for months,
without the user ever opening it.

Development runs as a two-agent loop: a **coder** subagent writes changes, a
**reviewer** subagent audits them against the laws below and cannot edit code.
See `docs/workflow.md`. The five project laws are non-negotiable — they outrank
convenience, brevity, and "it works on my phone".

The firmware lives in the sibling repo `../Firmware` and has its own laws. Read
them before designing anything on the wire: the two sides share one protocol and
one power budget.

---

## The one fact that shapes everything

Firmware Law 1 says the watch **never enables its radio on a routine wake** —
BLE comes up only on a schedule or a user action, always with a hard timeout.

So the watch is *absent from the air almost all of the time*, and when it does
appear it appears briefly and without warning.

Everything follows from that:

- The app's job is **to be ready**, not to be connected. Uptime is measured in
  "windows we were ready for", not "seconds we held a link".
- **Scanning in a loop is wrong** — it burns the phone's battery waiting for a
  peer that is deliberately silent, and Android will throttle it anyway.
- The correct primitive is `connectGatt(context, autoConnect = true, ...)`. That
  hands the wait to the Bluetooth controller, which survives Doze, costs the app
  nothing while idle, and fires when the watch shows up. A pending autoConnect is
  the app's **resting state**.
- Active scanning is for one case only: first-time pairing, user-initiated,
  foreground, hard timeout.
- A disconnect is **normal**, not an error. The watch finished and went back to
  sleep. Re-arm and go quiet. Do not log it as a fault, do not back off, do not
  notify.

## Law 1 — The service outlives boot, Doze, and the task killer

Required, not optional:

- **A foreground service of type `connectedDevice` is the app's resting state.**
  Not a bound service, not a bare `JobService`, not "WorkManager every 15
  minutes". Declare the type in the manifest and hold
  `FOREGROUND_SERVICE_CONNECTED_DEVICE`.
- **It starts on boot.** A receiver for `BOOT_COMPLETED`, `LOCKED_BOOT_COMPLETED`
  and `MY_PACKAGE_REPLACED` starts the service. All three — a package update must
  not silently leave the user unprotected until the next reboot.
- **Do not change the service type to `dataSync`.** `dataSync` is on the list of
  types Android 15 forbids starting from `BOOT_COMPLETED`; `connectedDevice` is
  not (see `docs/background-execution.md`). This is the single load-bearing
  platform fact in the app — a "simplification" here breaks startup on boot with
  a `ForegroundServiceStartNotAllowedException` that no unit test will catch.
- **CompanionDeviceManager association is mandatory, not a nicety.** It is what
  buys `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND` and
  `REQUEST_COMPANION_RUN_IN_BACKGROUND`, and it lets us find the watch without
  `ACCESS_FINE_LOCATION`. Associations survive reboot. Ship the association flow
  before anything else that touches the radio.
- **The Activity is a diagnostic window, nothing more.** Killing it, swiping the
  app from Recents, or never launching it again must change nothing. No state
  that matters lives in an Activity, a ViewModel, or a process-scoped singleton
  that only the UI initialises.
- **`START_STICKY`, plus a watchdog that does not trust it.** A periodic
  `WorkManager` job re-starts the service if it is not running. OEM task killers
  do not honour `START_STICKY`.
- **Battery-optimisation exemption is requested from the UI**, explained, and its
  state surfaced. On the OEM skins listed in `docs/background-execution.md` it is
  necessary but not sufficient — the doc says what else the user must do by hand.
- No work on the main thread. No `Thread.sleep` in the service. Waiting is done
  by a callback or a coroutine that suspends.

## Law 2 — It heals itself; a dead link is a bug, not a state

The app is expected to run for months without being opened. The failure mode we
refuse to ship is **an app that is installed, looks fine, and silently stopped
talking to the watch**. Restarting is always preferable to sitting broken.

Required:

- **Every failure path ends back at "armed and waiting".** There is no terminal
  error state. If you write a branch that can leave the app not-connected and
  not-waiting, that is a bug however rare.
- **Always `close()` the `BluetoothGatt`, never just `disconnect()`.** A leaked
  client burns one of the process's limited GATT client slots; leak enough and
  every later connection fails with no visible cause. One owner, one `close()`,
  on every path including cancellation.
- **Serialise GATT operations.** One outstanding operation at a time, through a
  single queue, each with its own timeout. Android's stack silently drops a write
  issued before the previous callback returns. This is the number one source of
  "works for ten minutes then stops" BLE bugs.
- **Backoff is bounded, capped and jittered**, and it is reset by a *successful
  exchange*, not by a successful connect. Never a hot retry loop: it is a battery
  fire on both sides.
- **Adapter off, airplane mode, unbonded, permission revoked, `status 133`** are
  handled transitions with tests, not crashes and not silent stalls.
- **A crash must restart the app, not wedge it.** Do not wrap the world in
  `try { } catch (e: Exception) { }` to keep a broken process alive — a process
  that dies is restarted by the watchdog and comes back clean. Catch what you can
  genuinely handle; log and let the rest kill the process.
- **Health is counted and persisted** across process death, and escalates into
  longer backoff instead of crash-looping. A successful exchange clears it.
- **Bounded memory.** Fixed-capacity log and event buffers. Nothing that grows
  with uptime — this process lives for months.

## Law 3 — Decisions are pure Kotlin, unit-tested on the JVM

- **`core/` is pure Kotlin**: no `android.*` imports, no `Context`, no
  `BluetoothGatt`, no Android framework types, no `System.currentTimeMillis()`
  (take a clock). It holds the connection state machine, the backoff policy, the
  protocol framing and parsing, the health/escalation rules, and the scheduling
  decisions. It runs on the developer's machine under `gradlew test`.
- **The Android layer is thin**: it performs effects and forwards callbacks. It
  does not decide.
- **Decisions belong in `core/`, effects belong in the Android layer.** If you
  catch yourself writing an `if` that decides *whether* to do something inside a
  `Service`, a `BroadcastReceiver` or a GATT callback, that `if` belongs in
  `core/` where a test can reach it.
- Every new function in `core/` ships with tests **in the same change**,
  including the boundaries: attempt 0 and attempt-at-cap, empty / truncated /
  oversized BLE payloads, a disconnect while a write is pending, adapter off
  mid-operation, clock jumps backwards, notification arriving after `close()`.
- The state machine is tested with a **fake clock and a fake transport**. No
  device, no emulator, no Robolectric in `core/`.
- **`gradlew test` passing is the merge gate.** A change that breaks it is not
  done. A change that adds behaviour without adding tests is not done.

## Law 4 — `git clone` → open in Android Studio → Sync → Run

- Opening the folder in Android Studio and pressing Sync must work with no manual
  steps, no README ritual, no hand-installed SDK component.
- The **Gradle wrapper is committed**. All dependencies and versions live in
  `gradle/libs.versions.toml`. Nothing is installed by hand.
- AGP, Gradle, Kotlin and `compileSdk` are **pinned**. Do not float them.
  `compileSdk` must be a platform that is actually installed — see
  `docs/toolchain.md` for what this machine has, and what to do before raising it.
- No machine-specific paths in committed files. `local.properties` is generated,
  never committed.
- Both `gradlew assembleDebug` and `gradlew test` must succeed from a clean
  checkout.

## Law 5 — One watch, deliberately

This app talks to **one** peripheral: the user's own Workaday watch. It is not a
BLE toolkit and must not become one.

- No generic `BleDeviceManager`, no device-type registry, no transport
  abstraction "in case of a second peripheral". That machinery costs clarity and
  buys nothing here.
- Exploit what we know: the watch's GATT layout is fixed, its MTU is known, its
  duty cycle is defined by the firmware, and there is exactly one bonded unit.
- **The GATT contract is a single source-of-truth file** mirroring the firmware's
  service and characteristic UUIDs, generated from or checked against
  `../Firmware`. Never retype a UUID from memory into a second place.
- This app is **not** targeted at Play Store distribution, so
  `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` is a legitimate tool here. If that ever
  changes, Law 5 changes first, deliberately.

---

## The GATT contract — settled, 17 Aug 2026

It lives in **`../PROTOCOL.md`**, one directory up, shared with the firmware.
UUIDs, both 12-byte payload layouts, the exchange ordering, every timeout, and
the golden test vectors are defined there. Read it before touching anything on
the wire.

- **Mirror it in exactly one file**: `core/…/protocol/WatchProtocol.kt`. No UUID
  literal, field offset or protocol timeout may appear anywhere else in this app
  (`PROTOCOL.md` §8).
- **Do not edit the contract locally.** A change there is a change to both repos:
  land the doc first, then both sides. If something in it is wrong or
  impossible, stop and say so — a silent local deviation desynchronises the
  watch, and the two sides are tested separately, so it would surface only on
  real hardware.
- The §7 golden vectors are reproduced byte for byte by a unit test on each
  side. That test, not the prose, is what stops the implementations drifting.

**What the app does with the watch, v1:** push the phone's time to the watch on
every connection, and read back the result. Nothing else — step counts,
notifications and calendar data are explicitly out of scope (`PROTOCOL.md` §9).
The watch opens a BLE window hourly, plus on demand from its own menu; it is
absent from the air the rest of the time.

## Open items — resolve before the code depends on them

- Bonding and encryption. v1 is deliberately unbonded (`PROTOCOL.md` §2.3), with
  the accepted risk written down there. It is the first thing to add.

## Commands

Run from the repo root. Android Studio's bundled JDK is required — the JDK on
`PATH` is too new for AGP. `docs/toolchain.md` explains and gives the one-time
setup.

```bash
./gradlew test              # JVM unit tests — the merge gate
./gradlew assembleDebug     # build the debug APK
./gradlew lint              # Android lint
./gradlew installDebug      # install on the attached device
adb logcat -s Workaday      # app logs
```

## Layout

```
core/           pure Kotlin: state machine, backoff, protocol, health — JVM-tested
app/            Android: service, receivers, GATT plumbing, CDM, UI shell
core/src/test/  unit tests, one file per core module
docs/           workflow, review checklist, platform facts, toolchain
```
