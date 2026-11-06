pluginManagement {
    // Gradle 8.10.2 embeds Kotlin 1.9.24 for its own Kotlin DSL, but the
    // distribution jars are not in the modules cache.  Expose them so an
    // offline build resolves the same versions the running Gradle was built
    // with instead of downloading them.
    val gradleLib = gradle.gradleHomeDir?.resolve("lib")
    repositories {
        if (gradleLib != null) {
            flatDir { dirs(gradleLib) }
        }
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        val localMaven = providers.gradleProperty("wc3LocalMaven").orNull
        if (localMaven != null) {
            maven {
                url = uri(localMaven)
                metadataSources {
                    mavenPom()
                    artifact()
                }
            }
        }
        google()
        mavenCentral()
    }
}

rootProject.name = "wc3-android"
include(":app")
