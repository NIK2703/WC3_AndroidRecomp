plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val hostBuildDir = providers.gradleProperty("wc3GameActivityBuildDir").orNull
if (hostBuildDir != null) {
    layout.buildDirectory.set(file(hostBuildDir))
}
// BV-3.2: jniLibs belong to a variant, not to `main`.  The generic property is
// a transitional fallback for local invocations that still call Gradle without
// -Variant; build_apk.ps1 always passes the per-variant properties.
val legacyJniLibsDir = providers.gradleProperty("wc3JniLibs").orNull
val hostJniLibsRelease = providers.gradleProperty("wc3JniLibsRelease").orNull ?: legacyJniLibsDir
val hostJniLibsDebug = providers.gradleProperty("wc3JniLibsDebug").orNull ?: legacyJniLibsDir
// BV-11: release signing is supplied from outside the tree.  When no keystore
// is configured the build remains a local release-hygiene artifact, explicitly
// not a distribution artifact.
val releaseStoreFile = providers.gradleProperty("wc3ReleaseStoreFile").orNull

android {
    namespace = "wc3.recomp"
    compileSdk = 34

    defaultConfig {
        applicationId = "wc3.recomp"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
    }

    buildFeatures {
        buildConfig = false
    }

    if (releaseStoreFile != null) {
        signingConfigs.create("wc3Release") {
            storeFile = file(releaseStoreFile)
            storePassword = providers.gradleProperty("wc3ReleaseStorePassword").orNull ?: ""
            keyAlias = providers.gradleProperty("wc3ReleaseKeyAlias").orNull ?: ""
            keyPassword = providers.gradleProperty("wc3ReleaseKeyPassword").orNull ?: ""
        }
    }

    hostJniLibsRelease?.let { sourceSets.maybeCreate("release").jniLibs.srcDir(it) }
    hostJniLibsDebug?.let { sourceSets.maybeCreate("debug").jniLibs.srcDir(it) }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildTypes {
        getByName("debug") {
            // ADR-030 / BV-2: wc3.recomp.debug, installable next to release.
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        getByName("release") {
            // Without wc3ReleaseStoreFile this stays a local release-hygiene
            // artifact signed by the AGP debug key; it is not distribution.
            signingConfig = if (releaseStoreFile != null) {
                signingConfigs.getByName("wc3Release")
            } else {
                signingConfigs.getByName("debug")
            }
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.games:games-activity:4.4.2")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("androidx.core:core:1.13.1")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("org.jetbrains.kotlin:kotlin-stdlib:2.0.21")
}

// Kotlin 2.0's stdlib already contains the jdk7/jdk8 extension classes; the
// old coroutines metadata still asks for the separate 1.6.21 jars.
configurations.configureEach {
    exclude(group = "org.jetbrains.kotlin", module = "kotlin-stdlib-jdk7")
    exclude(group = "org.jetbrains.kotlin", module = "kotlin-stdlib-jdk8")
}

// Offline builds use the local Gradle cache, which carries these AARs but not
// every transitive metadata-only version.  Pin the nearest cached versions
// that satisfy the same API instead of silently resolving to another tree.
configurations.configureEach {
    resolutionStrategy {
        force(
            "androidx.activity:activity:1.8.0",
            "androidx.annotation:annotation:1.3.0",
            "androidx.annotation:annotation-experimental:1.4.0",
            "androidx.arch.core:core-common:2.2.0",
            "androidx.arch.core:core-runtime:2.2.0",
            "androidx.collection:collection:1.1.0",
            "androidx.concurrent:concurrent-futures:1.1.0",
            "androidx.core:core:1.13.1",
            "androidx.core:core-ktx:1.13.1",
            "androidx.emoji2:emoji2:1.2.0",
            "androidx.emoji2:emoji2-views-helper:1.2.0",
            "androidx.lifecycle:lifecycle-common:2.6.2",
            "androidx.lifecycle:lifecycle-livedata-core:2.6.2",
            "androidx.lifecycle:lifecycle-process:2.6.2",
            "androidx.lifecycle:lifecycle-runtime:2.6.2",
            "androidx.lifecycle:lifecycle-viewmodel:2.6.2",
            "androidx.lifecycle:lifecycle-viewmodel-savedstate:2.6.2",
            "androidx.profileinstaller:profileinstaller:1.3.0",
            "androidx.savedstate:savedstate:1.2.1",
            "androidx.startup:startup-runtime:1.1.1",
            "androidx.tracing:tracing:1.0.0",
            "org.jetbrains.kotlin:kotlin-stdlib:2.0.21",
            "org.jetbrains.kotlin:kotlin-stdlib-common:1.8.22",
            "org.jetbrains.kotlinx:kotlinx-coroutines-android:1.6.4",
            "org.jetbrains.kotlinx:kotlinx-coroutines-core-jvm:1.6.4",
        )
    }
}
