import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.workaday.app"

    // 35 because android-34 and android-35 are the only platforms installed on
    // this machine (docs/toolchain.md). Raising this requires installing the
    // platform first, by hand, through Android Studio's SDK Manager.
    compileSdk = 35

    // AGP 8.13.1 would otherwise default to build-tools 35.0.0, which is not
    // installed here; 35.0.1 is. Pinning it keeps Sync from trying to download
    // an SDK component (CLAUDE.md Law 4).
    buildToolsVersion = "35.0.1"

    defaultConfig {
        applicationId = "com.workaday.app"

        // 31 is the floor for the runtime BLUETOOTH_SCAN / BLUETOOTH_CONNECT
        // permissions and for CompanionDeviceManager device profiles, both of
        // which Law 1 depends on. Settled 17 Aug 2026 — docs/toolchain.md.
        minSdk = 31
        targetSdk = 35

        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    testOptions {
        unitTests {
            // Plain JVM tests against the stubbed android.jar, whose methods
            // otherwise throw "Stub!". No Robolectric, no emulator: the one thing
            // tested here is WatchLink's delivery wiring — that a callback from a
            // closed GATT client reaches nothing — and it touches no real
            // platform behaviour. Law 3 keeps `core/` free of android.*; it does
            // not say app/ may go untested where a JVM can reach it.
            isReturnDefaultValues = true
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_17
        allWarningsAsErrors = true
    }
}

dependencies {
    implementation(project(":core"))

    // Law 1's watchdog. See gradle/libs.versions.toml for why this one and only
    // this one — nothing else here needs a library, and a dependency in an app
    // that must keep working untouched for months is a liability, not a shortcut.
    implementation(libs.androidx.work.runtime)

    // The same three core/ uses, so there is one test stack in the repo.
    testImplementation(libs.kotlin.test)
    testRuntimeOnly(libs.junit.jupiter.engine)
    testRuntimeOnly(libs.junit.platform.launcher)
}

// Both testDebugUnitTest and testReleaseUnitTest, matching core/.
tasks.withType<Test>().configureEach {
    useJUnitPlatform()
    testLogging {
        events("passed", "skipped", "failed")
    }
}
