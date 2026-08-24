import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// core/ is a plain Kotlin JVM library. The Android plugin is deliberately absent:
// with no android.jar on this module's compile classpath, an `import android.*`
// here is a compile error rather than a code-review finding (CLAUDE.md Law 3).
plugins {
    alias(libs.plugins.kotlin.jvm)
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        // 17, not 21: this module's classes are dexed into the APK, and 17 is the
        // baseline AGP 8.x targets. Compiled by the JBR 21 toolchain either way.
        jvmTarget = JvmTarget.JVM_17
        allWarningsAsErrors = true
    }
}

dependencies {
    testImplementation(libs.kotlin.test)
    testRuntimeOnly(libs.junit.jupiter.engine)
    testRuntimeOnly(libs.junit.platform.launcher)
}

tasks.test {
    useJUnitPlatform()
    testLogging {
        events("passed", "skipped", "failed")
    }
}
