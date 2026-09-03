# Review checklist

The reviewer works this list end to end. It is keyed to the five laws in
`CLAUDE.md`. Skip an item only when the change genuinely cannot touch it, and
say so.

When a bug escapes the loop, **add the case here** — that is how this file earns
its keep.

## Law 1 — background survival

- [ ] Foreground service type is still `connectedDevice` in the manifest, and
      `FOREGROUND_SERVICE_CONNECTED_DEVICE` is still declared.
- [ ] The boot receiver handles `BOOT_COMPLETED`, `LOCKED_BOOT_COMPLETED` **and**
      `MY_PACKAGE_REPLACED`. A change that drops the third leaves the user dead
      in the water after every app update until they reboot.
- [ ] `startForeground()` is called promptly on every path into the service, and
      the `ForegroundServiceStartNotAllowedException` path is handled rather than
      allowed to crash the boot receiver.
- [ ] Nothing load-bearing is owned by an Activity, a ViewModel, or a singleton
      only the UI initialises. Ask: if the user never opens the app again after
      install, does this still work?
- [ ] The `WorkManager` watchdog still exists, is still periodic, and still
      actually checks whether the service is running rather than assuming.
- [ ] No `Thread.sleep`, no blocking I/O, no long CPU work on the main thread.
- [ ] Wakelocks — if any — are bounded, and released on every path including
      exceptions.

## Law 2 — self-healing

- [ ] **Trace every exit from the connection code.** Every `return`, `throw`,
      `catch`, and coroutine cancellation ends with the app either connected or
      armed and waiting. Name the path you traced.
- [ ] `BluetoothGatt.close()` happens on every path, including early returns and
      cancellation. No `disconnect()` without an eventual `close()`.
- [ ] Exactly one owner of the `BluetoothGatt` reference; no chance of two live
      clients for one device.
- [ ] GATT operations are serialised — one outstanding at a time — and each has
      a timeout. No write/read/descriptor-write issued from inside another
      operation's callback without going through the queue.
- [ ] Backoff is capped, jittered, and reset by a **successful exchange**, not by
      a successful connect. (Reset-on-connect turns a peer that connects and
      immediately fails into a hot loop.)
- [ ] `status 133` and other non-zero GATT statuses are handled as retryable
      transitions, not treated as success and not fatal.
- [ ] Adapter off / airplane mode / device unbonded / permission revoked at
      runtime are each handled, and at least the decision half is unit-tested.
- [ ] A normal disconnect (the watch finishing its window and sleeping) is
      **not** treated as a fault: no backoff escalation, no error notification,
      no health counter increment.
- [ ] **The find-phone alarm never outlives its link.** Every exit from
      `Ringing` — the watch hanging up, the ring backstop, the user's Stop, the
      adapter or the permission going away, service shutdown — emits
      `StopFindAlarm` before the link closes, and `FakeTransport` refuses an alarm
      sounding in any other state. A ring that is not bounded by a timer, or a
      dismiss that settles or backs off instead of re-arming at once, is a
      `PROTOCOL.md` §4.1 violation.
- [ ] No `catch (e: Exception) {}` that keeps a structurally broken process
      alive. Catch what is genuinely handleable; let the rest restart the process.
- [ ] Health counters persist across process death and escalate to longer
      backoff instead of crash-looping. A successful exchange clears them.
- [ ] Nothing unbounded: retries, queues, log/event buffers, collections keyed by
      connection attempt. This process lives for months.

## Law 3 — pure core and tests

- [ ] No `android.*` import, `Context`, framework type, or wall-clock read
      (`System.currentTimeMillis()`, `Instant.now()`) anywhere in `core/`.
- [ ] No decision left in the Android layer: no `if` in a `Service`,
      `BroadcastReceiver` or GATT callback that decides *whether* rather than
      *how to perform*.
- [ ] New behaviour in `core/` has tests **in this same change**.
- [ ] Boundaries covered: attempt 0 and attempt-at-cap; empty, truncated and
      oversized payloads; disconnect while a write is pending; adapter off
      mid-operation; clock jumping backwards; a notification arriving after
      `close()`.
- [ ] Tests use a fake clock and a fake transport — no real delays, no
      `Thread.sleep` in tests, no flakiness by construction.
- [ ] The tests actually assert something meaningful, and do not mock the unit
      under test. A test that cannot fail is a finding.
- [ ] **For each test, ask what you would have to break to make it fail, then
      check that is the thing its name promises.** The gap this catches is a test
      guarding a property that does not live in the layer the test can reach —
      and it has now escaped this loop three times: a walk certifying that no
      sequence strands the app while never entering one of the states, a firmware
      test named for a timing property that only compared two constants, and a
      `core/` test named for a `close()`-then-callback bug whose mechanism was
      entirely in `app/`. All three passed, and all three passed with the bug
      reintroduced. When the answer does not match the name, the repair is to
      make the property reachable — inject the seam, add the source set — not to
      rename the test. Break it and watch it fail before believing it.
- [ ] `./gradlew test` **run by the reviewer** is green. Not quoted from the
      coder's report — run.

## Law 4 — opens in Android Studio

- [ ] Gradle wrapper still committed and unmodified, or modified deliberately.
- [ ] New dependencies are in `gradle/libs.versions.toml` with a pinned version,
      and each is justified.
- [ ] AGP / Gradle / Kotlin / `compileSdk` unchanged, or changed deliberately —
      and `compileSdk` is a platform actually installed (`docs/toolchain.md`).
- [ ] No absolute or machine-specific path in a committed file.
      `local.properties` not committed.
- [ ] `./gradlew assembleDebug` succeeds.

## Law 5 — one watch

- [ ] No new abstraction whose only justification is a second, hypothetical
      peripheral.
- [ ] No UUID, MTU or protocol constant retyped into a second place — one
      source-of-truth file, cross-checked against `../firmware`.
- [ ] **No invented protocol constants.** If the change needed a UUID or a
      packet layout that is not yet defined, that is a Blocker and it escalates
      to the user. A plausible placeholder is the worst possible outcome here,
      because it looks intentional forever after.

## Cross-cutting

- [ ] The change does what the task asked, not merely something reasonable.
- [ ] No unrelated edits riding along in the diff.
- [ ] Files changed match the files the coder's report claims (`git status`).
- [ ] Nothing was committed.
- [ ] The report's "Risks and unverified claims" section is honest — anything
      needing real hardware, a real Doze transition or a specific OEM skin is
      listed there, because none of it is provable on a JVM.
