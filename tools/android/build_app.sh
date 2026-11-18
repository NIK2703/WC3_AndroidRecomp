#!/usr/bin/env bash
# Build libwc3gameactivity.so for Android arm64.
#
# The native half is the same CMake target on every host.  On Termux/Linux/
# macOS the NDK prebuilt is selected automatically; on Windows/MSYS2 the
# launcher delegates to build_app.ps1 so the existing pipeline stays canonical.
# Debug keeps the product code generation: Release build type, -O2, WC3_DEBUG=ON
# and config/debug_tags.txt.
#
# Usage:
#   tools/android/build_app.sh [--variant debug] [--jobs N]
#                              [--corpus build/corpus-full] [--aar <file>]
#                              [--ndk <dir>] [--cmake-args "<args>"] [--no-deps]
#
# Exit codes: 0 built, 1 failure, 2 unavailable (SDK/NDK/CMake/AAR).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

variant=debug
jobs=""
corpus=build/corpus-full
aar=""
ndk_arg=""
no_deps=0
cmake_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --variant) variant=${2:-}; shift 2 ;;
        --variant=*) variant=${1#*=}; shift ;;
        --jobs) jobs=${2:-}; shift 2 ;;
        --jobs=*) jobs=${1#*=}; shift ;;
        --corpus) corpus=${2:-}; shift 2 ;;
        --corpus=*) corpus=${1#*=}; shift ;;
        --aar) aar=${2:-}; shift 2 ;;
        --aar=*) aar=${1#*=}; shift ;;
        --ndk) ndk_arg=${2:-}; shift 2 ;;
        --ndk=*) ndk_arg=${1#*=}; shift ;;
        --cmake-args) read -r -a _extra <<<"${2:-}"; cmake_args+=(${_extra[@]+"${_extra[@]}"}); shift 2 ;;
        --cmake-args=*) read -r -a _extra <<<"${1#*=}"; cmake_args+=(${_extra[@]+"${_extra[@]}"}); shift ;;
        --no-deps) no_deps=1; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "build_app.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done
case "$variant" in release|debug) ;;
    *) echo "build_app.sh: bad variant: $variant" >&2; exit 1 ;;
esac
[ -n "$jobs" ] || jobs=$(wc3_cpu_count)
is_debug=0; [ "$variant" = debug ] && is_debug=1
[ -n "$aar" ] || aar=${WC3_GAMEACTIVITY_AAR:-}

if [ "$WC3_IS_WINDOWS" = 1 ]; then
    if [ -n "${WC3_SDK:-}" ]; then
        sdk_win=$(wc3_win_path "$(wc3_abs "$WC3_SDK")")
        export WC3_SDK=$sdk_win
        export ANDROID_HOME=$sdk_win
        export ANDROID_SDK_ROOT=$sdk_win
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
    if [ -n "${WC3_ADRENOTOOLS_DIR:-}" ]; then
        export WC3_ADRENOTOOLS_DIR=$(wc3_win_path "$(wc3_abs "$WC3_ADRENOTOOLS_DIR")")
    fi
    [ -n "${WC3_CMAKE:-}" ] && export WC3_CMAKE=$(wc3_win_arg_path "$WC3_CMAKE")
    [ -n "${WC3_NINJA:-}" ] && export WC3_NINJA=$(wc3_win_arg_path "$WC3_NINJA")
    if [ "$no_deps" = 0 ] &&
       { [ ! -f "$root/build/lift-tools/dxvk_android_probe.exe" ] ||
         wc3_lift_tools_stale; }; then
        "$root/tools/lift/build_tools.sh"
    fi
    ps_args=(-Variant "$variant")
    [ "$corpus" != build/corpus-full ] &&
        ps_args+=(-CorpusRoot "$(wc3_win_arg_path "$(wc3_abs "$corpus")")")
    [ -n "$aar" ] &&
        ps_args+=(-GamesActivityAar "$(wc3_win_arg_path "$(wc3_abs "$aar")")")
    ps_args+=(-NativeBuildArgs "-j $jobs")
    [ "${#cmake_args[@]}" -gt 0 ] && ps_args+=(-CMakeArgs "${cmake_args[*]}")
    wc3_delegate_ps1 tools/android/build_app.ps1 "${ps_args[@]}"
    exit $?
fi

ndk=$(wc3_find_ndk "$ndk_arg") || {
    echo "[app] unavailable: Android NDK (${WC3_HOST_TAG:-unknown host}) not found" >&2
    exit 2
}
sdk=$(wc3_find_sdk 2>/dev/null || true)
cmake=$(wc3_find_cmake) || { echo '[app] unavailable: cmake not found' >&2; exit 2; }
ninja=$(wc3_find_ninja "$sdk") || { echo '[app] unavailable: ninja not found' >&2; exit 2; }
ccache=$(wc3_find_ccache) || {
    echo '[app] unavailable: ccache not found (BP-12)' >&2; exit 2; }
python=$(wc3_find_python) || { echo '[app] unavailable: python3 not found' >&2; exit 2; }
bin=$(wc3_ndk_bin "$ndk")
toolchain=$ndk/build/cmake/android.toolchain.cmake
[ -f "$toolchain" ] || { echo "[app] unavailable: $toolchain" >&2; exit 2; }

corpus_abs=$(wc3_abs "$corpus")
[ -f "$corpus_abs/corpus_manifest.json" ] || {
    echo "[app] corpus manifest missing: $corpus_abs/corpus_manifest.json" >&2; exit 1; }
[ -f "$corpus_abs/wc3_all_list.h" ] || {
    echo "[app] merged corpus list missing: $corpus_abs/wc3_all_list.h" >&2; exit 1; }

out=$root/build/android_app/$variant
cmake_dir=$out/cmake
ga=$out/gameactivity-aar
if [ -n "${WC3_ADRENOTOOLS_DIR:-}" ]; then
    adrenotools_dir=$(wc3_abs "$WC3_ADRENOTOOLS_DIR")
else
    adrenotools_dir=$root/build/android-shared/adrenotools
fi
mkdir -p "$out" "$cmake_dir"

# Resolve the GameActivity AAR before the expensive dependency builds so a
# clean host fails immediately when the Gradle/Maven cache is empty.
if [ -z "$aar" ]; then
    aar=$(wc3_find_gameactivity_aar "$out" || true)
fi
[ -n "$aar" ] && [ -f "$aar" ] || {
    echo '[app] unavailable: games-activity-4.4.2.aar not found; pass --aar' >&2; exit 2; }
aar=$(wc3_abs "$aar")
if [ "$aar" != "$out/games-activity-4.4.2.aar" ]; then
    cp "$aar" "$out/games-activity-4.4.2.aar"
fi
pom=$(wc3_find_gameactivity_pom "$out" || true)
if [ -n "$pom" ]; then
    pom=$(wc3_abs "$pom")
    if [ "$pom" != "$out/games-activity-4.4.2.pom" ]; then
        cp "$pom" "$out/games-activity-4.4.2.pom"
    fi
fi
if [ ! -d "$ga/prefab" ] || [ "$aar" -nt "$ga/prefab" ]; then
    rm -rf "$ga"; mkdir -p "$ga"
    "$python" -m zipfile -e "$aar" "$ga"
fi

if [ "$no_deps" = 0 ]; then
    if [ ! -f "$(wc3_lift_tool jobpool)" ] ||
       [ ! -f "$(wc3_lift_tool dxvk_android_probe)" ] ||
       wc3_lift_tools_stale; then
        "$root/tools/lift/build_tools.sh"
    fi
    if [ -z "${WC3_ADRENOTOOLS_DIR:-}" ] &&
       { [ ! -f "$adrenotools_dir/libadrenotools.a" ] ||
         [ ! -f "$adrenotools_dir/liblinkernsbypass.a" ]; }; then
        "$root/tools/android/build_adrenotools.sh" --jobs "$jobs"
    fi
    "$root/tools/android/build_dxvk.sh" --variant "$variant" --jobs "$jobs"
fi
for f in libadrenotools.a liblinkernsbypass.a; do
    [ -f "$adrenotools_dir/$f" ] || {
        echo "[app] unavailable: $adrenotools_dir/$f; run tools/android/build_adrenotools.sh" >&2
        exit 2; }
done
for a in libdxvk1_iso.a libdxvk3_iso.a libdxvkll_iso.a; do
    [ -f "$root/build/android/dxvk/$variant/$a" ] || {
        echo "[app] unavailable: build/android/dxvk/$variant/$a; run tools/android/build_dxvk.sh --variant $variant" >&2
        exit 2; }
done

# Parse and validate the debug tag registry exactly like build_app.ps1.
debug_tags=""
if [ "$is_debug" = 1 ]; then
    tag_file=$root/config/debug_tags.txt
    [ -f "$tag_file" ] || { echo "[app] debug tag file missing: $tag_file" >&2; exit 1; }
    declared=$(grep -oE '#define[[:space:]]+WC3_DEBUG_[A-Za-z0-9_]+' \
        "$root/runtime/diag/wc3_debug.h" | sed -E 's/.*WC3_DEBUG_//' | sort -u || true)
    tags=$(sed 's/#.*//' "$tag_file" | tr ';,' '\n' |
        sed 's/[[:space:]]//g' | sed '/^$/d' | tr '[:lower:]' '[:upper:]')
    for t in $tags; do
        if ! grep -qx "$t" <<<"$declared"; then
            echo "[app] unknown debug tag '$t' in $tag_file" >&2; exit 1
        fi
    done
    debug_tags=$(echo "$tags" | paste -sd ';' -)
fi

# NDK r29-aarch64 bundles LLD 21.0.0, which segfaults while computing
# --gc-sections/--icf over the full 200k-function product.  Termux's native
# aarch64 LLD links the same objects and flags.  Other hosts can opt in with
# WC3_LD_PATH; the NDK remains the default everywhere else.
ld_path=${WC3_LD_PATH:-}
if [ -z "$ld_path" ] && [ "$WC3_IS_TERMUX" = 1 ]; then
    if [ -x "${PREFIX:-}/bin/ld.lld" ]; then
        ld_path=${PREFIX:-}/bin/ld.lld
    elif command -v ld.lld >/dev/null 2>&1; then
        ld_path=$(command -v ld.lld)
    fi
fi
if [ -n "$ld_path" ]; then
    [ -x "$ld_path" ] || {
        echo "[app] WC3_LD_PATH is not executable: $ld_path" >&2; exit 2; }
fi

configure=(
    -S "$root" -B "$cmake_dir" -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$ninja"
    "-DWC3_CCACHE=$ccache"
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 "-DANDROID_NDK=$ndk"
    -DCMAKE_BUILD_TYPE=Release
    "-DWC3_VARIANT=$variant"
    "-DWC3_DEBUG=$([ "$is_debug" = 1 ] && echo ON || echo OFF)"
    "-DWC3_DEBUG_TAGS=$debug_tags"
    -DWC3_USE_CCACHE=ON -DWC3_REQUIRE_CCACHE=ON
    -DWC3_BUILD_PLATFORM_TEST=OFF -DWC3_BUILD_APP=ON
    "-DGAMEACTIVITY_ROOT=$ga"
    "-DWC3_RELIFTED_CORPUS_ROOT=$corpus_abs"
    -DWC3_W3L_COMPAT=ON -DWC3_W3L_PROFILE=rubattle -DWC3_W3L_LATENCY=100
    -DWC3_SELECTION_24=ON
    -DWC3_LOG_TO_DOWNLOADS=OFF -DWC3_CORPUS_WERROR=OFF
    "-DWC3_CORPUS_JOBS=$jobs"
    -DWC3_CORPUS_RETRIES=5
    -DWC3_LTO=OFF -DWC3_PGO_MODE=off -DWC3_PGO_KIND=instr
    -DWC3_OPT_LEVEL=O2 -DWC3_CORPUS_OPT_LEVEL=O2 -DWC3_MTUNE=
    -DWC3_OMIT_FRAME_POINTER=OFF -DWC3_SPLIT_MACHINE_FUNCTIONS=OFF
    -DWC3_CALL_GRAPH_SORT=none -DWC3_LTO_OPT_LEVEL=O2
    -DWC3_LTO_WHOLE_PROGRAM_VISIBILITY=OFF -DWC3_THINLTO_CACHE=OFF
    -DWC3_EMIT_RELOCS=OFF -DWC3_POLLY=OFF -DWC3_HIDDEN_VISIBILITY=OFF
    -DWC3_NO_SEMANTIC_INTERPOSITION=OFF -DWC3_SYMBOL_ORDER_FILE=
    -DWC3_PACK_DYN_RELOCS=ON -DWC3_PACK_DYN_RELOCS_MODE=android
    -DWC3_NATIVE_TLS=ON -DWC3_TLS_MODEL=global-dynamic
    -DWC3_THIN_SKIP_WRITEONLY_COPYIN=ON -DWC3_HOST_HARDENING=ON
    -DWC3_ICF=ON -DWC3_ICF_MODE=safe -DWC3_ICF_PRINT_SECTIONS=ON
    "-DWC3_HIDE_DYNSYM=$([ "$is_debug" = 1 ] && echo OFF || echo ON)"
    "-DWC3_ADRENOTOOLS_DIR=$adrenotools_dir"
    "-DWC3_ADRENOTOOLS_TRUST_MANIFEST=$([ -f "$adrenotools_dir/source-build.json" ] && echo ON || echo OFF)"
    "-DWC3_LD_PATH=$ld_path"
)
[ "${#cmake_args[@]}" -gt 0 ] && configure+=("${cmake_args[@]}")

echo "[app] ndk     $ndk"
echo "[app] linker  ${ld_path:-$bin/ld.lld (NDK default)}"
echo "[app] variant $variant debug_tags '$debug_tags'"
echo "[app] corpus  $corpus_abs"
echo "[app] jobs    $jobs"
"$cmake" "${configure[@]}" || { echo '[app] configure failed' >&2; exit 1; }
"$cmake" --build "$cmake_dir" --parallel "$jobs" || {
    echo '[app] build failed' >&2; exit 1; }

so=$cmake_dir/libwc3gameactivity.so
[ -f "$so" ] || so=$(find "$cmake_dir" -name libwc3gameactivity.so -print 2>/dev/null | head -n 1 || true)
[ -f "$so" ] || { echo '[app] build produced no libwc3gameactivity.so' >&2; exit 1; }

readelf=$(wc3_ndk_tool "$ndk" llvm-readelf)
nm=$(wc3_ndk_tool "$ndk" llvm-nm)
strings=$(wc3_ndk_tool "$ndk" llvm-strings)
dynsyms=$("$readelf" --dyn-syms "$so")
for s in android_main JNI_OnLoad Java_com_google_androidgamesdk_GameActivity_initializeNativeCode; do
    grep -qw "$s" <<<"$dynsyms" || { echo "[app] missing symbol: $s" >&2; exit 1; }
done

corpus_archive=$cmake_dir/libwc3_corpus_relifted.a
[ -f "$corpus_archive" ] || {
    echo "[app] selection24 corpus archive missing: $corpus_archive" >&2; exit 1; }
corpus_syms=$("$nm" -u "$corpus_archive")
for s in wc3_sel24_init_group wc3_sel24_note_selection wc3_sel24_select \
         wc3_sel24_hide_buttons wc3_sel24_fill wc3_sel24_cap; do
    grep -qw "$s" <<<"$corpus_syms" || {
        echo "[app] selection24 call site absent from corpus: $s" >&2; exit 1; }
done

fix_report=$root/reports/generated/required_fixes.json
[ -f "$fix_report" ] || { echo "[app] required-fix report missing: $fix_report" >&2; exit 1; }
"$python" - "$fix_report" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
f = d.get("fixes", {})
s = f.get("save_list_display")
if not s or int(s.get("sites", 0)) < 1:
    raise SystemExit("[app] required-fix report lacks the save-list fix")
if f.get("overlapped_save_io") is None:
    raise SystemExit("[app] required-fix report lacks the overlapped save-I/O fix")
PY

so_strings=$("$strings" "$so" |
    grep -E 'WC3_SAVE_LIST_STRIP_W3Z|WC3_HIDE_CURSOR|WC3_SELECTION_24' || true)
grep -q WC3_SAVE_LIST_STRIP_W3Z <<<"$so_strings" || {
    echo '[app] save-list marker absent from the product .so' >&2; exit 1; }
grep -q WC3_HIDE_CURSOR <<<"$so_strings" || {
    echo '[app] cursor-hide marker absent from the product .so' >&2; exit 1; }
grep -q WC3_SELECTION_24 <<<"$so_strings" || {
    echo '[app] selection24 marker absent from the product .so' >&2; exit 1; }

so_wraps=$("$nm" "$so" |
    grep -E '__wrap_L_(6F1A47B0|6F342220)' || true)
for s in __wrap_L_6F1A47B0 __wrap_L_6F342220; do
    grep -qw "$s" <<<"$so_wraps" || {
        echo "[app] cursor-hide wrapper absent: $s" >&2; exit 1; }
done
grep -qw pwrite <<<"$dynsyms" || { echo '[app] pwrite missing' >&2; exit 1; }
grep -qw pread <<<"$dynsyms" || { echo '[app] pread missing' >&2; exit 1; }

echo "[app] so      $so ($(wc3_file_size "$so") bytes)"
echo "[app] syms    android_main/JNI_OnLoad/GameActivity present; selection24 and save fixes verified"
echo "[app] built $variant"
