# FFmpeg 7.1.1 vendor boundary

This directory contains the static arm64-v8a FFmpeg libraries used by the
campaign movie player.  The platform MPEG-4 decoder on the target device is
Simple Profile only; the campaign AVI files use MPEG-4 Part 2 Advanced Simple
Profile with B-frames.  FFmpeg supplies the missing video decoder while Media3
keeps the AVI audio path and the audio clock.

## Version and license

| Component | Version | License |
|---|---|---|
| FFmpeg | 7.1.1 (`https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`) | LGPL-2.1-or-later |

Source archive SHA-256:
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`

The build used `--disable-gpl --disable-nonfree`, so the libraries are
LGPL-2.1-or-later.  Static linking into `libwc3gameactivity.so` means the
product must keep the corresponding source and relink path available as the
LGPL requires.  `COPYING.LGPLv2.1` and `LICENSE.md` are copied verbatim from the
source release.

## Build configuration

Built with Android NDK 29.0.14206865, target `aarch64-linux-android26`, PIC.
The archives are stripped with `llvm-strip --strip-debug`.  This is the
configure line:

```text
--prefix=<build>/install
--target-os=android --arch=aarch64 --cpu=armv8-a \
--enable-cross-compile \
--sysroot=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/sysroot \
--cc=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe \
--ar=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ar.exe \
--nm=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe \
--ranlib=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ranlib.exe \
--strip=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe \
--extra-cflags='--target=aarch64-linux-android26 -fPIC' \
--extra-ldflags='--target=aarch64-linux-android26' \
--disable-everything \
--enable-avformat --enable-avcodec --enable-avutil --enable-swscale \
--enable-demuxer=avi --enable-decoder=mpeg4 --enable-parser=mpeg4video \
--enable-protocol=file \
--enable-static --disable-shared --disable-programs --disable-doc \
--disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages \
--disable-network --disable-autodetect \
--disable-avdevice --disable-avfilter --disable-postproc \
--disable-gpl --disable-nonfree --disable-iconv --disable-zlib \
--disable-bzlib --disable-lzma --disable-sdl2 --disable-securetransport \
--disable-vulkan --disable-xlib --disable-libxcb --disable-vaapi \
--disable-vdpau --disable-cuda --disable-nvenc --disable-ffnvcodec
```

The resulting enabled paths are the AVI demuxer, the MPEG-4 Part 2 video
decoder and parser, the file protocol, and libswscale.  MP3 audio is
deliberately not linked: Media3/Media3Avi owns audio playback and sync.

## Layout

| Path | Contents |
|---|---|
| `include/` | Public FFmpeg 7.1.1 headers |
| `lib/arm64-v8a/libavformat.a` | AVI demuxer and format layer |
| `lib/arm64-v8a/libavcodec.a` | MPEG-4 Part 2 decoder |
| `lib/arm64-v8a/libswscale.a` | YUV420P to RGBA conversion |
| `lib/arm64-v8a/libavutil.a` | FFmpeg utility layer |

`VENDOR_COMMIT` pins the source archive and every vendored archive by SHA-256.

## Refreshing the vendor

1. Build the same configuration from the pinned source archive.
2. `llvm-strip --strip-debug` each archive.
3. Replace the selected configuration and hashes in `VENDOR_COMMIT`.
4. Update the FFmpeg row in `third_party/VENDOR_MANIFEST.md`.
5. Keep the LGPL text and this build description next to the binaries.
