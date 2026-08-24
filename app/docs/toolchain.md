# Toolchain on this machine

Probed 12 Aug 2026. Re-probe if Gradle starts failing in a way that makes no
sense — it is usually the JDK.

| | |
|---|---|
| Android Studio | `C:\Program Files\Android\Android Studio` (build `AI-252.27397.103`) |
| Bundled JDK (JBR) | `C:\Program Files\Android\Android Studio\jbr` — **OpenJDK 21.0.8** |
| JDK on `PATH` | Oracle **Java 26** — too new for AGP/Gradle |
| Android SDK | `C:\Users\Vasil\AppData\Local\Android\Sdk` |
| Platforms installed | **android-34, android-35 only** |
| Build-tools | 30.0.3, 34.0.0, 35.0.1 |
| `adb` | `…\Sdk\platform-tools\adb.exe` — **not on `PATH`** |
| `cmdline-tools` | **not installed** — no `sdkmanager` on the command line |
| Git | `C:\Program Files\Git\cmd\git.exe` |

## The JDK trap — read this before your first Gradle run

`java` on `PATH` is **Java 26**. AGP will not run on it; you get a Gradle
"unsupported class file major version" error that reads like a corrupt build
rather than a wrong JDK. Android Studio does not have this problem because it
silently uses its own bundled JBR 21 — so **the build works in the IDE and fails
on the command line**, which is exactly the confusing case.

Point Gradle at the bundled JBR. For the current shell:

```bash
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
```

Permanently, for this user:

```bash
setx JAVA_HOME "C:\Program Files\Android\Android Studio\jbr"
```

Or set `org.gradle.java.home` in **`~/.gradle/gradle.properties`** — the
user-level file, outside the repo.

**Do not put that path in the repo's `gradle.properties`.** A machine-specific
absolute path in a committed file breaks Law 4 on every other machine, including
CI. This is a per-developer setting.

## SDK platforms — a real constraint, not a detail

Only **android-34 and android-35** are installed, so `compileSdk` should be
**35** until someone deliberately installs another. Raising `compileSdk` to 36
without installing platform 36 first fails the Sync, which breaks Law 4 for
anyone who clones the repo.

This had a design consequence, now settled the other way: the Android 16
CompanionDeviceManager presence API
(`startObservingDevicePresence(ObservingDevicePresenceRequest)`) needs
`compileSdk 36`. **The app uses no presence monitoring at all** — see the decision
in `docs/background-execution.md` §3, which turns on the fact that a pending
`autoConnect` already is the resting state, not on this constraint. If it is ever
reopened while `compileSdk` is 35, the deprecated `startObservingDevicePresence(String)`
path is the reachable one.

There is no `sdkmanager` on the command line here (`cmdline-tools` is not
installed), so extra platforms are installed through **Android Studio → SDK
Manager**, or by installing the command-line tools first. An agent cannot do
this silently — if a change needs a new SDK component, it must say so and stop.

## `adb`

Not on `PATH`. Either use the full path:

```bash
& "C:\Users\Vasil\AppData\Local\Android\Sdk\platform-tools\adb.exe" devices
```

or add `…\Sdk\platform-tools` to `PATH` once and use `adb` directly — the
commands in `CLAUDE.md` assume you have.

## Settled decisions — 17 Aug 2026

The two questions that used to sit here are answered by the user. They are facts
now, not options. Reopening one is a deliberate change, not a tidy-up.

- **SDK levels: `minSdk = 31`, `compileSdk = 35`, `targetSdk = 35`.**
  31 is the floor that gives us the runtime `BLUETOOTH_SCAN` /
  `BLUETOOTH_CONNECT` permissions and CompanionDeviceManager device profiles —
  both load-bearing for Law 1. 35 is the highest platform installed here.

  The consequence is the one flagged in the section above: **platform 36 is not
  installed and the build must not require it**, so the Android 16 presence API
  (`startObservingDevicePresence(ObservingDevicePresenceRequest)`) is out of
  reach. That work is not happening: `docs/background-execution.md` §3 records the
  decision (18 Aug 2026) that the app uses **no** presence monitoring, for reasons
  independent of this constraint. Should it ever be revisited under `compileSdk 35`,
  the deprecated `startObservingDevicePresence(String)` path is the one that
  compiles.

- **The app is sideloaded, not distributed through Play.**
  That is what makes `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` a legitimate tool
  here rather than a policy violation, and it is the assumption Law 5 already
  records. If distribution ever changes, Law 5 changes first.

## Pinned build toolchain — 17 Aug 2026

Chosen against the Android Studio build installed here, not against "latest".
All of it is pinned in the repo; none of it floats.

| | Version | Pinned in | Why this one |
|---|---|---|---|
| AGP | 8.13.1 | `gradle/libs.versions.toml` | the exact version this Studio build names as its own last stable AGP, in `plugins/android/lib/libagp-version.jar` → `com/android/version.properties` (`lastStableBuildVersion = 8.13.1`) |
| Gradle | 8.14.5 | `gradle/wrapper/gradle-wrapper.properties` | above AGP 8.13.1's hard floor (`GRADLE_MIN_VERSION = 8.13`); last 8.x line, so it predates none of the APIs AGP 8.13 uses; 8.14.5 specifically is the security-patch release of that line |
| Kotlin | 2.1.21 | `gradle/libs.versions.toml` | the compiler this Studio build bundles (`plugins/Kotlin/kotlinc/build.txt`), so the IDE analyser and the Gradle build agree |
| build-tools | 35.0.1 | `app/build.gradle.kts` | AGP 8.13.1 would otherwise default to 35.0.0, which is **not** installed here — pinning stops Sync trying to download an SDK component |
| JVM target | 17 | both module build files | AGP 8.x baseline; compiled *by* JBR 21, targeting 17 bytecode, so no JDK 17 toolchain has to be provisioned |

The wrapper carries `distributionSha256Sum`, so a tampered or truncated Gradle
download fails loudly instead of silently.

## Command-line builds need two environment variables

Android Studio sets both for itself, which is why Sync works before the command
line does. For a shell, using the paths from the table at the top of this file:

```bash
$env:JAVA_HOME    = "<Android Studio>\jbr"   # the bundled JBR 21
$env:ANDROID_HOME = "<Android SDK>"          # so AGP can find the SDK
```

`ANDROID_HOME` is the alternative to `local.properties`, which is generated by
Studio on first Sync and is **git-ignored, never committed** — it holds an
absolute SDK path and committing it breaks Law 4 on every other machine.
