#!/data/data/com.termux/files/usr/bin/sh
# distccd worker for the WC3 Android build, started over SSH from
# tools/dist/phone_tunnel.ps1.  It reads worker.env next to this file; that
# file carries the exact Windows compiler paths that distcc sends as argv[0]
# and the Android NDK copy that should answer them.
#
# The script stays in the foreground of the SSH session and waits for the
# distccd child, so closing the tunnel also stops the worker.  A previous
# worker is removed first; the next tunnel start repairs any stale process.
set -u

PREFIX=/data/data/com.termux/files/usr
HOME=/data/data/com.termux/files/home
export PREFIX HOME
PATH="$PREFIX/bin:$PATH"
export PATH
LD_LIBRARY_PATH="$PREFIX/lib"
export LD_LIBRARY_PATH

BASE="$HOME/.wc3-dist"
WORK="$BASE/work"
ENV_FILE="$BASE/worker.env"
SLOTS_FILE="$BASE/slots"
BOOST_HELPER="$BASE/boost.sh"

if [ ! -f "$ENV_FILE" ]; then
    echo "phone_worker: missing $ENV_FILE" >&2
    exit 1
fi
. "$ENV_FILE"

: "${PHONE_NDK:=$HOME/tools/android-ndk-r29}"
: "${CLANG_REL:=E:\\Android\\Sdk\\ndk\\29.0.14206865\\toolchains\\llvm\\prebuilt\\windows-x86_64\\bin\\clang.exe}"
: "${CLANGXX_REL:=E:\\Android\\Sdk\\ndk\\29.0.14206865\\toolchains\\llvm\\prebuilt\\windows-x86_64\\bin\\clang++.exe}"
: "${EXPECTED_REVISION:=}"
: "${BOOST:=1}"
: "${FORCE_JOBS:=0}"
: "${ALLOW_VERSION_MISMATCH:=0}"

NDK_BIN="$PHONE_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

mkdir -p "$WORK" || exit 1
cd "$WORK" || exit 1

if ! command -v distccd >/dev/null 2>&1; then
    echo "phone_worker: distccd missing; running pkg install distcc" >&2
    pkg install -y distcc >/dev/null 2>&1 || {
        echo "phone_worker: pkg install distcc failed" >&2
        exit 1
    }
fi

if [ ! -x "$NDK_BIN/clang" ]; then
    echo "phone_worker: NDK clang not found: $NDK_BIN/clang" >&2
    exit 1
fi

if [ -n "$EXPECTED_REVISION" ] && [ -f "$PHONE_NDK/source.properties" ]; then
    REVISION=$(awk -F' = ' '/^Pkg[.]Revision/ {print $2}' \
        "$PHONE_NDK/source.properties" | head -1)
    if [ -n "$REVISION" ] && [ "$REVISION" != "$EXPECTED_REVISION" ]; then
        if [ "$ALLOW_VERSION_MISMATCH" != "1" ]; then
            echo "phone_worker: NDK revision $REVISION does not match host $EXPECTED_REVISION" >&2
            exit 1
        fi
        echo "phone_worker: warning: NDK revision $REVISION differs from host $EXPECTED_REVISION" >&2
    fi
fi

# distccd executes the compiler path exactly as it arrived from Windows.  A
# file with that literal name in the work directory, plus the work directory
# first on PATH, turns the Windows path into the matching Termux NDK compiler.
write_shim() {
    name="$1"
    compiler="$2"
    cat > "$WORK/$name" <<EOF
#!/data/data/com.termux/files/usr/bin/sh
exec "$compiler" --target=aarch64-none-linux-android26 "\$@"
EOF
    chmod 755 "$WORK/$name"
}
write_shim "$CLANG_REL" "$NDK_BIN/clang"
write_shim "$CLANGXX_REL" "$NDK_BIN/clang++"

# Remove a worker left by an earlier tunnel or a crashed build.  The script
# name does not contain "distccd"; the bracket keeps pkill from matching its
# own command line.
pkill -f '[d]istccd' 2>/dev/null || true
sleep 1
# Acquire the wake lock after the old worker's wait has returned; otherwise
# its termux-wake-unlock can release the lock this worker just took.
command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock >/dev/null 2>&1 || true
rm -f "$SLOTS_FILE" "$BASE/shim.err"
: > "$BASE/distccd.log"

# Capacity: Android gives background apps a small cpuset (usually 0-2).  With
# root, move distccd to the top-app cpuset to reach the big cores.  Without
# root, stay on nproc and keep --jobs honest.
BASE_JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
BASE_JOBS=${BASE_JOBS:-1}
TOP_CPUS=""
TOP_JOBS=0
if [ "$BOOST" = "1" ] && command -v su >/dev/null 2>&1; then
    if command -v timeout >/dev/null 2>&1; then
        TOP_CPUS=$(timeout 10 su -c 'cat /dev/cpuset/top-app/cpus' 2>/dev/null | tr -d '\r')
    else
        TOP_CPUS=$(su -c 'cat /dev/cpuset/top-app/cpus' 2>/dev/null | tr -d '\r')
    fi
    if [ -n "$TOP_CPUS" ]; then
        TOP_JOBS=$(printf '%s' "$TOP_CPUS" | awk -F, '
            { n = 0
              for (i = 1; i <= NF; i++) {
                  if ($i ~ /-/) { split($i, a, "-"); n += a[2] - a[1] + 1 }
                  else if ($i != "") { n++ }
              }
              print n }')
    fi
fi

# Prefer the top-app capacity whenever root could read it, even if nproc
# already reports the same count: Android can shrink the app's cpuset when it
# goes to the background, and the daemon must already be pinned when that
# happens.
JOBS=$BASE_JOBS
WANT_BOOST=0
BOOSTED=0
if [ "$FORCE_JOBS" -gt 0 ] 2>/dev/null; then
    JOBS=$FORCE_JOBS
    [ -n "$TOP_CPUS" ] && WANT_BOOST=1
elif [ "$TOP_JOBS" -gt 0 ] 2>/dev/null; then
    JOBS=$TOP_JOBS
    WANT_BOOST=1
fi

# Root helper: move the daemon and its preforked workers to top-app.  New
# compiler children inherit the cgroup of their worker parent.
cat > "$BOOST_HELPER" <<'EOF'
#!/system/bin/sh
for p in $(ps -ef | awk '/distccd --no-detach/ && !/awk/ {print $2}'); do
    echo "$p" > /dev/cpuset/top-app/cgroup.procs 2>/dev/null
done
EOF
chmod 755 "$BOOST_HELPER"

export TMPDIR="$WORK"
export PATH="$WORK:$PATH"

DAEMON=""
start_daemon() {
    "$PREFIX/bin/distccd" --no-detach --listen 127.0.0.1 --port 3632 \
        --allow 127.0.0.1 --enable-tcp-insecure --jobs "$JOBS" \
        --log-file="$BASE/distccd.log" --log-level=info &
    DAEMON=$!
    i=0
    while [ "$i" -lt 60 ]; do
        if ! kill -0 "$DAEMON" 2>/dev/null; then
            return 1
        fi
        if grep -q 'listening on 127.0.0.1:3632' "$BASE/distccd.log" 2>/dev/null; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

stop_daemon() {
    if [ -n "$DAEMON" ]; then
        kill "$DAEMON" 2>/dev/null || true
        wait "$DAEMON" 2>/dev/null || true
        DAEMON=""
    fi
}

apply_boost() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 15 su -c "sh $BOOST_HELPER" >/dev/null 2>&1 || true
    else
        su -c "sh $BOOST_HELPER" >/dev/null 2>&1 || true
    fi
}

allowed_count() {
    allowed=$(awk '/Cpus_allowed_list/ {print $2}' "/proc/$DAEMON/status" 2>/dev/null)
    printf '%s' "$allowed" | awk -F, '
        { n = 0
          for (i = 1; i <= NF; i++) {
              if ($i ~ /-/) { split($i, a, "-"); n += a[2] - a[1] + 1 }
              else if ($i != "") { n++ }
          }
          print n }'
}

if ! start_daemon; then
    echo "phone_worker: distccd failed to start" >&2
    stop_daemon
    exit 1
fi

FALLBACK_JOBS=$BASE_JOBS
if [ "$FORCE_JOBS" -gt 0 ] 2>/dev/null && \
   [ "$FORCE_JOBS" -lt "$BASE_JOBS" ] 2>/dev/null; then
    FALLBACK_JOBS=$FORCE_JOBS
fi

if [ "$WANT_BOOST" = "1" ]; then
    apply_boost
    ALLOWED_JOBS=$(allowed_count)
    ALLOWED_JOBS=${ALLOWED_JOBS:-0}
    if [ "$ALLOWED_JOBS" -gt "$BASE_JOBS" ] 2>/dev/null; then
        BOOSTED=1
        # The top-app cpuset can advertise more CPUs than are actually online
        # (for example 0-7 with one core offline), so trust the daemon's
        # Cpus_allowed_list rather than the cpuset text.
        if [ "$ALLOWED_JOBS" -lt "$JOBS" ] 2>/dev/null; then
            JOBS=$ALLOWED_JOBS
            echo "phone_worker: top-app allows $JOBS cpus; reporting $JOBS slots" >&2
        fi
    else
        echo "phone_worker: cpuset boost did not widen the daemon (allowed=$ALLOWED_JOBS, base=$BASE_JOBS)" >&2
        if [ "$JOBS" -ne "$FALLBACK_JOBS" ] 2>/dev/null; then
            echo "phone_worker: restarting with $FALLBACK_JOBS jobs" >&2
            stop_daemon
            JOBS=$FALLBACK_JOBS
            if ! start_daemon; then
                echo "phone_worker: distccd failed to restart" >&2
                stop_daemon
                exit 1
            fi
        fi
    fi
fi

echo "$JOBS" > "$SLOTS_FILE"
echo "WC3_PHONE_SLOTS=$JOBS"
echo "WC3_PHONE_BOOSTED=$BOOSTED"
echo "phone_worker: ready jobs=$JOBS boosted=$BOOSTED cpus=${TOP_CPUS:-unknown} allowed=$(allowed_count)" >&2

trap 'termux-wake-unlock >/dev/null 2>&1 || true; stop_daemon; exit 0' HUP INT TERM
wait "$DAEMON"
termux-wake-unlock >/dev/null 2>&1 || true
