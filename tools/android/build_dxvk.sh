#!/usr/bin/env bash
# Build the vendored DXVK trees for Android arm64.
#
# The NDK host prebuilt is selected from uname, so the same launcher works on
# Termux, Linux and macOS.  On Windows/MSYS2 it delegates to the canonical
# PowerShell pipeline.  Every compiler call goes through ccache (BP-12).  The
# output layout is identical on every host:
#
#   build/android/dxvk/<variant>/libdxvk1_iso.a
#   build/android/dxvk/<variant>/libdxvk3_iso.a
#   build/android/dxvk/<variant>/libdxvkll_iso.a
#
# Usage:
#   tools/android/build_dxvk.sh --variant debug [--api 26] [--jobs N] [--full]
#                               [--no-deps] [--no-dist]
#
# Exit codes: 0 built, 1 failure, 2 unavailable (SDK/NDK/ccache missing).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

variant=release
api=26
jobs=""
jobs_explicit=0
full=0
no_deps=0
no_dist=0
ndk_arg=""

while [ $# -gt 0 ]; do
    case "$1" in
        --variant) variant=${2:-}; shift 2 ;;
        --variant=*) variant=${1#*=}; shift ;;
        --api) api=${2:-}; shift 2 ;;
        --api=*) api=${1#*=}; shift ;;
        --jobs) jobs=${2:-}; jobs_explicit=1; shift 2 ;;
        --jobs=*) jobs=${1#*=}; jobs_explicit=1; shift ;;
        --full) full=1; shift ;;
        --no-deps) no_deps=1; shift ;;
        --no-dist) no_dist=1; shift ;;
        --ndk) ndk_arg=${2:-}; shift 2 ;;
        --ndk=*) ndk_arg=${1#*=}; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "build_dxvk.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done
case "$variant" in release|debug) ;; *) echo "build_dxvk.sh: bad variant: $variant" >&2; exit 1 ;; esac
if [ "$no_dist" = 1 ]; then
    export WC3_DIST_AUTO=0
fi
. "$root/tools/dist/distcc_env.sh"
if [ "$no_dist" = 1 ] || [ "${WC3_DIST_AUTO:-1}" = 0 ]; then
    wc3_dist_clear_env
fi

if [ "$WC3_IS_WINDOWS" = 1 ]; then
    if [ "$no_deps" = 0 ] &&
       { [ ! -f "$root/build/lift-tools/dxvk_android_probe.exe" ] ||
         wc3_lift_tools_stale; }; then
        "$root/tools/lift/build_tools.sh"
    fi
    ps_args=(-Variant "$variant" -Api "$api")
    [ "$jobs_explicit" = 1 ] && ps_args+=(-Jobs "$jobs")
    [ "$full" = 1 ] && ps_args+=(-Full)
    [ -n "$ndk_arg" ] && ps_args+=(-Ndk "$(wc3_win_arg_path "$(wc3_abs "$ndk_arg")")")
    wc3_delegate_ps1 tools/android/build_dxvk.ps1 "${ps_args[@]}"
    exit $?
fi

ndk=$(wc3_find_ndk "$ndk_arg") || {
    echo "[dxvk] unavailable: Android NDK (${WC3_HOST_TAG:-unknown host}) not found" >&2
    exit 2
}
bin=$(wc3_ndk_bin "$ndk")
sysroot=$(wc3_ndk_sysroot "$ndk")
ccache=$(wc3_find_ccache || true)
if [ -z "$ccache" ]; then
    echo '[dxvk] unavailable: ccache not found (BP-12 requires it)' >&2
    exit 2
fi

# A standalone DXVK target uses the same desktop-PC worker as build_app.sh.
# When build_app.sh already owns the transport this call only inherits its
# environment; it never opens a second tunnel.  Without --jobs the worker's
# slots are added to every local core.
if [ "$no_dist" = 0 ]; then
    WC3_DIST_NDK_ROOT=${WC3_DIST_NDK_ROOT:-$ndk}
    export WC3_DIST_NDK_ROOT
    if wc3_dist_enable "$variant"; then
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

dxvk_root=$root/third_party/dxvk
dxvk1=$dxvk_root/1.10.3
dxvk3=$dxvk_root/3.1.1
dxvk_ll=$dxvk_root/low-latency
shim=$dxvk_root/shim
directx=$dxvk3/include/native/directx
probe=$(wc3_lift_tool dxvk_android_probe)
isolate=$(wc3_lift_tool dxvk_isolate_archive_elf)

if [ ! -f "$probe" ] || [ ! -f "$isolate" ] || wc3_lift_tools_stale; then
    if [ "$no_deps" = 1 ]; then
        echo "[dxvk] unavailable: lift tools missing; run tools/lift/build_tools.sh" >&2
        exit 2
    fi
    "$root/tools/lift/build_tools.sh"
fi

for p in "$dxvk1" "$dxvk3" "$dxvk_ll" "$shim" \
         "$dxvk1/build-win64-ucrt/compile_commands.json" \
         "$dxvk3/build-win64-ucrt/compile_commands.json" \
         "$dxvk_ll/build-win64-ucrt/compile_commands.json" \
         "$dxvk_root/patches/dxvk-3.1.1-wc3-d3d9.patch" \
         "$dxvk_root/patches/dxvk-3.1.1-lowmem-placed.patch" \
         "$dxvk_root/patches/dxvk-1.10.3-lowmem-placed.patch" \
         "$shim/wc3_lowmem.h" "$shim/wc3_vulkan_lowmem.h" \
         "$probe" "$isolate"; do
    [ -e "$p" ] || { echo "[dxvk] vendored input missing: $p" >&2; exit 1; }
done

# ---- source pin checks (same markers as build_dxvk.ps1) -------------------
fail() { echo "[dxvk] source pin check failed: $*" >&2; exit 1; }
need() { grep -Fq -- "$2" "$1" || fail "missing '$2' in $1"; }
forbid() { ! grep -Fq -- "$2" "$1" || fail "unexpected '$2' in $1"; }
need_re() { grep -Eq -- "$2" "$1" || fail "missing /$2/ in $1"; }

feature=$dxvk3/src/dxvk/dxvk_device_info.cpp
cmdlist=$dxvk3/src/dxvk/dxvk_cmdlist.h
options=$dxvk3/src/dxvk/dxvk_options.cpp
options_h=$dxvk3/src/dxvk/dxvk_options.h
device_h=$dxvk3/src/dxvk/dxvk_device_info.h
graphics=$dxvk3/src/dxvk/dxvk_graphics.cpp
context=$dxvk3/src/dxvk/dxvk_context.cpp
shader=$dxvk3/src/dxvk/dxvk_shader.cpp
shader_h=$dxvk3/src/dxvk/dxvk_shader.h
fixed_function=$dxvk3/src/d3d9/d3d9_fixed_function.cpp
memory3=$dxvk3/src/dxvk/dxvk_memory.cpp
presenter3=$dxvk3/src/dxvk/dxvk_presenter.cpp
swapchain3=$dxvk3/src/d3d9/d3d9_swapchain.cpp
sleep_h3=$dxvk3/src/util/util_sleep.h
sleep3=$dxvk3/src/util/util_sleep.cpp

need "$feature" 'storageBuffer8BitAccess, false'
need "$feature" 'khrMaintenance6, maintenance6, false'
need "$feature" 'Ignoring missing required extension feature'
need "$feature" 'm_ignoreMissingExtensions'
need "$feature" 'extMapMemoryPlaced'
need "$cmdlist" 'vkCmdBindDescriptorSets2KHR != nullptr'
need "$cmdlist" 'vkCmdPushConstants2KHR != nullptr'
need "$cmdlist" 'vkCmdBindIndexBuffer2KHR != nullptr'
need "$cmdlist" 'vkCmdSetDepthClipEnableEXT != nullptr'
need "$options" 'WC3_DXVK_VERSION'
need "$options" 'ignoreMissingExtensions'
need "$options" 'useShaderModules'
need "$options_h" 'ignoreMissingExtensions'
need "$options_h" 'useShaderModules'
need "$device_h" 'm_ignoreMissingExtensions'
need "$graphics" 'extDepthClipEnable.depthClipEnable'
need "$context" 'khrLoadStoreOpNone'
need "$shader" 'useShaderModules'
need "$shader" 'vkCreateShaderModule'
need "$shader_h" 'm_modules'
need "$fixed_function" 'must be SPIR-V 1.3'
need "$memory3" 'wc3_lowmem.h'
need "$memory3" 'm_usePlacedMemory'
need "$presenter3" 'if (m_acquireStatus < 0 || !m_swapchain)'
need "$presenter3" 'Presenter: VK_SUBOPTIMAL_KHR (surface is not pre-rotated), keeping swapchain'
forbid "$swapchain3" 'SynchronizePresent'
forbid "$swapchain3" 'm_presentStatus'
need "$sleep_h3" 'defined(_WIN32) && !defined(__ANDROID__)'
need "$sleep3" 'clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME'

for ff in vert frag frag_sample; do
    h=$dxvk3/build-win64-ucrt/src/d3d9/libd3d9.a.p/d3d9_fixed_function_$ff.h
    [ -f "$h" ] || fail "fixed-function header missing: $h"
    need_re "$h" '0x07230203,[[:space:]]*0x00010300'
    need "$h" 'SPIRV_VERSION 0x00010300u'
done

# DXVK 1.10.3 low-memory patch and Android pacing.
need "$dxvk1/src/dxvk/dxvk_memory.cpp" 'm_usePlacedMemory'
need "$dxvk1/src/dxvk/dxvk_memory.cpp" 'wc3_lowmem_acquire'
need "$dxvk1/src/dxvk/dxvk_adapter.cpp" 'extMapMemoryPlaced'
need "$dxvk1/src/dxvk/dxvk_adapter.cpp" 'khrMapMemory2'
need "$dxvk1/src/dxvk/dxvk_options.cpp" 'lowMemoryPlaced'
need "$dxvk1/src/util/util_fps_limiter.cpp" 'clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME'
need "$dxvk1/src/d3d9/d3d9_swapchain.cpp" 'if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)'
need "$dxvk1/src/d3d9/d3d9_swapchain.cpp" 'the frame was displayed, so that result must not destroy'

# Low-latency fork: the same patch set replayed on DXVK 3.1.1 plus upstream's
# frame pacer.  The loader setter and the Android futex path are required here
# because this archive does not link the 1.10.3 Win32 shim.
ll_feature=$dxvk_ll/src/dxvk/dxvk_device_info.cpp
ll_cmdlist=$dxvk_ll/src/dxvk/dxvk_cmdlist.h
ll_options=$dxvk_ll/src/dxvk/dxvk_options.cpp
ll_options_h=$dxvk_ll/src/dxvk/dxvk_options.h
ll_device_h=$dxvk_ll/src/dxvk/dxvk_device_info.h
ll_graphics=$dxvk_ll/src/dxvk/dxvk_graphics.cpp
ll_context=$dxvk_ll/src/dxvk/dxvk_context.cpp
ll_shader=$dxvk_ll/src/dxvk/dxvk_shader.cpp
ll_shader_h=$dxvk_ll/src/dxvk/dxvk_shader.h
ll_memory=$dxvk_ll/src/dxvk/dxvk_memory.cpp
ll_sleep=$dxvk_ll/src/util/util_sleep.cpp
ll_presenter=$dxvk_ll/src/dxvk/dxvk_presenter.cpp
ll_swapchain=$dxvk_ll/src/d3d9/d3d9_swapchain.cpp
ll_fixed=$dxvk_ll/src/d3d9/d3d9_fixed_function.cpp
ll_device=$dxvk_ll/src/d3d9/d3d9_device.cpp
ll_device_h_d3d9=$dxvk_ll/src/d3d9/d3d9_device.h
ll_loader=$dxvk_ll/src/vulkan/vulkan_loader.cpp
ll_sync_signal=$dxvk_ll/src/util/sync/sync_signal.h
ll_sync_atomic=$dxvk_ll/src/util/sync/sync_atomic_signal.h

need "$ll_feature" 'storageBuffer8BitAccess, false'
need "$ll_feature" 'khrMaintenance6, maintenance6, false'
need "$ll_feature" 'm_ignoreMissingExtensions'
need "$ll_feature" 'extMapMemoryPlaced'
need "$ll_cmdlist" 'vkCmdBindDescriptorSets2KHR != nullptr'
need "$ll_cmdlist" 'vkCmdPushConstants2KHR != nullptr'
need "$ll_cmdlist" 'vkCmdBindIndexBuffer2KHR != nullptr'
need "$ll_cmdlist" 'vkCmdSetDepthClipEnableEXT != nullptr'
need "$ll_options" 'lowMemoryPlaced'
need "$ll_options" 'ignoreMissingExtensions'
need "$ll_options_h" 'ignoreMissingExtensions'
need "$ll_device_h" 'm_ignoreMissingExtensions'
need "$ll_options" "forcedVersion[0] == 'l'"
need "$ll_options" 'useShaderModules'
need "$ll_options_h" 'useShaderModules'
need "$ll_shader" 'useShaderModules'
need "$ll_shader" 'vkCreateShaderModule'
need "$ll_shader_h" 'm_modules'
need "$ll_graphics" 'extDepthClipEnable.depthClipEnable'
need "$ll_context" 'khrLoadStoreOpNone'
need "$ll_device" 'm_ffViewValid'
need "$ll_device_h_d3d9" 'm_ffCachedLights'
need "$ll_memory" 'wc3_lowmem.h'
need "$ll_sleep" 'clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME'
need "$ll_presenter" 'keeping swapchain'
need "$ll_presenter" 'm_lastSignaled = frameId'
forbid "$ll_presenter" 'if (m_hasPresentWait) {'
need "$ll_fixed" 'must be SPIR-V 1.3'
need "$ll_loader" 'dxvk_set_vulkan_get_instance_proc_addr'
need "$ll_sync_signal" 'defined(_WIN32) && !defined(__ANDROID__)'
need "$ll_sync_atomic" 'defined(_WIN32) && !defined(__ANDROID__)'
forbid "$ll_swapchain" 'SynchronizePresent'
forbid "$ll_swapchain" 'm_presentStatus'

for ff in vert frag frag_sample; do
    h=$dxvk_ll/build-win64-ucrt/src/d3d9/libd3d9.a.p/d3d9_fixed_function_$ff.h
    [ -f "$h" ] || fail "low-latency fixed-function header missing: $h"
    need_re "$h" '0x07230203,[[:space:]]*0x00010300'
    need "$h" 'SPIRV_VERSION 0x00010300u'
done
for unit in dxvk_hud_item_latency.cpp dxvk_framepacer.cpp \
            dxvk_framepacer_mode_low_latency.cpp \
            dxvk_calibrated_device_timestamps.cpp \
            dxvk_calibrated_swapchain_timestamps.cpp; do
    need "$dxvk_ll/build-win64-ucrt/compile_commands.json" "$unit"
done

# ---- variant flags --------------------------------------------------------
debug=0
[ "$variant" = debug ] && debug=1
debug_flags=()
graphics_debug=0
dxvk_debug=0
if [ "$debug" = 1 ]; then
    debug_flags+=(-DWC3_DEBUG=1)
    if [ -f "$root/config/debug_tags.txt" ]; then
        tags=$(sed 's/#.*//' "$root/config/debug_tags.txt" |
               tr ';,' '\n' | tr -d '[:space:]')
        echo "$tags" | grep -qx GRAPHICS && graphics_debug=1 || true
        echo "$tags" | grep -qx DXVK_DEBUG && dxvk_debug=1 || true
    fi
fi
[ "$graphics_debug" = 1 ] && debug_flags+=(-DWC3_DEBUG_GRAPHICS=1)
[ "$dxvk_debug" = 1 ] && debug_flags+=(-DWC3_DEBUG_DXVK_DEBUG=1)

build_root=$root/build/android/dxvk
obj1=$build_root/obj/1.10.3
obj3=$build_root/obj/3.1.1
objll=$build_root/obj/low-latency
out=$build_root/$variant
mkdir -p "$obj1" "$obj3" "$objll" "$out" "$build_root/tmp"

echo "[dxvk] variant $variant api $api -> $out"
echo "[dxvk] ndk $ndk"
echo "[dxvk] ccache $ccache"
echo "[dxvk] jobs $jobs full $full"

run_probe() {
    local tree=$1 objdir=$2 name=$3
    local args=(--dxvk "$tree" --ndk "$ndk" --api "$api" --objdir "$objdir"
                --codegen --jobs "$jobs" --ccache "$ccache")
    if [ "$full" != 1 ]; then args+=(--stale-only); fi
    echo "[dxvk] compiling $name objects"
    "$probe" "${args[@]}"
}

run_probe "$dxvk1" "$obj1" '1.10.3'
run_probe "$dxvk3" "$obj3" '3.1.1'
run_probe "$dxvk_ll" "$objll" 'low-latency'

objects1=(); objects3=(); objectsll=()
while IFS= read -r f; do objects1+=("$f"); done < <(find "$obj1" -type f -name '*.o' | LC_ALL=C sort)
while IFS= read -r f; do objects3+=("$f"); done < <(find "$obj3" -type f -name '*.o' | LC_ALL=C sort)
while IFS= read -r f; do objectsll+=("$f"); done < <(find "$objll" -type f -name '*.o' | LC_ALL=C sort)
[ "${#objects1[@]}" -ge 100 ] || { echo "[dxvk] only ${#objects1[@]} 1.10.3 objects; rerun with --full" >&2; exit 1; }
[ "${#objects3[@]}" -ge 190 ] || { echo "[dxvk] only ${#objects3[@]} 3.1.1 objects; rerun with --full" >&2; exit 1; }
[ "${#objectsll[@]}" -ge 201 ] || { echo "[dxvk] only ${#objectsll[@]} low-latency objects; rerun with --full" >&2; exit 1; }

# ---- Win32 surface --------------------------------------------------------
win32_src=$shim/dxvk_win32.cpp
win32_obj=$out/dxvk_win32.o
needs_shim=0
if [ "$full" = 1 ] || [ ! -f "$win32_obj" ] || [ "$win32_src" -nt "$win32_obj" ] ||
   [ "$shim/wc3_lowmem.h" -nt "$win32_obj" ]; then
    needs_shim=1
fi
if [ "$needs_shim" = 1 ]; then
    echo "[dxvk] compiling dxvk_win32.cpp ($variant)"
    "$ccache" "$bin/clang++" \
        "--target=aarch64-linux-android$api" "--sysroot=$sysroot" \
        -std=c++17 -O0 -fPIC -fshort-wchar -fdeclspec \
        -DNOMINMAX -D_WIN32_WINNT=0xa00 \
        -I "$shim" -I "$directx" ${debug_flags[@]+"${debug_flags[@]}"} \
        -c "$win32_src" -o "$win32_obj"
fi

# ---- archives and symbol isolation ---------------------------------------
ar=$(wc3_ndk_tool "$ndk" llvm-ar)
archive1=$out/libdxvk1.a
archive3=$out/libdxvk3.a
archivell=$out/libdxvkll.a
iso1=$out/libdxvk1_iso.a
iso3=$out/libdxvk3_iso.a
isoll=$out/libdxvkll_iso.a
rm -f "$archive1" "$archive3" "$archivell" "$iso1" "$iso3" "$isoll"

echo "[dxvk] archiving ${#objects1[@]} 1.10.3 objects"
"$ar" rcs "$archive1" "${objects1[@]}" "$win32_obj"
echo "[dxvk] archiving ${#objects3[@]} 3.1.1 objects"
"$ar" rcs "$archive3" "${objects3[@]}"
echo "[dxvk] archiving ${#objectsll[@]} low-latency objects"
"$ar" rcs "$archivell" "${objectsll[@]}"

export AR=$ar
export NM=$(wc3_ndk_tool "$ndk" llvm-nm)
export OBJCOPY=$(wc3_ndk_tool "$ndk" llvm-objcopy)
export TMPDIR=$build_root/tmp
echo '[dxvk] isolating 1.10.3 symbols'
"$isolate" wc3_dxvk1_ "$iso1" "$archive1"
echo '[dxvk] isolating 3.1.1 symbols'
"$isolate" wc3_dxvk3_ "$iso3" "$archive3"
echo '[dxvk] isolating low-latency symbols'
"$isolate" wc3_dxvkll_ "$isoll" "$archivell"

for a in "$iso1" "$iso3" "$isoll"; do
    [ -f "$a" ] || { echo "[dxvk] archive was not produced: $a" >&2; exit 1; }
    echo "[dxvk] $(basename "$a") $(wc3_file_size "$a") bytes"
done
echo "[dxvk] built ($variant)"
