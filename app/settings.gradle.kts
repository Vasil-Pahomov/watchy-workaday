pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    // A subproject that declares its own repository is a Law 4 hazard: it makes
    // the build depend on something settings.gradle.kts does not describe.
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "Workaday"

include(":core")
include(":app")
