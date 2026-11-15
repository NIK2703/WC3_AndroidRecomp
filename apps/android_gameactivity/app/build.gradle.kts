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
if (hostJniLibsRelease == null && hostJniLibsDebug == null) {
    throw GradleException(
        "No WC3 native library directory was supplied. " +
        "Run tools/android/build_apk.ps1 (canonical) or pass " +
        "-Pwc3JniLibsRelease=<dir> / -Pwc3JniLibsDebug=<dir>. " +
        "A direct Gradle build must not package src/main/jniLibs."
    )
}
// BV-11: release signing is supplied from outside the tree.  When no keystore
// is configured the build remains a local release-hygiene artifact, explicitly
// not a distribution artifact.
val releaseStoreFile = providers.gradleProperty("wc3ReleaseStoreFile").orNull

android {
    namespace = "wc3.recomp"
    // Media3 1.7.1 is built against API 35.  targetSdk stays at 34 so the
    // runtime behaviour of the product does not change with this dependency.
    compileSdk = 35

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

    // BV-3.2: never let AGP fall back to src/main/jniLibs.  That ignored
    // directory can contain a stale .so from an older native build; only the
    // per-variant directory produced by build_app.ps1 may be packaged.
    sourceSets.getByName("main").jniLibs.setSrcDirs(emptyList<String>())
    hostJniLibsRelease?.let { sourceSets.maybeCreate("release").jniLibs.srcDir(it) }
    hostJniLibsDebug?.let { sourceSets.maybeCreate("debug").jniLibs.srcDir(it) }

    packaging {
        jniLibs {
            // Modern packaging: native libraries stay uncompressed in the APK
            // and are loaded from it, so the product .so is never extracted on
            // install (O-12 stays closed).  libadrenotools still needs real
            // hook files; VulkanDriverStore extracts only the small hook DSOs
            // into filesDir and passes that directory to the loader.
            useLegacyPackaging = false
        }
    }

    buildTypes {
        getByName("debug") {
            // ADR-030 / BV-2: wc3.recomp.debug is the only debug id; all
            // diagnostics run inside this package, installable next to release.
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

    // Campaign movies are RIFF/AVI with MPEG-4 Part 2 video and MP3 audio.
    // Android has no AVI extractor in MediaExtractor/Media3, so the AVI
    // extractor is the small Media3Avi add-on.  Media3 keeps the platform
    // MediaCodec decoder, but owns AVI sample ordering, B-frame timestamps,
    // audio/video sync, late-frame dropping and Surface scheduling.  The
    // files are vendored flat in third_party/media3 for the offline build.
    implementation("androidx.media3:media3-common:1.7.1@aar")
    implementation("androidx.media3:media3-container:1.7.1@aar")
    implementation("androidx.media3:media3-database:1.7.1@aar")
    implementation("androidx.media3:media3-datasource:1.7.1@aar")
    implementation("androidx.media3:media3-decoder:1.7.1@aar")
    implementation("androidx.media3:media3-exoplayer:1.7.1@aar")
    implementation("androidx.media3:media3-extractor:1.7.1@aar")
    implementation("com.github.dburckh:Media3Avi:2.7.1@aar")
    implementation("com.google.guava:guava:33.3.1-android@jar")
    implementation("com.google.guava:failureaccess:1.0.2@jar")
    implementation(
        "com.google.guava:listenablefuture:9999.0-empty-to-avoid-conflict-with-guava@jar"
    )
    implementation("androidx.collection:collection:1.2.0@jar")
    implementation("androidx.exifinterface:exifinterface:1.3.6@aar")
    implementation("androidx.annotation:annotation:1.6.0")
    implementation("androidx.annotation:annotation-experimental:1.4.0")
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
            "androidx.annotation:annotation:1.6.0",
            "androidx.annotation:annotation-experimental:1.4.0",
            "androidx.arch.core:core-common:2.2.0",
            "androidx.arch.core:core-runtime:2.2.0",
            "androidx.collection:collection:1.2.0",
            "androidx.concurrent:concurrent-futures:1.1.0",
            "androidx.core:core:1.13.1",
            "androidx.core:core-ktx:1.13.1",
            "androidx.exifinterface:exifinterface:1.3.6",
            "com.google.guava:failureaccess:1.0.2",
            "com.google.guava:guava:33.3.1-android",
            "com.google.guava:listenablefuture:9999.0-empty-to-avoid-conflict-with-guava",
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
