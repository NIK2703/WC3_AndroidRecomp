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
        // Media3 and the AVI extractor are vendored as a local Maven
        // repository so the canonical --offline APK build does not depend on
        // a Gradle cache descriptor or a network fetch.  Every file is pinned
        // by SHA-256 in third_party/media3/VENDOR_COMMIT.
        maven {
            url = uri(file("../../third_party/media3/repo"))
            metadataSources {
                mavenPom()
                artifact()
            }
        }
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
