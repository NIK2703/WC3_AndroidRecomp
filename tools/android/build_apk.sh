#!/usr/bin/env bash
# Build the Android APK from this tree.
#
# Native code is built by build_app.sh/CMake; Gradle only packages the Java
# activity and the already-built libwc3gameactivity.so.  The host prebuilt is
# selected from uname, so the same launcher works on Termux, Linux and macOS.
# On Windows/MSYS2 it delegates to build_apk.ps1, keeping one entry point on a
# PC and in Termux without a second implementation.
#
# Usage:
#   tools/android/build_apk.sh [--variant debug] [--install]
#                              [--launch] [--offline] [--skip-native]
#                              [--aar <file>] [--ndk <dir>]
#                              [--serial <adb-serial>]
#
# By default the native job count is derived from the reachable distcc
# workers; WC3_DIST_AUTO=0 forces a local build.  There is no --jobs or
# --no-dist switch.
#
# Exit codes: 0 built, 1 failure, 2 unavailable (SDK/JDK/Gradle/AAR).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

variant=debug
install=0
launch=0
offline=0
skip_native=0
serial=""
aar_arg=""
ndk_arg=""
while [ $# -gt 0 ]; do
    case "$1" in
        --variant) variant=${2:-}; shift 2 ;;
        --variant=*) variant=${1#*=}; shift ;;
        --install) install=1; shift ;;
        --launch) launch=1; shift ;;
        --offline) offline=1; shift ;;
        --skip-native) skip_native=1; shift ;;
        --serial) serial=${2:-}; shift 2 ;;
        --serial=*) serial=${1#*=}; shift ;;
        --aar) aar_arg=${2:-}; shift 2 ;;
        --aar=*) aar_arg=${1#*=}; shift ;;
        --ndk) ndk_arg=${2:-}; shift 2 ;;
        --ndk=*) ndk_arg=${1#*=}; shift ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "build_apk.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done
case "$variant" in release|debug) ;;
    *) echo "build_apk.sh: bad variant: $variant" >&2; exit 1 ;;
esac
# Probe the configured distcc workers (ideapad and ideacentre) by default.
# WC3_DIST_AUTO=0 is the documented local-build escape hatch; keep it and
# discard any transport inherited from a parent build.
: "${WC3_DIST_AUTO:=1}"
export WC3_DIST_AUTO
. "$root/tools/dist/distcc_env.sh"
if [ "$WC3_DIST_AUTO" = 0 ]; then
    wc3_dist_clear_env
fi
is_debug=0; [ "$variant" = debug ] && is_debug=1
gradle_variant=$variant
gradle_task=assembleDebug
[ "$is_debug" = 0 ] && gradle_task=assembleRelease
[ -n "$aar_arg" ] || aar_arg=${WC3_GAMEACTIVITY_AAR:-}

if [ "$WC3_IS_WINDOWS" = 1 ]; then
    if [ "$skip_native" = 1 ]; then
        echo '[apk] --skip-native is not available through the PowerShell pipeline' >&2
        exit 2
    fi
    if [ -n "${WC3_SDK:-}" ]; then
        sdk_win=$(wc3_win_path "$(wc3_abs "$WC3_SDK")")
        export WC3_SDK=$sdk_win
        export ANDROID_HOME=$sdk_win
        export ANDROID_SDK_ROOT=$sdk_win
    fi
    if [ -n "${WC3_JAVA_HOME:-}" ]; then
        java_win=$(wc3_win_path "$(wc3_abs "$WC3_JAVA_HOME")")
        export WC3_JAVA_HOME=$java_win
        export JAVA_HOME=$java_win
    fi
    if [ -n "$ndk_arg" ]; then
        ndk_win=$(wc3_win_path "$(wc3_abs "$ndk_arg")")
        export WC3_NDK=$ndk_win
        export ANDROID_NDK_HOME=$ndk_win
        export ANDROID_NDK_ROOT=$ndk_win
    elif [ -n "${WC3_NDK:-}" ]; then
        ndk_win=$(wc3_win_path "$(wc3_abs "$WC3_NDK")")
        export WC3_NDK=$ndk_win
        export ANDROID_NDK_HOME=$ndk_win
        export ANDROID_NDK_ROOT=$ndk_win
    fi
    if [ -n "${WC3_GRADLE:-}" ]; then
        export WC3_GRADLE=$(wc3_win_arg_path "$WC3_GRADLE")
    fi
    if [ -n "${WC3_ADRENOTOOLS_DIR:-}" ]; then
        export WC3_ADRENOTOOLS_DIR=$(wc3_win_path "$(wc3_abs "$WC3_ADRENOTOOLS_DIR")")
    fi
    if [ -n "${WC3_GAMEACTIVITY_POM:-}" ]; then
        export WC3_GAMEACTIVITY_POM=$(wc3_win_path "$(wc3_abs "$WC3_GAMEACTIVITY_POM")")
    fi
    if [ ! -f "$root/build/lift-tools/dxvk_android_probe.exe" ] ||
       wc3_lift_tools_stale; then
        "$root/tools/lift/build_tools.sh"
    fi
    ps_args=(-Variant "$variant")
    [ "$install" = 1 ] && ps_args+=(-Install)
    [ "$launch" = 1 ] && ps_args+=(-Launch)
    [ -n "$serial" ] && ps_args+=(-Serial "$serial")
    [ -n "$aar_arg" ] &&
        ps_args+=(-GamesActivityAar "$(wc3_win_arg_path "$(wc3_abs "$aar_arg")")")
    # Leave the native job count unset.  build_app.ps1 then uses
    # CMAKE_BUILD_PARALLEL_LEVEL from its auto-distribution helper, which
    # includes every reachable worker.
    wc3_delegate_ps1 tools/android/build_apk.ps1 "${ps_args[@]}"
    exit $?
fi

sdk=$(wc3_find_sdk) || { echo '[apk] unavailable: Android SDK not found' >&2; exit 2; }
ndk=$(wc3_find_ndk "$ndk_arg") || {
    echo "[apk] unavailable: NDK (${WC3_HOST_TAG:-unknown host}) not found" >&2; exit 2; }
java_home=$(wc3_find_java) || { echo '[apk] unavailable: JDK not found' >&2; exit 2; }
[ -f "$java_home/bin/java" ] || { echo "[apk] unavailable: $java_home/bin/java" >&2; exit 2; }
gradle=$(wc3_find_gradle) || { echo '[apk] unavailable: gradle not found' >&2; exit 2; }
build_tools=$(wc3_find_build_tools "$sdk") || {
    echo '[apk] unavailable: Android build-tools (aapt2/zipalign)' >&2; exit 2; }
aapt2=$build_tools/aapt2$WC3_EXE
zipalign=$build_tools/zipalign$WC3_EXE
adb=$sdk/platform-tools/adb$WC3_EXE
readelf=$(wc3_ndk_tool "$ndk" llvm-readelf)
strip=$(wc3_ndk_tool "$ndk" llvm-strip)
libcxx=$ndk/toolchains/llvm/prebuilt/$WC3_HOST_TAG/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so
python=$(wc3_find_python) || { echo '[apk] unavailable: python3 not found' >&2; exit 2; }
[ -f "$aapt2" ] || { echo "[apk] unavailable: $aapt2" >&2; exit 2; }
[ -f "$zipalign" ] || { echo "[apk] unavailable: $zipalign" >&2; exit 2; }
[ -f "$libcxx" ] || { echo "[apk] unavailable: $libcxx" >&2; exit 2; }

out=$root/build/android_app/$variant
jni_root=$out/jniLibs
jni_dir=$jni_root/arm64-v8a
mkdir -p "$jni_dir"
so_src=$out/cmake/libwc3gameactivity.so

if [ "$skip_native" = 0 ]; then
    app_args=(--variant "$variant")
    [ -n "$ndk_arg" ] && app_args+=(--ndk "$ndk_arg")
    [ -n "$aar_arg" ] && app_args+=(--aar "$aar_arg")
    "$root/tools/android/build_app.sh" "${app_args[@]}" || exit 1
fi
[ -f "$so_src" ] || { echo "[apk] native library missing: $so_src" >&2; exit 1; }
cp "$so_src" "$jni_dir/libwc3gameactivity.so"
jni_so=$jni_dir/libwc3gameactivity.so

# The product link deliberately has no DT_NEEDED libvulkan.so.
if "$readelf" -d "$jni_so" | grep -q 'NEEDED.*libvulkan\.so'; then
    echo '[apk] product .so has DT_NEEDED libvulkan.so; ADR-036 requires the hidden shim' >&2
    exit 1
fi

# Release strips only non-PT_LOAD metadata; .dynsym stays in full so on-device
# crash_sym() resolves L_<va>/wc3_* names.  Debug keeps the unstripped symbols.
# The native build already checked the unstripped artifact.
if [ "$is_debug" = 0 ]; then
    "$strip" --strip-all "$jni_so"
    sections=$("$readelf" -S "$jni_so")
    if ! grep -q '\.dynsym' <<<"$sections" || grep -q '\.symtab' <<<"$sections" ||
       grep -q '\.debug_' <<<"$sections"; then
        echo '[apk] release strip removed .dynsym or kept .symtab/.debug_*' >&2
        exit 1
    fi
    dynsyms=$("$readelf" --dyn-syms "$jni_so")
    if ! grep -q 'Java_wc3_recomp_' <<<"$dynsyms" ||
       ! grep -qw android_main <<<"$dynsyms" ||
       ! grep -qw JNI_OnLoad <<<"$dynsyms"; then
        echo '[apk] release JNI/android_main surface missing from .dynsym' >&2
        exit 1
    fi
    if ! grep -Eq 'L_[0-9A-Fa-f]{8}' <<<"$dynsyms" ||
       ! grep -Eq '(^|[[:space:]])wc3_' <<<"$dynsyms"; then
        echo '[apk] release .dynsym lost L_*/wc3_* crash symbols' >&2
        exit 1
    fi
    echo '[apk] release .dynsym retains L_*/wc3_* for crash symbolization'
fi

# Pinned hooks, copied from the vendor directory.  A previous APK can seed the
# hooks on a clean checkout, matching the old-tree workflow without copying
# binaries into the repository.
hook_main=be1cbb5774d76d1a06be0e2f67d4c11d681f9af9cb8959d5e2a6c0fa285bc103
hook_impl=f95475bd98254c9172c9efa29ced69dc3404631fff96fbb589b4c919e6e9d855
adrenotools=$root/build/android-shared/adrenotools
if [ -n "${WC3_ADRENOTOOLS_DIR:-}" ]; then
    custom=$(wc3_abs "$WC3_ADRENOTOOLS_DIR")
    # Use custom hooks only when they are the pinned bytes.  A custom archive
    # directory may be a read-only legacy tree; never seed it from an APK.
    if [ "$(wc3_sha256 "$custom/libmain_hook.so" 2>/dev/null || true)" = "$hook_main" ] &&
       [ "$(wc3_sha256 "$custom/libhook_impl.so" 2>/dev/null || true)" = "$hook_impl" ]; then
        adrenotools=$custom
    fi
fi
hooks_ok=1
[ "$(wc3_sha256 "$adrenotools/libmain_hook.so" 2>/dev/null || true)" = "$hook_main" ] ||
    hooks_ok=0
[ "$(wc3_sha256 "$adrenotools/libhook_impl.so" 2>/dev/null || true)" = "$hook_impl" ] ||
    hooks_ok=0
old_apk=$root/build/android_app/wc3-$variant.apk
[ -f "$old_apk" ] || old_apk=$root/build/android_app/wc3-debug.apk
if [ "$hooks_ok" = 0 ] && [ -f "$old_apk" ]; then
    # Seed only the two hook DSOs; rebuilding the archives is unnecessary for
    # packaging and would require the git source tree on an offline host.
    mkdir -p "$adrenotools"
    "$python" - "$old_apk" "$adrenotools" <<'PY'
import sys, zipfile
apk, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk) as z:
    for name in ("libmain_hook.so", "libhook_impl.so"):
        with open(out + "/" + name, "wb") as f:
            f.write(z.read("lib/arm64-v8a/" + name))
PY
fi
for pair in "libmain_hook.so=$hook_main" "libhook_impl.so=$hook_impl"; do
    f=${pair%%=*}; want=${pair#*=}
    [ -f "$adrenotools/$f" ] || {
        echo "[apk] unavailable: $adrenotools/$f; run tools/android/build_adrenotools.sh --from-apk <apk>" >&2
        exit 2; }
    got=$(wc3_sha256 "$adrenotools/$f")
    [ "$got" = "$want" ] || { echo "[apk] $f sha256 $got != pinned $want" >&2; exit 1; }
    cp "$adrenotools/$f" "$jni_dir/$f"
done
cp "$libcxx" "$jni_dir/libc++_shared.so"

# Resolve the GameActivity AAR once and keep a local Maven repository next to
# the variant output.  The Gradle cache is only a source, never a required
# network dependency.
if [ -n "$aar_arg" ]; then
    aar=$aar_arg
else
    aar=$(wc3_find_gameactivity_aar "$out" || true)
fi
[ -n "$aar" ] && [ -f "$aar" ] || {
    echo '[apk] unavailable: games-activity-4.4.2.aar; pass --aar' >&2; exit 2; }
aar=$(wc3_abs "$aar")
if [ "$aar" != "$out/games-activity-4.4.2.aar" ]; then
    cp "$aar" "$out/games-activity-4.4.2.aar"
fi
local_maven=$out/local-maven
dest=$local_maven/androidx/games/games-activity/4.4.2
mkdir -p "$dest"
cp "$aar" "$dest/games-activity-4.4.2.aar"
pom=$(wc3_find_gameactivity_pom "$out" || true)
if [ -n "$pom" ]; then
    pom=$(wc3_abs "$pom")
    cp "$pom" "$dest/games-activity-4.4.2.pom"
else
    # The AAR carries no transitive dependency of its own.  Keep the local
    # Maven repository usable even if a source snapshot omitted libs/*.pom.
    cat > "$dest/games-activity-4.4.2.pom" <<'POM'
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion>
  <groupId>androidx.games</groupId>
  <artifactId>games-activity</artifactId>
  <version>4.4.2</version>
  <packaging>aar</packaging>
</project>
POM
fi

gradle_app=$out/gradle-app
gradle_cache=$out/gradle-cache
mkdir -p "$gradle_app" "$gradle_cache"
gradle_args=(
    --no-daemon --console=plain
    --project-cache-dir "$gradle_cache"
    -p "$root/apps/android_gameactivity"
    "-Pwc3GameActivityBuildDir=$gradle_app"
    "-Pwc3JniLibs$([ "$is_debug" = 1 ] && echo Debug || echo Release)=$jni_root"
    "-Pwc3LocalMaven=$local_maven"
    "-Pandroid.aapt2FromMavenOverride=$aapt2"
    ':app:clean' "$gradle_task"
)
[ "$offline" = 1 ] && gradle_args=(--offline "${gradle_args[@]}")

export JAVA_HOME=$java_home
export ANDROID_HOME=$sdk
export ANDROID_SDK_ROOT=$sdk
# Honor a writable custom Gradle home; otherwise use the per-user cache that
# already contains the offline dependency set.
gradle_home=${GRADLE_USER_HOME:-$HOME/.gradle}
if ! mkdir -p "$gradle_home" 2>/dev/null || [ ! -w "$gradle_home" ]; then
    gradle_home=$HOME/.gradle
    mkdir -p "$gradle_home"
fi
export GRADLE_USER_HOME=$gradle_home
export GRADLE_OPTS=${GRADLE_OPTS:--Xmx1536m -Dorg.gradle.native=false -Dfile.encoding=UTF-8}

echo "[apk] gradle  $gradle"
echo "[apk] home    $GRADLE_USER_HOME"
echo "[apk] jdk     $java_home"
echo "[apk] variant $variant"
"$gradle" "${gradle_args[@]}"

apk_dir=$gradle_app/outputs/apk/$gradle_variant
apk=$apk_dir/app-$gradle_variant.apk
[ -f "$apk" ] || apk=$(find "$apk_dir" -name '*.apk' -print 2>/dev/null | head -n 1 || true)
[ -f "$apk" ] || { echo '[apk] no APK produced' >&2; exit 1; }
final=$root/build/android_app/wc3-$variant.apk
cp "$apk" "$final"

"$zipalign" -c -P 16 4 "$final" || { echo '[apk] APK is not 16 KiB page aligned' >&2; exit 1; }

so_hash=$(wc3_sha256 "$jni_so")
"$python" - "$final" "$so_hash" <<'PY' || exit 1
import hashlib, os, sys, zipfile
path, so_hash = sys.argv[1], sys.argv[2]
required = [
    "lib/arm64-v8a/libwc3gameactivity.so",
    "lib/arm64-v8a/libc++_shared.so",
    "lib/arm64-v8a/libmain_hook.so",
    "lib/arm64-v8a/libhook_impl.so",
]
with zipfile.ZipFile(path) as z:
    for name in required:
        try:
            info = z.getinfo(name)
        except KeyError:
            raise SystemExit(f"[apk] APK lacks {name}")
        if info.compress_type != zipfile.ZIP_STORED:
            raise SystemExit(f"[apk] {name} is compressed; extractNativeLibs=false needs stored")
        print(f"[apk] lib     {name} ({info.file_size} bytes, stored)")
        if name == required[0]:
            h = hashlib.sha256(z.read(name)).hexdigest()
            if h != so_hash:
                raise SystemExit("[apk] packaged .so does not match the native build")
    live = sum(i.compress_size for i in z.infolist())
container = os.path.getsize(path)
print(f"[apk] zip     live={live} container={container} overhead={container - live}")
if container - live > 8 * 1024 * 1024:
    raise SystemExit("[apk] APK contains unreferenced padding; stale package rejected")
PY

echo "[apk] apk     $final ($(wc3_file_size "$final") bytes)"

if [ "$install" = 1 ] || [ "$launch" = 1 ]; then
    [ -f "$adb" ] || { echo '[apk] unavailable: adb not found' >&2; exit 2; }
    adb_args=()
    [ -n "$serial" ] && adb_args=(-s "$serial")
    "$adb" ${adb_args[@]+"${adb_args[@]}"} install -r "$final"
    if [ "$launch" = 1 ]; then
        package=wc3.recomp
        [ "$is_debug" = 1 ] && package=wc3.recomp.debug
        # Wake and dismiss the keyguard when the device permits it.  Neither
        # command is required for am start, so older devices may ignore them.
        "$adb" ${adb_args[@]+"${adb_args[@]}"} shell cmd power wakeup >/dev/null 2>&1 || true
        "$adb" ${adb_args[@]+"${adb_args[@]}"} shell wm dismiss-keyguard >/dev/null 2>&1 || true
        "$adb" ${adb_args[@]+"${adb_args[@]}"} shell am start -n "$package/wc3.recomp.Wc3GameActivity"
    fi
fi
echo "[apk] built $variant"
