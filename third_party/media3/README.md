# Media3 / Media3Avi offline vendor

This directory contains the Android media stack used by the campaign movie
player.  The player no longer parses AVI samples or drives `MediaCodec` itself:
Media3 owns AVI audio demuxing, the playback clock and A/V synchronisation, and
the Media3Avi extractor supplies the AVI container support that Android's
`MediaExtractor` does not have.  The MPEG-4 Part 2 Advanced Simple Profile
video stream is decoded by the vendored FFmpeg path, not by `MediaCodec`.

## Versions

| Component | Version | Upstream revision | License |
|---|---|---|---|
| AndroidX Media3 | 1.7.1 | `androidx/media` tag `1.7.1`, commit `5d42f5b5527c3e8d1a3e6ec155fc7c263bab3dd9` | Apache-2.0 |
| Media3Avi | 2.7.1 | `dburckh/Media3Avi` tag `2.7.1`, commit `23936fefaf7bd32976d93f42079ca915a2078f2c` | MIT |
| Guava | 33.3.1-android | Maven Central artifact | Apache-2.0 |
| AndroidX Collection | 1.2.0 | Google Maven artifact | Apache-2.0 |
| AndroidX ExifInterface | 1.3.6 | Google Maven artifact | Apache-2.0 |

The AAR/JAR files are binary upstream artifacts.  No vendor source was patched
in this tree.  The complete artifact list and SHA-256 pins are in
[`VENDOR_COMMIT`](VENDOR_COMMIT).

## Why Media3Avi

The campaign movies are RIFF/AVI files with MPEG-4 Part 2 video (`XVID`) and
MP3 audio.  Android has no AVI extractor in `MediaExtractor`, and Media3's
built-in AVI support is intentionally limited.  Media3Avi adds the missing
demuxer and emits the display-order timestamps used by the audio/clock side.
The video track is disabled in ExoPlayer: the target device only exposes
`c2.android.mpeg4.decoder`, a software Simple Profile decoder, while these
movies are Advanced Simple Profile with B-frames.  FFmpeg decodes that stream
and presents it through `ANativeWindow`; Media3Avi is not used for video
decode.

`libVLC` was evaluated and rejected for this use: its AAR is about 87 MB before
native extraction, it adds about 52 MB of arm64 libraries plus its own
`libc++_shared.so`, and it pulls `libEGL`/`libGLESv2` into a product whose
graphics path is D3D9 via DXVK.

## Offline build layout

`repo/` is a minimal local Maven repository with the exact fixed versions.  Its
POM files intentionally declare no transitive dependencies; every needed
artifact is listed explicitly in
`apps/android_gameactivity/app/build.gradle.kts` and pinned by
`resolutionStrategy.force`.  This keeps the canonical `--offline` build
independent of a Maven cache descriptor or a network fetch.

The AAR/JAR copies in the directory root are the downloaded upstream files;
`repo/` is the copy consumed by Gradle.  `VENDOR_COMMIT` hashes both.

## Refreshing the vendor

1. Download the fixed versions from Google Maven, Maven Central and JitPack or
   GitHub Packages.
2. Verify the upstream license for each new component and place its text next
   to this file.
3. Recreate the `repo/<group>/<artifact>/<version>/` layout and the minimal POM
   files.
4. Replace the hashes in `VENDOR_COMMIT` from `Get-FileHash <path> -Algorithm
   SHA256` (lower-case hex).
5. Update the Media3/Media3Avi row in `third_party/VENDOR_MANIFEST.md`.
