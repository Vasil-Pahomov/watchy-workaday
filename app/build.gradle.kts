// Root project. It builds nothing itself — it only declares the plugins the two
// modules resolve, so their versions live in exactly one place.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.jvm) apply false
}
