# Background execution — the platform facts

This is the app's equivalent of the firmware's "verified hardware facts" table.
Android's background rules change every release and are widely misremembered;
most BLE advice on the web predates the rules below and is actively wrong now.

**Do not "fix" section 1 from memory.** Each item was read from the official
documentation on the date given. If you believe one is wrong, re-read the source,
and if it has changed, update this file *with its source* rather than working
around it in code.

---

## 1. Verified against developer.android.com — 12 Aug 2026

### Foreground service types blocked from `BOOT_COMPLETED`

If the app targets Android 14+, a `BOOT_COMPLETED` receiver **cannot** start a
foreground service of these types:

`dataSync` · `camera` · `mediaPlayback` · `phoneCall` · `mediaProjection` ·
`microphone`

`microphone` was blocked in Android 14; the other five were added in Android 15.
Attempting it throws `ForegroundServiceStartNotAllowedException`.

**`connectedDevice` is not on that list** — it can still be started from
`BOOT_COMPLETED`. This is why the service is typed `connectedDevice` and why
Law 1 forbids changing it. The obvious-looking alternative, `dataSync`, would
break start-on-boot outright.

Source: [Android 15 behavior changes — FGS from BOOT_COMPLETED](https://developer.android.com/about/versions/15/behavior-changes-15#fgs-boot-completed)

### Exemptions that let an app start a foreground service from the background

The ones that apply to us:

- receiving `ACTION_BOOT_COMPLETED`, `ACTION_LOCKED_BOOT_COMPLETED` or
  `ACTION_MY_PACKAGE_REPLACED` in a broadcast receiver;
- **using CompanionDeviceManager and declaring
  `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND` or
  `REQUEST_COMPANION_RUN_IN_BACKGROUND`**;
- the user turning off battery optimisations for the app;
- transitioning from a user-visible state, e.g. the user opening the app.

We rely on the first two by design and treat the third as a bonus the user can
grant. Others on the list (high-priority FCM, exact alarms, geofencing,
`SYSTEM_ALERT_WINDOW`, device/profile owner, input method) are not appropriate
here — do not reach for them to paper over a broken service.

Source: [Restrictions on starting a foreground service from the background](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)

### CompanionDeviceManager

- Associating lets us find and talk to the watch **without
  `ACCESS_FINE_LOCATION`** — the system runs the scan on our behalf.
- The association grants `REQUEST_COMPANION_RUN_IN_BACKGROUND`,
  `REQUEST_COMPANION_USE_DATA_IN_BACKGROUND` and
  `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND`.
- **Associations survive reboot.** They are revoked only by uninstall, by
  `disassociate()`, or by us clearing them. So the boot path does not re-pair —
  it re-arms.
- The association by itself **creates no connection and no scanning.** We still
  do the GATT work ourselves. This is the most common misunderstanding of CDM.
- Requires `<uses-feature android:name="android.software.companion_device_setup"/>`.

API levels: basic pairing 26 · device profiles 31 · `AssociationInfo` and
`onAssociationCreated()` 33 · `startObservingDevicePresence(ObservingDevicePresenceRequest)`
**36**.

Presence monitoring changed in Android 16: `startObservingDevicePresence(String)`
with `CompanionDeviceService.onDeviceAppeared()` / `onDeviceDisappeared()` is
deprecated in favour of the `ObservingDevicePresenceRequest` overload, which
binds the service when the watch is in BLE range or connected and unbinds when it
leaves, delivering `onDevicePresenceEvent(DevicePresenceEvent)`.

Which of the two the app should use — or whether it should use either — is a
**decision**, not a platform fact, so it is recorded in §3 rather than here.

Source: [Companion device pairing](https://developer.android.com/develop/connectivity/bluetooth/companion-device-pairing)

---

## 2. Known behaviour — treat as strong priors, verify before depending on one

Not re-checked in this session. If a design decision hangs on one of these,
verify it and promote it to section 1 with its source.

- **Permissions.** `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` are runtime
  permissions from API 31. `BLUETOOTH_SCAN` takes
  `android:usesPermissionFlags="neverForLocation"` when we do not derive
  location — which we do not. `POST_NOTIFICATIONS` is a runtime permission from
  API 33 and the foreground-service notification needs it to be visible; the
  service still runs if it is denied, but the user sees nothing.
- **Doze.** An already-established BLE connection survives Doze; starting a
  *scan* from the background does not, and is throttled. This is the main reason
  Law 1 makes a pending `autoConnect` the resting state rather than a scan loop.
- **Scan throttling.** Repeatedly starting and stopping a scan (roughly more
  than 5 times in 30 seconds) gets the app silently throttled — the scan
  "succeeds" and returns nothing. Silent failure, so it is easy to misdiagnose.
- **`autoConnect = true`** hands the wait to the controller: slower to connect,
  effectively free while idle, survives Doze, and reconnects when the peer
  reappears. Exactly the shape of a peripheral that wakes on a schedule.
- **`status 133`** is Android's catch-all GATT failure. Usually retryable;
  commonly caused by connecting with a stale `BluetoothDevice`, by not calling
  `close()`, or by too many open GATT clients.
- **GATT client slots are limited per process** (traditionally ~32) and a
  `disconnect()` without `close()` leaks one. Leak enough and every subsequent
  connection fails with no visible cause — the classic "works for a day, then
  never again".
- **`START_STICKY` is not honoured by every OEM.** Hence the WorkManager
  watchdog.
- **A user `force-stop` is not something the watchdog can undo, and is not meant
  to be.** Verified on a Galaxy S20 FE (Android 13), 19 Aug 2026: after
  `am force-stop`, `dumpsys package` reports `stopped=true`, the app's scheduled
  work is gone and no broadcast reaches it; the service came back only when the
  Activity was launched, after which `stopped=false` and the foreground service
  was up again immediately. This is the one kill the app must **not** try to
  survive — Android treats it as the user's explicit instruction. The watchdog is
  for process death that carries no such instruction, which from the outside looks
  the same. Whether an OEM "put to sleep" sets the same flag is **not** verified
  and matters: if it does, no amount of watchdog will help and the per-vendor
  advice on the diagnostic screen is the only remedy.
- **OEM battery managers are the real adversary.** Xiaomi/MIUI, Huawei/EMUI,
  Samsung (device care / "put unused apps to sleep"), OnePlus/Oppo/Vivo and
  others kill background services regardless of the AOSP rules, and require
  per-vendor user action — autostart permission, "unrestricted" battery usage,
  exclusion from deep sleep. No API detects this reliably. See
  [dontkillmyapp.com](https://dontkillmyapp.com) for the per-vendor steps.
  **The app must tell the user what to do on their phone and show whether it
  looks like it worked** — this is a product requirement, not an FAQ entry.

---

## 3. What this means for the app's shape

- Resting state: foreground service (`connectedDevice`) alive, one pending
  `connectGatt(autoConnect = true)`, radio idle, app doing nothing.
- Boot: receiver → start the service → re-arm from the surviving CDM
  association. No re-pairing, no scan.
- The watch appears → GATT connects → serialised exchange with per-operation
  timeouts → the watch sleeps → disconnect is **normal** → `close()`, re-arm.
- Anything unexpected → capped, jittered backoff → re-arm. Never a terminal
  state, never a hot loop.
- Watchdog: periodic WorkManager job re-starts the service if it is gone.
- Active scanning happens exactly once — first-time pairing, in the foreground,
  user-initiated, with a hard timeout.

### Decision — CompanionDeviceManager presence monitoring is not used at all

*Settled 18 Aug 2026, APP-4. §1 used to record this as a live question with two
answers; it has a third.* **Neither API is used.** The app calls `associate()`,
`getAssociations()` / `getMyAssociations()` and `disassociate()`, and nothing else
on `CompanionDeviceManager`. There is no `CompanionDeviceService` in the manifest.

Three reasons, in order of weight:

1. **It would buy nothing.** The resting state is already a pending
   `connectGatt(autoConnect = true)`, which the Bluetooth controller holds for us,
   which survives Doze, and which fires when the watch appears. Presence monitoring
   would report the same event a moment earlier, at the cost of a bound service and
   a second lifecycle to keep correct for months. §1 also records that the
   association creates no connection either way, so nothing is lost.
2. **It would be a delivery path that bypasses `WatchLink.deliver`.** That is where
   the `ClientEpoch` gate lives — the thing that stops a closed GATT client's
   callbacks reaching the state machine, without which every successful sync is
   followed by a spurious backoff step. A `CompanionDeviceService` callback routed
   into the machine would not pass through it and would be invisible to
   `WatchLinkDeliveryTest`, which covers exactly one path.
3. **The new overload needs `compileSdk 36`, which is not installed here**
   (`docs/toolchain.md`). This is the *least* important of the three and it is
   worth saying so: the deprecated overload is perfectly reachable at
   `compileSdk 35`, so the SDK constraint never forced the answer. If platform 36
   were installed tomorrow the answer would not change.

If this is ever reopened — say, because a firmware change makes connection latency
matter — the deprecated `startObservingDevicePresence(String)` path is the one to
use while `compileSdk` is 35, and the new events must enter the state machine
**through** `WatchLink`, not around it.
