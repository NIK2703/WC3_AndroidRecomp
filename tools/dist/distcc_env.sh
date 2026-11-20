#!/usr/bin/env bash
# Auto-enable the distcc workers for the POSIX build entries.
#
# Dot-source this file, then call wc3_dist_enable <variant>.  It starts every
# enabled worker independently: the ideapad laptop (usually 12 cores) and the
# ideacentre desktop (usually 4).  Each worker is resolved by MAC through
# pc_resolve.py, reached through its own SSH forward from pc_tunnel.sh, and
# contributes one loopback term to DISTCC_HOSTS plus its slot count to the
# total Ninja job count.  Each worker reports its own CPU count unless a
# WC3_*_SLOTS override is set.  The caller owns cleanup and must call
# wc3_dist_cleanup, normally from an EXIT trap.
#
# Environment toggles:
#   WC3_DIST_AUTO=0          force a local build
#   WC3_DIST=1               the caller already owns the transport
#   WC3_DIST_ACTIVE=1        a parent build already started the transport
#   WC3_DIST_WORKERS=LIST    explicit list, e.g. "ideapad" or "pc,ideapad";
#                            takes precedence over the per-worker toggles
#   WC3_DIST_IDEAPAD=0       disable the laptop worker when the list is unset
#   WC3_DIST_PC=0            disable the desktop worker when the list is unset
#   WC3_DIST_JOBS=N          override the total Ninja job count
#   WC3_DIST_LOCAL_SLOTS=N   local compiler slots (default: all local CPUs)
#   WC3_DIST_STATE_DIR=DIR   tunnel/state directory (default: ~/.wc3-dist)
#   WC3_PC_*                 passed through to the pc worker
#   WC3_IDEAPAD_*            passed through to the laptop worker
#
# Neither worker is required: a missing host, key or daemon is not an error,
# and the build still runs with the workers that are reachable (or locally).

WC3_DIST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

wc3_dist_warn() {
    printf '[dist] %s\n' "$*" >&2
}

wc3_dist_clear_env() {
    # A --no-dist/auto-disabled invocation must not inherit a transport that a
    # parent build exported.  Keep user knobs such as WC3_DIST_JOBS and a
    # custom CCACHE_PREFIX; only remove the settings this project owns.
    case "${CCACHE_PREFIX:-}" in
        *wc3_dist_cc.py*) unset CCACHE_PREFIX ;;
    esac
    unset DISTCC_HOSTS DISTCC_DIR WC3_DIST WC3_DIST_ACTIVE \
        WC3_DIST_TRANSPORT WC3_DIST_JOBS_EFFECTIVE WC3_DISTCC_EXE \
        WC3_DIST_SHIM_DIR WC3_DIST_REMOTE_BASENAME WC3_DIST_REMOTE_NDK \
        WC3_DIST_ALLOW_LTO WC3_DIST_ALLOW_PGO \
        WC3_DIST_PGO_PROFILE_READY CMAKE_BUILD_PARALLEL_LEVEL
}

wc3_dist_cpu_count() {
    # The launchers source wc3_env.sh, but keep this helper usable on its own.
    # macOS has no nproc; use the same fallbacks as wc3_cpu_count so every
    # available core is counted on every POSIX host.
    if declare -F wc3_cpu_count >/dev/null 2>&1; then
        wc3_cpu_count
        return 0
    fi
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || printf '1\n'
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n'
    else
        printf '1\n'
    fi
}

wc3_dist_local_ndk() {
    local c tag=${WC3_HOST_TAG:-}
    if [ -z "$tag" ]; then
        case "$(uname -s 2>/dev/null || echo unknown)/$(uname -m 2>/dev/null || echo unknown)" in
            Darwin/arm64|Darwin/aarch64) tag=darwin-arm64 ;;
            Darwin/x86_64|Darwin/amd64)  tag=darwin-x86_64 ;;
            Linux/aarch64|Linux/arm64)   tag=linux-aarch64 ;;
            Linux/x86_64|Linux/amd64)    tag=linux-x86_64 ;;
            Linux/armv7l|Linux/armv8l)   tag=linux-armv7a ;;
            *)                           tag=linux-$(uname -m 2>/dev/null || echo unknown) ;;
        esac
    fi
    for c in "${WC3_DIST_NDK_ROOT:-}" "${WC3_NDK:-}" \
             "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
             "$HOME/tools/android-ndk-r29" "$HOME/android-ndk-r29"; do
        [ -n "$c" ] || continue
        if [ -f "$c/toolchains/llvm/prebuilt/$tag/bin/clang" ]; then
            printf '%s\n' "$c"
            return 0
        fi
    done
    return 1
}

wc3_dist_unset_worker_state() {
    # pc_tunnel.sh prints the same WC3_PC_* contract for every worker.  Clear
    # it before each start so a failed worker cannot reuse the previous one's
    # port or slot count.
    unset WC3_PC_STATUS WC3_PC_WORKER WC3_PC_TARGET WC3_PC_USER \
        WC3_PC_SSH_PORT WC3_PC_PORT WC3_PC_LOCAL_PORT WC3_PC_SLOTS \
        WC3_PC_LOCAL_SLOTS WC3_PC_SSH_PID WC3_PC_CONFIG WC3_PC_NDK \
        WC3_PC_REMOTE_NDK WC3_PC_NDK_REVISION WC3_PC_DISTCCD \
        WC3_PC_DISTCCD_VERSION WC3_PC_STARTED WC3_PC_OWNER_PID
}

wc3_dist_worker_list() {
    # WC3_DIST_WORKERS is an explicit comma/space separated list.  Without it
    # every configured worker is tried independently.  IDEAPAD is first
    # because it usually has the most cores; the desktop remains the fallback.
    local w
    local seen=""
    if [ -n "${WC3_DIST_WORKERS:-}" ]; then
        for w in ${WC3_DIST_WORKERS//,/ }; do
            case "$w" in
                pc|ideacentre|desktop) w=pc ;;
                ideapad|laptop) w=ideapad ;;
                *)
                    wc3_dist_warn "unknown worker '$w' in WC3_DIST_WORKERS; skipping"
                    continue
                    ;;
            esac
            case " $seen " in
                *" $w "*) ;;
                *) seen="$seen $w"; printf '%s\n' "$w" ;;
            esac
        done
        return 0
    fi
    if [ "${WC3_DIST_IDEAPAD:-1}" != 0 ]; then
        printf 'ideapad\n'
    fi
    if [ "${WC3_DIST_PC:-1}" != 0 ]; then
        printf 'pc\n'
    fi
}

wc3_dist_enable() {
    local variant=${1:-release}
    local inherited_jobs=${WC3_DIST_JOBS_EFFECTIVE:-}
    WC3_DIST_JOBS_EFFECTIVE=0
    WC3_DIST_TUNNEL_OWNED=0
    WC3_DIST_OWNED_WORKERS=

    if [ "${WC3_DIST_AUTO:-1}" = 0 ]; then
        return 1
    fi
    if [ "${WC3_DIST_ACTIVE:-0}" = 1 ] || [ "${WC3_DIST:-0}" = 1 ]; then
        # A parent process (or an explicit caller) already installed the
        # transport.  Never start a second tunnel or stop the parent's.
        if [ -n "$inherited_jobs" ] && [ "$inherited_jobs" -gt 0 ] 2>/dev/null; then
            WC3_DIST_JOBS_EFFECTIVE=$inherited_jobs
        elif [ -n "${WC3_DIST_JOBS:-}" ]; then
            WC3_DIST_JOBS_EFFECTIVE=$WC3_DIST_JOBS
        else
            WC3_DIST_JOBS_EFFECTIVE=$(wc3_dist_cpu_count 2>/dev/null || echo 1)
        fi
        export WC3_DIST_JOBS_EFFECTIVE
        return 0
    fi

    local root distcc ccache python tunnel local_ndk local_slots local_slots_cap
    root=$WC3_DIST_ROOT
    distcc=$(command -v distcc 2>/dev/null || true)
    ccache=$(command -v ccache 2>/dev/null || true)
    python=$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)
    tunnel=$root/tools/dist/pc_tunnel.sh
    if [ -z "$distcc" ] || [ -z "$ccache" ] || [ -z "$python" ] || [ ! -x "$tunnel" ]; then
        wc3_dist_warn 'distcc, ccache, python or pc_tunnel.sh missing; building locally'
        return 1
    fi
    case "${CCACHE_PREFIX:-}" in
        *wc3_dist_cc.py*) ;;
        '') ;;
        *)
            wc3_dist_warn 'CCACHE_PREFIX is already set; building locally (set WC3_DIST=1 to own the transport)'
            return 1
            ;;
    esac

    local_slots=${WC3_DIST_LOCAL_SLOTS:-0}
    if ! [ "$local_slots" -gt 0 ] 2>/dev/null; then
        local_slots=$(wc3_dist_cpu_count 2>/dev/null || echo 1)
    fi
    [ "$local_slots" -ge 1 ] 2>/dev/null || local_slots=1
    local_slots_cap=$(wc3_dist_cpu_count 2>/dev/null || echo "$local_slots")
    [ "$local_slots" -le "$local_slots_cap" ] 2>/dev/null || local_slots=$local_slots_cap
    local_ndk=$(wc3_dist_local_ndk 2>/dev/null || true)
    if [ -n "$local_ndk" ]; then
        export WC3_DIST_NDK_ROOT=$local_ndk
    fi

    local -a workers=() worker_names=() worker_ports=() worker_slots=()
    local -a worker_targets=() worker_ndks=()
    local w out total_remote=0 hosts=""
    while IFS= read -r w; do
        [ -n "$w" ] && workers+=("$w")
    done < <(wc3_dist_worker_list)
    if [ ${#workers[@]} -eq 0 ]; then
        wc3_dist_warn 'no distributed worker is enabled; building locally'
        return 1
    fi

    for w in "${workers[@]}"; do
        wc3_dist_unset_worker_state
        out=""
        # The tunnel runs in a command substitution, so tell it the build
        # shell owns the tunnel; a second build must refuse instead of
        # replacing it.
        if ! out=$(WC3_PC_OWNER_PID=$$ "$tunnel" start --worker "$w" \
                --quiet --local-slots "$local_slots"); then
            wc3_dist_warn "$w worker unavailable; skipping (set WC3_DIST_DEBUG=1 for details)"
            continue
        fi
        # The tunnel prints shell-quoted assignments; only its own known keys
        # are read, and pc_tunnel.sh is the sole producer of this text.
        eval "$out"
        if [ "${WC3_PC_STATUS:-}" != ready ] ||
           [ -z "${WC3_PC_LOCAL_PORT:-}" ] || [ -z "${WC3_PC_SLOTS:-}" ]; then
            wc3_dist_warn "$w worker returned no usable state; skipping"
            "$tunnel" stop --worker "$w" --quiet >/dev/null 2>&1 || true
            continue
        fi
        # pc_tunnel.sh caps the local slot count to this host; use its value
        # so DISTCC_HOSTS and the Ninja limit agree with the daemon's state.
        local_slots=${WC3_PC_LOCAL_SLOTS:-$local_slots}
        worker_names+=("$w")
        worker_ports+=("$WC3_PC_LOCAL_PORT")
        worker_slots+=("$WC3_PC_SLOTS")
        worker_targets+=("${WC3_PC_TARGET:-?}")
        worker_ndks+=("${WC3_PC_REMOTE_NDK:-${WC3_PC_NDK:-}}")
        WC3_DIST_OWNED_WORKERS="$WC3_DIST_OWNED_WORKERS $w"
    done

    if [ ${#worker_names[@]} -eq 0 ]; then
        wc3_dist_warn 'no distributed worker available; building locally'
        return 1
    fi

    local i jobs state_dir
    for i in "${!worker_names[@]}"; do
        hosts+="127.0.0.1:${worker_ports[i]}/${worker_slots[i]} "
        total_remote=$((total_remote + worker_slots[i]))
    done
    jobs=${WC3_DIST_JOBS:-0}
    if ! [ "$jobs" -gt 0 ] 2>/dev/null; then
        jobs=$((local_slots + total_remote))
    fi
    state_dir=${WC3_DIST_STATE_DIR:-$HOME/.wc3-dist}
    mkdir -p "$state_dir/tmp"
    export WC3_DIST=1
    export WC3_DIST_ACTIVE=1
    export WC3_DIST_TRANSPORT=distcc
    export WC3_DIST_LOCAL_SLOTS=$local_slots
    export WC3_DIST_JOBS_EFFECTIVE=$jobs
    export WC3_DIST_ALLOW_LTO=1
    export WC3_DIST_ALLOW_PGO=1
    export WC3_DIST_PGO_PROFILE_READY=1
    export WC3_DISTCC_EXE=$distcc
    export WC3_DIST_REMOTE_BASENAME=1
    export WC3_DIST_REMOTE_NDK=${worker_ndks[0]:-}
    export WC3_DIST_REMOTE_NDK=${WC3_DIST_REMOTE_NDK//\\//}
    export WC3_DIST_STATE_DIR=$state_dir
    export WC3_DIST_SHIM_DIR=$state_dir/shims
    export CCACHE_PREFIX="$python $root/tools/dist/wc3_dist_cc.py"
    export DISTCC_HOSTS="$hosts localhost/$local_slots --localslots=$local_slots --localslots_cpp=$local_slots"
    export DISTCC_DIR="$state_dir/distcc-state-$$-$(date +%s)"
    export TMPDIR="$state_dir/tmp"
    export CMAKE_BUILD_PARALLEL_LEVEL=$jobs
    mkdir -p "$DISTCC_DIR"
    WC3_DIST_TUNNEL_OWNED=1

    printf '[dist] workers=%s local=%s remote=%s jobs=%s variant=%s\n' \
        "${worker_names[*]}" "$local_slots" "$total_remote" "$jobs" "$variant" >&2
    for i in "${!worker_names[@]}"; do
        printf '[dist] %s target=%s port=%s slots=%s\n' \
            "${worker_names[i]}" "${worker_targets[i]}" \
            "${worker_ports[i]}" "${worker_slots[i]}" >&2
    done
    if [ "${WC3_DIST_DEBUG:-0}" = 1 ]; then
        printf '[dist] hosts=%s\n[dist] remote_ndk=%s\n' \
            "$DISTCC_HOSTS" "$WC3_DIST_REMOTE_NDK" >&2
    fi
    return 0
}

wc3_dist_cleanup() {
    local state_dir=${WC3_DIST_STATE_DIR:-$HOME/.wc3-dist}
    local w
    if [ "${WC3_DIST_TUNNEL_OWNED:-0}" = 1 ]; then
        for w in ${WC3_DIST_OWNED_WORKERS:-}; do
            "$WC3_DIST_ROOT/tools/dist/pc_tunnel.sh" stop --worker "$w" --quiet \
                >/dev/null 2>&1 || true
        done
        if [ -n "${DISTCC_DIR:-}" ] && [ "${WC3_DIST_KEEP_STATE:-0}" != 1 ]; then
            case "$DISTCC_DIR" in
                "$state_dir"/distcc-state-*) rm -rf "$DISTCC_DIR" ;;
            esac
        fi
    fi
    WC3_DIST_TUNNEL_OWNED=0
    WC3_DIST_OWNED_WORKERS=
}
