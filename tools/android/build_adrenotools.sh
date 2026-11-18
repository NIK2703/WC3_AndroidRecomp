#!/usr/bin/env bash
# Build libadrenotools.a / liblinkernsbypass.a for Android arm64 from the
# commits pinned in third_party/libadrenotools/VENDOR_COMMIT.
#
# This replaces the historical opaque legacy archives with a reproducible
# source build.  The NDK host prebuilt is selected from uname, so the same
# launcher works on Termux, Linux, macOS and MSYS2; every compile goes through
# ccache (BP-12).  The two hook DSOs are not built here: the product packages
# the pinned transitional hooks and verifies their SHA-256 in build_apk.sh.
#
# Usage:
#   tools/android/build_adrenotools.sh [--hooks <dir>] [--from-apk <apk>]
#                                      [--jobs N] [--ndk <dir>] [--no-dist]
#
# Output: build/android-shared/adrenotools/{libadrenotools.a,
#         liblinkernsbypass.a,source-build.json}
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

pin=$root/third_party/libadrenotools/VENDOR_COMMIT
out=$root/build/android-shared/adrenotools
src=$root/build/vendor/adrenotools-src
build=$root/build/vendor/adrenotools-out
hooks=""
apk=""
ndk_arg=""
jobs=""
jobs_explicit=0
no_dist=0

while [ $# -gt 0 ]; do
    case "$1" in
        --hooks) hooks=${2:-}; shift 2 ;;
        --hooks=*) hooks=${1#*=}; shift ;;
        --from-apk) apk=${2:-}; shift 2 ;;
        --from-apk=*) apk=${1#*=}; shift ;;
        --jobs) jobs=${2:-}; jobs_explicit=1; shift 2 ;;
        --jobs=*) jobs=${1#*=}; jobs_explicit=1; shift ;;
        --ndk) ndk_arg=${2:-}; shift 2 ;;
        --ndk=*) ndk_arg=${1#*=}; shift ;;
        --no-dist) no_dist=1; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "build_adrenotools.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done
if [ "$no_dist" = 1 ]; then
    export WC3_DIST_AUTO=0
fi
python=$(wc3_find_python) || { echo 'unavailable: python3' >&2; exit 2; }

[ -f "$pin" ] || { echo "missing vendor pin: $pin" >&2; exit 1; }
read -r _tool _url _commit < <(grep '^libadrenotools ' "$pin" | tr -d '\r')
read -r _tool _lurl _lcommit < <(grep '^liblinkernsbypass ' "$pin" | tr -d '\r')
[ -n "${_commit:-}" ] && [ -n "${_lcommit:-}" ] || {
    echo "bad vendor pin: $pin" >&2; exit 1; }

ndk=$(wc3_find_ndk "$ndk_arg") || {
    echo "unavailable: Android NDK (${WC3_HOST_TAG:-unknown host}) not found" >&2
    exit 2
}
cmake=$(wc3_find_cmake) || { echo 'unavailable: cmake' >&2; exit 2; }
ninja=$(wc3_find_ninja "$(wc3_find_sdk 2>/dev/null || true)") || {
    echo 'unavailable: ninja' >&2; exit 2; }
git_bin=$(command -v git) || { echo 'unavailable: git' >&2; exit 2; }
ccache_bin=$(wc3_find_ccache || true)
if [ -z "$ccache_bin" ]; then
    if [ "${WC3_REQUIRE_CCACHE:-1}" = 1 ]; then
        echo 'unavailable: ccache (BP-12)' >&2; exit 2
    fi
    echo 'warning: ccache not found; compiling without it' >&2
fi

. "$root/tools/dist/distcc_env.sh"
if [ "$no_dist" = 1 ] || [ "${WC3_DIST_AUTO:-1}" = 0 ]; then
    wc3_dist_clear_env
fi

# Standalone adrenotools compiles use the same desktop-PC worker as the app.
# The Windows host is local to distccd here, so it keeps its own cores; the
# build_app.sh parent already owns the transport when this is a nested call.
if [ "$WC3_IS_WINDOWS" = 0 ] && [ "$no_dist" = 0 ]; then
    WC3_DIST_NDK_ROOT=${WC3_DIST_NDK_ROOT:-$ndk}
    export WC3_DIST_NDK_ROOT
    if wc3_dist_enable release; then
        trap 'wc3_dist_cleanup' EXIT
        if [ "$jobs_explicit" = 0 ] && [ "${WC3_DIST_JOBS_EFFECTIVE:-0}" -gt 0 ] 2>/dev/null; then
            jobs=$WC3_DIST_JOBS_EFFECTIVE
        fi
    fi
fi
if [ -z "$jobs" ] || ! [ "$jobs" -gt 0 ] 2>/dev/null; then
    if [ -n "${WC3_DIST_JOBS:-}" ] && [ "$WC3_DIST_JOBS" -gt 0 ] 2>/dev/null; then
        jobs=$WC3_DIST_JOBS
    else
        jobs=$(wc3_cpu_count)
    fi
fi

mkdir -p "$out" "$root/build/vendor"
if [ ! -d "$src/.git" ]; then
    rm -rf "$src"
    "$git_bin" clone --quiet "$_url" "$src"
fi
if ! "$git_bin" -C "$src" cat-file -e "$_commit^{commit}" 2>/dev/null; then
    "$git_bin" -C "$src" fetch --quiet origin "$_commit"
fi
"$git_bin" -C "$src" checkout --quiet "$_commit"
mkdir -p "$src/lib"
if [ ! -d "$src/lib/linkernsbypass/.git" ]; then
    rm -rf "$src/lib/linkernsbypass"
    "$git_bin" clone --quiet "$_lurl" "$src/lib/linkernsbypass"
fi
if ! "$git_bin" -C "$src/lib/linkernsbypass" cat-file -e "$_lcommit^{commit}" 2>/dev/null; then
    "$git_bin" -C "$src/lib/linkernsbypass" fetch --quiet origin "$_lcommit"
fi
"$git_bin" -C "$src/lib/linkernsbypass" checkout --quiet "$_lcommit"

launcher=()
if [ -n "$ccache_bin" ]; then
    launcher=("-DCMAKE_C_COMPILER_LAUNCHER=$ccache_bin"
              "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache_bin")
    if [ "$WC3_IS_WINDOWS" = 1 ]; then
        wrapper=$root/build/lift-tools/ccache_wrapper.exe
        if [ -f "$wrapper" ]; then
            launcher=("-DCMAKE_C_COMPILER_LAUNCHER=$wrapper;$ccache_bin"
                      "-DCMAKE_CXX_COMPILER_LAUNCHER=$wrapper;$ccache_bin")
        fi
    fi
fi

rm -rf "$build"
configure=( -S "$src" -B "$build" -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$ninja"
    "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON )
[ "${#launcher[@]}" -gt 0 ] && configure+=("${launcher[@]}")
"$cmake" "${configure[@]}"
"$cmake" --build "$build" --target adrenotools linkernsbypass --parallel "$jobs"

cp "$build/libadrenotools.a" "$build/lib/linkernsbypass/liblinkernsbypass.a" "$out/"
chmod 644 "$out/libadrenotools.a" "$out/liblinkernsbypass.a"

# Hooks: the product still packages the transitional pinned DSOs.  Accept a
# directory or an APK, then verify the exact bytes.
if [ -n "$apk" ]; then
    [ -f "$apk" ] || { echo "APK not found: $apk" >&2; exit 1; }
    "$python" - "$apk" "$out" <<'PY'
import sys, zipfile
apk, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk) as z:
    for name in ("libmain_hook.so", "libhook_impl.so"):
        data = z.read("lib/arm64-v8a/" + name)
        open(out + "/" + name, "wb").write(data)
PY
elif [ -n "$hooks" ]; then
    cp "$hooks/libmain_hook.so" "$hooks/libhook_impl.so" "$out/"
fi

expect_main=be1cbb5774d76d1a06be0e2f67d4c11d681f9af9cb8959d5e2a6c0fa285bc103
expect_impl=f95475bd98254c9172c9efa29ced69dc3404631fff96fbb589b4c919e6e9d855
for pair in "libmain_hook.so=$expect_main" "libhook_impl.so=$expect_impl"; do
    f=${pair%%=*}; want=${pair#*=}
    if [ -f "$out/$f" ]; then
        got=$(wc3_sha256 "$out/$f")
        [ "$got" = "$want" ] || {
            echo "hook hash mismatch: $f $got != $want" >&2; exit 1; }
    else
        echo "note: $f not present yet (needed before build_apk.sh)" >&2
    fi
done

main_hash=$(wc3_sha256 "$out/libadrenotools.a")
linker_hash=$(wc3_sha256 "$out/liblinkernsbypass.a")
ndk_version=$(sed -n 's/^Pkg.Revision *= *//p' "$ndk/source.properties" 2>/dev/null | tr -d '\r' || true)
"$python" - "$out/source-build.json" "$_commit" "$_lcommit" "$ndk_version" \
    "$WC3_HOST_TAG" "$main_hash" "$linker_hash" <<'PY'
import json, sys
path, commit, lcommit, ndk, host, main, linker = sys.argv[1:8]
data = {
    "schema": 1,
    "source": "tools/android/build_adrenotools.sh",
    "libadrenotools_commit": commit,
    "liblinkernsbypass_commit": lcommit,
    "ndk_revision": ndk,
    "host_tag": host,
    "archives": {
        "libadrenotools.a": main,
        "liblinkernsbypass.a": linker,
    },
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY

echo "libadrenotools.a $main_hash"
echo "liblinkernsbypass.a $linker_hash"
echo "adrenotools built in $out"
