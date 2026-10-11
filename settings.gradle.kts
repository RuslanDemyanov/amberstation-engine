// Standalone build of the engine. When this repository is used as a
// submodule of the app, the app's own settings file is used instead.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0"
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "amberstation-engine"
include(":engine-api")
project(":engine-api").projectDir = file("api")
