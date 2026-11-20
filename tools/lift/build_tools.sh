#!/usr/bin/env bash
# Build the host-side lift toolchain.
#
# The target list and defines match tools/lift/build_tools.ps1.  On POSIX the
# pinned sources compile against tools/lift/posix_shim; on Windows/MSYS2 this
# launcher delegates to the canonical PowerShell script so the same command
# works on a PC and in Termux.
#
#   tools/lift/build_tools.sh [-Force] [-Smoke]
#
# Environment:
#   WC3_CCACHE / WC3_CLANG / WC3_PYTHON   explicit tool overrides
#   WC3_REQUIRE_CCACHE=0                   allow a local no-ccache run
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/tools/lib/wc3_env.sh"

force=0
smoke=0
while [ $# -gt 0 ]; do
    case "$1" in
        -Force|--force) force=1; shift ;;
        -Smoke|--smoke) smoke=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "build_tools.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [ "$WC3_IS_WINDOWS" = 1 ]; then
    ps_args=()
    [ "$force" = 1 ] && ps_args+=(-Force)
    [ "$smoke" = 1 ] && ps_args+=(-Smoke)
    wc3_delegate_ps1 tools/lift/build_tools.ps1 "${ps_args[@]}"
    exit $?
fi

csrc=$root/tools/lift/csrc
local=$root/tools/lift/csrc_local
shim=$root/tools/lift/posix_shim
bin=$root/build/lift-tools
obj=$bin/obj
report=$root/reports/generated/lift_tools.json
python=$(wc3_find_python) || { echo "build_tools.sh: python not found" >&2; exit 2; }
clang_bin=$(wc3_find_compiler) || {
    echo "build_tools.sh: clang/gcc not found" >&2; exit 2; }
ccache_bin=$(wc3_find_ccache || true)

cc=()
if [ -n "$ccache_bin" ]; then
    cc=("$ccache_bin")
elif [ "${WC3_REQUIRE_CCACHE:-1}" = 1 ]; then
    echo "build_tools.sh: ccache not found; BP-12 requires it" >&2
    exit 2
else
    echo "build_tools.sh: warning: ccache not found; compiling without it" >&2
fi

if [ ! -f "$shim/windows.h" ]; then
    echo "build_tools.sh: POSIX shim missing: $shim/windows.h" >&2
    exit 1
fi
mkdir -p "$bin" "$obj" "$(dirname "$report")"
"$python" "$root/tools/lift/gen_state_hooks.py" --out "$bin/state_hooks_gen.h"
capstone=$(wc3_find_capstone || true)
if [ -n "$capstone" ]; then
    ln -sfn "$capstone" "$bin/libcapstone.so"
else
    echo "build_tools.sh: warning: libcapstone.so not found; decode-based tools need it" >&2
fi

inc=(-I"$local" -I"$csrc" -I"$shim" -I"$root/third_party/capstone/include" -I"$bin")
# glibc hides mkdtemp/clock_gettime/strdup behind feature-test macros when
# -std=c11 is strict.  Termux's bionic exposes them regardless, so ask for the
# same POSIX surface explicitly on every host.
flags=(-D_GNU_SOURCE -O2 -Wall -Wextra -std=c11 -Wno-unused-parameter -pthread)
built_targets=()

run_compiler() {
    if [ "${#cc[@]}" -gt 0 ]; then
        "${cc[@]}" "$clang_bin" "$@"
    else
        "$clang_bin" "$@"
    fi
}

build_target() {
    local out=$1 def=${2:-} srcs=$3
    local -a objects=()
    local s o i=0
    for s in $srcs; do
        o=$obj/$(basename "$out" .exe)_${i}_$(basename "$s" .c).o
        if [ -n "$def" ]; then
            run_compiler "${flags[@]}" -D"$def" "${inc[@]}" -c "$s" -o "$o"
        else
            run_compiler "${flags[@]}" "${inc[@]}" -c "$s" -o "$o"
        fi
        objects+=("$o")
        i=$((i + 1))
    done
    run_compiler "${flags[@]}" "${inc[@]}" "${objects[@]}" -o "$bin/$out"
    built_targets+=("$out")
    echo "built: $out"
}

core="$csrc/liftcore.c $csrc/liftutil.c $csrc/liftemit.c $csrc/liftflags.c $csrc/csglue.c"

build_target triage.exe      "" "$csrc/triage.c"
build_target probe.exe       "" "$csrc/probe.c"
build_target expand.exe      "" "$csrc/expand.c"
build_target manifest.exe    "" "$csrc/manifest.c"
build_target lint.exe        "" "$csrc/lint.c"
build_target map.exe         "" "$csrc/map.c"
build_target addr.exe        "" "$csrc/addr.c"
build_target scan.exe        "" "$csrc/scan.c"
build_target ledger.exe      "" "$csrc/ledger.c"
build_target stack.exe       "" "$csrc/stack.c"
build_target watch.exe       "" "$csrc/watch.c"
build_target refs.exe        "" "$csrc/refs.c"
build_target entry.exe       "" "$csrc/entry.c"
build_target hle.exe         "" "$csrc/hle.c"
build_target bmp2png.exe     "" "$csrc/bmp2png.c"
build_target bmpinfo.exe     "" "$csrc/bmpinfo.c"
build_target lift.exe        "" "$csrc/lift.c"
build_target known.exe       "" "$csrc/known.c"
build_target lift32.exe      "" "$csrc/lift32.c $core"
build_target seed.exe        "" "$csrc/seed.c $core"
build_target closure.exe     "WC3_NO_LIFT_MAIN" "$csrc/closure.c $csrc/lift32.c $core"
build_target closure2.exe    "WC3_NO_LIFT_MAIN" "$local/closure2.c $csrc/lift32.c $core"
build_target cstest.exe      "" "$csrc/cstest.c $csrc/csglue.c"
build_target corpus.exe      "" "$local/corpus.c $local/sha256.c $local/dirwalk.c"
build_target liftpass.exe    "" "$local/liftpass.c $local/sha256.c $local/dirwalk.c"
build_target atomiccheck.exe "" "$local/atomiccheck.c $local/dirwalk.c"
build_target devirtualize.exe "" "$local/devirtualize.c"
build_target gen_boot_dispatch.exe "" "$local/gen_boot_dispatch.c"
build_target save_list_display.exe "" "$local/save_list_display.c $local/sha256.c"
build_target dead_flags.exe  "" "$local/dead_flags.c $local/sha256.c"
build_target xmm_local.exe   "" "$local/xmm_local.c"
build_target xmm_lane.exe    "" "$local/xmm_lane.c"
build_target leaf_call.exe   "" "$local/leaf_call.c"
build_target parallel_raycast.exe "" "$local/parallel_raycast.c"
build_target return_address_elide.exe "" "$local/return_address_elide.c $local/sha256.c"
build_target w3l_compat.exe  "" "$local/w3l_compat.c $local/sha256.c"
build_target selection.exe "" "$local/selection.c $local/sha256.c"
build_target ccache_wrapper.exe "" "$local/ccache_wrapper.c"
build_target dxvk_isolate_archive_elf.exe "" "$local/dxvk_isolate_archive_elf.c"
build_target flag_read_audit.exe "" "$local/flag_read_audit.c"
build_target audit_tls.exe   "" "$local/audit_tls.c $local/sha256.c"
build_target dxvk_android_probe.exe "" "$local/dxvk_android_probe.c"
build_target closure_pipeline.exe "" "$local/closure_pipeline.c $local/sha256.c $local/dirwalk.c"
build_target recomp_ratio.exe "" "$local/recomp_ratio.c $local/sha256.c $local/dirwalk.c $csrc/csglue.c"
build_target thunkseeds.exe  "" "$local/thunkseeds.c $csrc/csglue.c"
build_target corpusinput.exe "" "$local/corpusinput.c $local/sha256.c $local/dirwalk.c"
build_target corpusclosure.exe "" "$local/corpusclosure.c $local/sha256.c $local/dirwalk.c"
build_target splitcorpus.exe "" "$local/splitcorpus.c $local/dirwalk.c"
build_target jobpool.exe     "" "$local/jobpool.c"

# corpusclosure and closure_pipeline invoke their helpers with the .exe suffix
# the Windows pipeline uses.  Keep both spellings available on POSIX.
for t in closure_pipeline corpus known; do
    ln -sf "$t.exe" "$bin/$t"
done
for f in "$bin"/*.exe; do
    t=$(basename "$f" .exe)
    [ -e "$bin/$t" ] || ln -s "$(basename "$f")" "$bin/$t"
done

if [ "$smoke" = 1 ]; then
    for t in parallel_raycast dxvk_isolate_archive_elf flag_read_audit audit_tls dxvk_android_probe jobpool; do
        echo "smoke: $t"
        "$bin/$t.exe" --self-test
    done
fi

compiler_version=$("$clang_bin" --version | head -n 1)
"$python" - "$report" "$bin" "$compiler_version" "${built_targets[@]}" <<'PY'
import hashlib, json, os, sys
report, bindir, compiler, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
tools = []
for name in names:
    path = os.path.join(bindir, name)
    data = open(path, "rb").read()
    tools.append({"name": name, "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest()})
with open(report, "w", encoding="utf-8") as f:
    json.dump({"schema": 1, "compiler": compiler, "tools": tools}, f, indent=2)
    f.write("\n")
PY

echo "lift tools built in $bin"
