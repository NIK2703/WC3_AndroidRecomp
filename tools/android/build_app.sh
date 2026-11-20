#!/usr/bin/env bash
# Build libwc3gameactivity.so for Android arm64.
#
# The native half is the same CMake target on every host.  On Termux/Linux/
# macOS the NDK prebuilt is selected automatically; on Windows/MSYS2 the
# launcher delegates to build_app.ps1 so the existing pipeline stays canonical.
# Release is the optimized product: -O3, ThinLTO, IRPGO, Polly and the
# layout/visibility profile.  The PGO training pass is not a variant: use
# --variant release --pgo generate, which writes build/android_app/release-pgo.
# Debug keeps the product code generation: Release build type, -O2, WC3_DEBUG=ON
# and config/debug_tags.txt.
#
# Usage:
#   tools/android/build_app.sh [--variant release|debug] [--pgo off|generate|use]
#                              [--pgo-profile <file>] [--pgo-raw-dir <dir>]
#                              [--corpus build/corpus-full] [--aar <file>]
#                              [--ndk <dir>] [--cmake-args "<args>"] [--no-deps]
#
# --no-deps skips the lift-tools/adrenotools bootstrap.  DXVK is target code,
# not a bootstrap dependency: its archives are always built and verified so a
# patched DXVK source can never be skipped silently.
#
# Release defaults to --pgo use.  Debug rejects any non-off PGO mode.
#
# By default the native job count is derived from the reachable distcc
# workers; WC3_DIST_AUTO=0 forces a local build.  There is no --jobs or
# --no-dist switch.
#
# Exit codes: 0 built, 1 failure, 2 unavailable (SDK/NDK/CMake/AAR).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

variant=debug
pgo=""
pgo_profile=build/pgo/wc3.profdata
pgo_raw_dir=/data/data/wc3.recomp/files/pgo
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
        --pgo) pgo=${2:-}; shift 2 ;;
        --pgo=*) pgo=${1#*=}; shift ;;
        --pgo-profile) pgo_profile=${2:-}; shift 2 ;;
        --pgo-profile=*) pgo_profile=${1#*=}; shift ;;
        --pgo-raw-dir) pgo_raw_dir=${2:-}; shift 2 ;;
        --pgo-raw-dir=*) pgo_raw_dir=${1#*=}; shift ;;
        --corpus) corpus=${2:-}; shift 2 ;;
        --corpus=*) corpus=${1#*=}; shift ;;
        --aar) aar=${2:-}; shift 2 ;;
        --aar=*) aar=${1#*=}; shift ;;
        --ndk) ndk_arg=${2:-}; shift 2 ;;
        --ndk=*) ndk_arg=${1#*=}; shift ;;
        --cmake-args) read -r -a _extra <<<"${2:-}"; cmake_args+=(${_extra[@]+"${_extra[@]}"}); shift 2 ;;
        --cmake-args=*) read -r -a _extra <<<"${1#*=}"; cmake_args+=(${_extra[@]+"${_extra[@]}"}); shift ;;
        --no-deps) no_deps=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "build_app.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done
case "$variant" in release|debug) ;;
    *) echo "build_app.sh: bad variant: $variant" >&2; exit 1 ;;
esac
case "$pgo" in
    '') [ "$variant" = release ] && pgo=use || pgo=off ;;
    off|generate|use) ;;
    *) echo "build_app.sh: bad PGO mode: $pgo" >&2; exit 1 ;;
esac
if [ "$variant" = debug ] && [ "$pgo" != off ]; then
    echo "build_app.sh: --pgo is only valid with --variant release" >&2
    exit 1
fi
# Relative profile paths are relative to the repository root, matching
# build_app.ps1 and build_dist.ps1, not to the caller's current directory.
pgo_profile_path=$pgo_profile
case "$pgo_profile_path" in
    /*|[A-Za-z]:[\\/]*) ;;
    *) pgo_profile_path=$root/$pgo_profile_path ;;
esac
is_profile=0; [ "$pgo" = generate ] && is_profile=1
# The training pass keeps WC3_VARIANT=release but gets its own directory so it
# can never overwrite the final release product.
if [ "$is_profile" = 1 ]; then artifact_variant=release-pgo; else artifact_variant=$variant; fi
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
    [ "$variant" = release ] && ps_args+=(-Pgo "$pgo")
    [ "$pgo" = use ] && ps_args+=(-PgoProfile "$(wc3_win_arg_path "$(wc3_abs "$pgo_profile_path")")")
    [ "$pgo" = generate ] && ps_args+=(-PgoRawDir "$pgo_raw_dir")
    [ "$corpus" != build/corpus-full ] &&
        ps_args+=(-CorpusRoot "$(wc3_win_arg_path "$(wc3_abs "$corpus")")")
    [ -n "$aar" ] &&
        ps_args+=(-GamesActivityAar "$(wc3_win_arg_path "$(wc3_abs "$aar")")")
    # Leave the native job count unset so build_app.ps1 can use every local
    # and distributed slot.
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

out=$root/build/android_app/$artifact_variant
cmake_dir=$out/cmake
ga=$out/gameactivity-aar
# The training pass shares the release DXVK archives: only the native profile
# instrumentation differs, not the renderer.
if [ "$variant" = release ]; then dxvk_variant=release; else dxvk_variant=debug; fi
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
    if [ -z "$aar" ] && [ "$artifact_variant" != "$variant" ]; then
        aar=$(wc3_find_gameactivity_aar \
            "$root/build/android_app/$variant" || true)
    fi
fi
[ -n "$aar" ] && [ -f "$aar" ] || {
    echo '[app] unavailable: games-activity-4.4.2.aar not found; pass --aar' >&2; exit 2; }
aar=$(wc3_abs "$aar")
if [ "$aar" != "$out/games-activity-4.4.2.aar" ]; then
    cp "$aar" "$out/games-activity-4.4.2.aar"
fi
pom=$(wc3_find_gameactivity_pom "$out" || true)
if [ -z "$pom" ] && [ "$artifact_variant" != "$variant" ]; then
    pom=$(wc3_find_gameactivity_pom \
        "$root/build/android_app/$variant" || true)
fi
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

# Start the distcc workers before the dependency builds so DXVK, adrenotools
# and the corpus all share them.  Every worker is optional: an unreachable
# worker leaves the exact same build running locally.
if [ "$WC3_IS_WINDOWS" = 0 ]; then
    WC3_DIST_NDK_ROOT=${WC3_DIST_NDK_ROOT:-$ndk}
    export WC3_DIST_NDK_ROOT
    if wc3_dist_enable "$variant"; then
        trap 'wc3_dist_cleanup' EXIT
        [ -n "$jobs" ] || jobs=$WC3_DIST_JOBS_EFFECTIVE
    else
        # The parent already probed the workers and found none usable.  Keep
        # the local fallback for this invocation and do not make every nested
        # target repeat the same timeout; a standalone target still probes.
        export WC3_DIST_AUTO=0
    fi
fi
if [ -z "$jobs" ] || ! [ "$jobs" -gt 0 ] 2>/dev/null; then
    if [ -n "${WC3_DIST_JOBS:-}" ] && [ "$WC3_DIST_JOBS" -gt 0 ] 2>/dev/null; then
        jobs=$WC3_DIST_JOBS
    else
        jobs=$(wc3_cpu_count)
    fi
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
        "$root/tools/android/build_adrenotools.sh"
    fi
fi
# DXVK is target code, not a bootstrap dependency.  Keep this outside the
# --no-deps block: otherwise a stale libdxvk1_iso.a silently links old D3D9
# code and the Mali feature negotiation never reaches the APK.
"$root/tools/android/build_dxvk.sh" --variant "$dxvk_variant"
for f in libadrenotools.a liblinkernsbypass.a; do
    [ -f "$adrenotools_dir/$f" ] || {
        echo "[app] unavailable: $adrenotools_dir/$f; run tools/android/build_adrenotools.sh" >&2
        exit 2; }
done
for a in libdxvk1_iso.a libdxvk3_iso.a libdxvkll_iso.a; do
    [ -f "$root/build/android/dxvk/$dxvk_variant/$a" ] || {
        echo "[app] unavailable: build/android/dxvk/$dxvk_variant/$a; run tools/android/build_dxvk.sh --variant $dxvk_variant" >&2
        exit 2; }
done
# A present archive is not enough: it must be the WC3-patched 1.10.3 build.
# This is the fast failure for a --no-deps or cached-archive build.
dxvk1_iso=$root/build/android/dxvk/$dxvk_variant/libdxvk1_iso.a
for m in \
    'WC3 DXVK: createDevice begin' \
    'WC3 DXVK: vkCreateDevice result=' \
    'WC3 D3D9: InitialReset' \
    'WC3 D3D9: missing adapter core features'; do
    grep -a -q -F -- "$m" "$dxvk1_iso" || {
        echo "[app] DXVK archive is stale: $dxvk1_iso lacks '$m'" >&2
        echo "[app] rebuild: tools/android/build_dxvk.sh --variant $dxvk_variant --full" >&2
        exit 1; }
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

# Release is the optimized product.  The internal training pass keeps the same
# O3/codegen base but skips LTO and profile-use layout decisions.  Debug keeps
# the conservative profile.
pgo_configure=()
if [ "$variant" = release ]; then
    pgo_kind=ir
    opt_level=O3
    corpus_opt_level=O3
    omit_frame_pointer=ON
    polly=ON
    hidden_visibility=ON
    no_semantic_interposition=ON
    lto=THIN
    lto_opt_level=O3
    lto_whole_program_visibility=ON
    thinlto_cache=ON
    call_graph_sort=cdsort
    split_machine_functions=OFF
    if [ "$pgo" = generate ]; then
        lto=OFF
        lto_whole_program_visibility=OFF
        thinlto_cache=OFF
        call_graph_sort=none
        pgo_configure+=(-DWC3_PGO_MODE=generate "-DWC3_PGO_RAW_DIR=$pgo_raw_dir")
    elif [ "$pgo" = use ]; then
        split_machine_functions=ON
        profile_abs=$(wc3_abs "$pgo_profile_path")
        [ -f "$profile_abs" ] || {
            echo "[app] PGO profile not found: $profile_abs (run --pgo generate first)" >&2
            exit 1; }
        profdata=$(wc3_ndk_tool "$ndk" llvm-profdata)
        [ -x "$profdata" ] || {
            echo "[app] llvm-profdata not found in NDK: $profdata" >&2
            exit 2; }
        level=$("$profdata" show "$profile_abs" 2>/dev/null |
            sed -n 's/.*Instrumentation level:[[:space:]]*//p' | head -n 1 || true)
        case "$level" in
            IR*) actual_pgo_kind=ir ;;
            Front-end*) actual_pgo_kind=instr ;;
            *) actual_pgo_kind="" ;;
        esac
        [ "$actual_pgo_kind" = "$pgo_kind" ] || {
            echo "[app] PGO profile kind '$actual_pgo_kind' does not match '$pgo_kind'" >&2
            exit 1; }
        pgo_configure+=(-DWC3_PGO_MODE=use "-DWC3_PGO_PROFILE=$profile_abs")
    else
        pgo_configure+=(-DWC3_PGO_MODE=off)
    fi
else
    pgo_kind=instr
    opt_level=O2
    corpus_opt_level=O2
    omit_frame_pointer=OFF
    polly=OFF
    hidden_visibility=OFF
    no_semantic_interposition=OFF
    lto=OFF
    lto_opt_level=O2
    lto_whole_program_visibility=OFF
    thinlto_cache=OFF
    call_graph_sort=none
    split_machine_functions=OFF
    pgo_configure+=(-DWC3_PGO_MODE=off)
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
    -DWC3_SELECTION_24=ON -DWC3_SELECTION_STRUCT_CAP=40
    -DWC3_CORPUS_WERROR=OFF
    "-DWC3_CORPUS_JOBS=$jobs"
    -DWC3_CORPUS_RETRIES=5
    "-DWC3_LTO=$lto" "${pgo_configure[@]}" "-DWC3_PGO_KIND=$pgo_kind"
    "-DWC3_OPT_LEVEL=$opt_level" "-DWC3_CORPUS_OPT_LEVEL=$corpus_opt_level" -DWC3_MTUNE=
    "-DWC3_OMIT_FRAME_POINTER=$omit_frame_pointer"
    "-DWC3_SPLIT_MACHINE_FUNCTIONS=$split_machine_functions"
    "-DWC3_CALL_GRAPH_SORT=$call_graph_sort" "-DWC3_LTO_OPT_LEVEL=$lto_opt_level"
    "-DWC3_LTO_WHOLE_PROGRAM_VISIBILITY=$lto_whole_program_visibility"
    "-DWC3_THINLTO_CACHE=$thinlto_cache"
    -DWC3_EMIT_RELOCS=OFF "-DWC3_POLLY=$polly"
    "-DWC3_HIDDEN_VISIBILITY=$hidden_visibility"
    "-DWC3_NO_SEMANTIC_INTERPOSITION=$no_semantic_interposition"
    -DWC3_SYMBOL_ORDER_FILE=
    -DWC3_PACK_DYN_RELOCS=ON -DWC3_PACK_DYN_RELOCS_MODE=android
    -DWC3_NATIVE_TLS=ON -DWC3_TLS_MODEL=global-dynamic
    -DWC3_THIN_SKIP_WRITEONLY_COPYIN=ON -DWC3_HOST_HARDENING=ON
    -DWC3_ICF=ON -DWC3_ICF_MODE=safe -DWC3_ICF_PRINT_SECTIONS=ON
    "-DWC3_ADRENOTOOLS_DIR=$adrenotools_dir"
    "-DWC3_ADRENOTOOLS_TRUST_MANIFEST=$([ -f "$adrenotools_dir/source-build.json" ] && echo ON || echo OFF)"
    "-DWC3_LD_PATH=$ld_path"
)
# The debug package keeps the post-lift pass enabled so it carries the
# WC3_DEBUG_PERF vertex dispatch counter.  The existing parallel paths are
# included by the same pass but remain behind the runtime parallelExecution
# switch, which defaults off.  Release stays on the exact serial corpus.
# Appended before the user --cmake-args so an explicit override still wins.
if [ "$is_debug" = 1 ]; then
    configure+=(-DWC3_PARALLEL_RAYCAST=ON)
fi
[ "${#cmake_args[@]}" -gt 0 ] && configure+=("${cmake_args[@]}")

echo "[app] ndk     $ndk"
echo "[app] linker  ${ld_path:-$bin/ld.lld (NDK default)}"
echo "[app] variant $variant output $artifact_variant pgo $pgo kind $pgo_kind"
echo "[app] debug_tags '$debug_tags'"
echo "[app] corpus  $corpus_abs"
echo "[app] jobs    $jobs"
echo "[app] opt     $opt_level corpus $corpus_opt_level lto $lto polly $polly"
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
    grep -E 'WC3_SAVE_LIST_STRIP_W3Z|WC3_HIDE_CURSOR|WC3_SELECTION_24|WC3_SELECTION_CAP|WC3 DXVK: createDevice begin|WC3 DXVK: vkCreateDevice result=|WC3 D3D9: InitialReset|WC3 D3D9: missing adapter core features' || true)
grep -q WC3_SAVE_LIST_STRIP_W3Z <<<"$so_strings" || {
    echo '[app] save-list marker absent from the product .so' >&2; exit 1; }
grep -q WC3_HIDE_CURSOR <<<"$so_strings" || {
    echo '[app] cursor-hide marker absent from the product .so' >&2; exit 1; }
grep -q WC3_SELECTION_24 <<<"$so_strings" || {
    echo '[app] selection24 marker absent from the product .so' >&2; exit 1; }
grep -q WC3_SELECTION_CAP <<<"$so_strings" || {
    echo '[app] selection-cap runtime marker absent from the product .so' >&2; exit 1; }
# The final link must contain the patched DXVK 1.10.3, not a cached archive.
# This catches --skip-native packaging and any future CMake dependency slip.
for m in \
    'WC3 DXVK: createDevice begin' \
    'WC3 DXVK: vkCreateDevice result=' \
    'WC3 D3D9: InitialReset' \
    'WC3 D3D9: missing adapter core features'; do
    grep -q -F -- "$m" <<<"$so_strings" || {
        echo "[app] DXVK WC3 marker absent from the product .so: $m" >&2
        echo "[app] rebuild DXVK: tools/android/build_dxvk.sh --variant $dxvk_variant --full" >&2
        exit 1; }
done

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
