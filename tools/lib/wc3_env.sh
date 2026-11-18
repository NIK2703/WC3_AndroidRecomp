# wc3_env.sh - host, SDK and tool discovery shared by the POSIX launchers.
#
# Sourced by tools/android/*.sh and tools/lift/build_tools.sh.  Nothing here
# writes to the source tree or changes the caller's shell options.  The same
# script runs on Termux, Linux, macOS, and inside MSYS2/Cygwin; Windows callers
# use wc3_delegate_ps1() where the canonical PowerShell pipeline exists.

WC3_ROOT=${WC3_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}

case "$(uname -s)" in
    Linux*)                 WC3_HOST_OS=linux ;;
    Darwin*)                WC3_HOST_OS=darwin ;;
    MINGW*|MSYS*|CYGWIN*)   WC3_HOST_OS=windows ;;
    *)                      WC3_HOST_OS=unknown ;;
esac
case "$(uname -m)" in
    x86_64|amd64)  WC3_HOST_ARCH=x86_64 ;;
    aarch64|arm64) WC3_HOST_ARCH=aarch64 ;;
    *)             WC3_HOST_ARCH=$(uname -m) ;;
esac
case "$WC3_HOST_OS/$WC3_HOST_ARCH" in
    linux/x86_64)   WC3_HOST_TAG=linux-x86_64 ;;
    linux/aarch64)  WC3_HOST_TAG=linux-aarch64 ;;
    darwin/x86_64)  WC3_HOST_TAG=darwin-x86_64 ;;
    darwin/aarch64) WC3_HOST_TAG=darwin-arm64 ;;
    windows/x86_64) WC3_HOST_TAG=windows-x86_64 ;;
    *)              WC3_HOST_TAG="" ;;
esac

WC3_EXE=
[ "$WC3_HOST_OS" = windows ] && WC3_EXE=.exe
WC3_IS_WINDOWS=0
WC3_IS_TERMUX=0
[ "$WC3_HOST_OS" = windows ] && WC3_IS_WINDOWS=1
if [ -n "${TERMUX_VERSION:-}" ] ||
   [ "${PREFIX:-}" = /data/data/com.termux/files/usr ] ||
   [ -d /data/data/com.termux ]; then
    WC3_IS_TERMUX=1
fi

wc3_cpu_count() {
    if [ -n "${WC3_JOBS:-}" ]; then
        printf '%s\n' "$WC3_JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4\n'
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || printf '4\n'
    else
        printf '4\n'
    fi
}

wc3_posix_path() {
    if [ "$WC3_HOST_OS" = windows ] && command -v cygpath >/dev/null 2>&1; then
        cygpath -u "$1"
    else
        printf '%s\n' "$1"
    fi
}

wc3_win_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    elif command -v wslpath >/dev/null 2>&1; then
        wslpath -w "$1"
    else
        printf '%s\n' "$1"
    fi
}

wc3_win_arg_path() {
    case "$1" in
        /*) wc3_win_path "$1" ;;
        *)  printf '%s\n' "$1" ;;
    esac
}

wc3_abs() {
    local p=$1 d b
    if [ -d "$p" ]; then
        (cd "$p" && pwd -P)
        return
    fi
    d=$(dirname "$p")
    b=$(basename "$p")
    (cd "$d" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$b") || printf '%s\n' "$p"
}

wc3_find_sdk() {
    local c bt
    local -a candidates
    candidates=(
        "${WC3_SDK:-}"
        "${ANDROID_SDK_ROOT:-}"
        "${ANDROID_HOME:-}"
        "${LOCALAPPDATA:+$(wc3_posix_path "$LOCALAPPDATA")/Android/Sdk}"
        "$HOME/Android/Sdk"
        "$HOME/Library/Android/sdk"
        "$HOME/tools/android-sdk"
        "$HOME/android-sdk"
        "/opt/android-sdk"
        "/usr/lib/android-sdk"
        "${PREFIX:-}/opt/android-sdk"
    )
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        c=$(wc3_posix_path "$c")
        [ -d "$c" ] || continue
        if [ -f "$c/platform-tools/adb$WC3_EXE" ]; then
            wc3_abs "$c"
            return 0
        fi
        for bt in "$c"/build-tools/*; do
            if [ -f "$bt/aapt2$WC3_EXE" ]; then
                wc3_abs "$c"
                return 0
            fi
        done
    done
    return 1
}

wc3_find_build_tools() {
    local sdk=$1 c
    local -a candidates
    [ -n "$sdk" ] || return 1
    candidates=("$sdk/build-tools/35.0.0")
    while IFS= read -r c; do
        [ -n "$c" ] && candidates+=("$c")
    done < <(
        for c in "$sdk"/build-tools/*; do
            [ -d "$c" ] && printf '%s\n' "$c"
        done | LC_ALL=C sort -r
    )
    for c in "${candidates[@]}"; do
        [ -d "$c" ] || continue
        if [ -f "$c/aapt2$WC3_EXE" ] && [ -f "$c/zipalign$WC3_EXE" ]; then
            wc3_abs "$c"
            return 0
        fi
    done
    return 1
}

wc3_find_ndk() {
    local explicit=${1:-} c bin
    local -a candidates
    candidates=(
        "$explicit"
        "${WC3_NDK:-}"
        "${ANDROID_NDK_HOME:-}"
        "${ANDROID_NDK_ROOT:-}"
        "${ANDROID_NDK:-}"
        "$HOME/tools/android-ndk-r29"
        "$HOME/android-ndk-r29"
        "$HOME/tools/android-ndk"
        "$HOME/Android/Sdk/ndk"
        "/opt/android-ndk"
    )
    local sdk
    sdk=$(wc3_find_sdk 2>/dev/null || true)
    [ -n "$sdk" ] && candidates+=("$sdk/ndk")
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        c=$(wc3_posix_path "$c")
        [ -d "$c" ] || continue
        if [ -f "$c/toolchains/llvm/prebuilt/$WC3_HOST_TAG/bin/clang$WC3_EXE" ]; then
            wc3_abs "$c"
            return 0
        fi
        # A parent such as <sdk>/ndk contains version directories.  Prefer
        # the newest name when several NDKs are installed.
        while IFS= read -r bin; do
            [ -n "$bin" ] || continue
            if [ -f "$bin/toolchains/llvm/prebuilt/$WC3_HOST_TAG/bin/clang$WC3_EXE" ]; then
                wc3_abs "$bin"
                return 0
            fi
        done < <(
            for bin in "$c"/*; do
                [ -d "$bin" ] && printf '%s\n' "$bin"
            done | LC_ALL=C sort -r
        )
    done
    return 1
}

wc3_ndk_bin() {
    printf '%s/toolchains/llvm/prebuilt/%s/bin\n' "$1" "$WC3_HOST_TAG"
}

wc3_ndk_sysroot() {
    printf '%s/toolchains/llvm/prebuilt/%s/sysroot\n' "$1" "$WC3_HOST_TAG"
}

wc3_ndk_tool() {
    printf '%s/%s%s\n' "$(wc3_ndk_bin "$1")" "$2" "$WC3_EXE"
}

wc3_find_java() {
    local c
    local -a candidates
    candidates=(
        "${WC3_JAVA_HOME:-}"
        "${JAVA_HOME:-}"
        "$HOME/tools/java-17-openjdk"
        "$HOME/tools/java-21-openjdk"
        "${PREFIX:-}/lib/jvm/java-17-openjdk"
        "${PREFIX:-}/lib/jvm/java-21-openjdk"
        "$HOME/.sdkman/candidates/java/current"
        "/Applications/Android Studio.app/Contents/jbr/Contents/Home"
        "$HOME/Applications/Android Studio.app/Contents/jbr/Contents/Home"
        "/usr/lib/jvm/default-java"
        /usr/lib/jvm/java-17-openjdk*
        /usr/lib/jvm/java-21-openjdk*
        "${PREFIX:-}"/lib/jvm/java-17-openjdk*
        "${PREFIX:-}"/lib/jvm/java-21-openjdk*
        "/Library/Java/JavaVirtualMachines/"*/Contents/Home
        "/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home"
    )
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        c=$(wc3_posix_path "$c")
        if [ -f "$c/bin/java$WC3_EXE" ] && [ -f "$c/bin/javac$WC3_EXE" ]; then
            wc3_abs "$c"
            return 0
        fi
    done
    if command -v java >/dev/null 2>&1; then
        c=$(command -v java)
        if command -v readlink >/dev/null 2>&1; then
            c=$(readlink -f "$c" 2>/dev/null || printf '%s' "$c")
        fi
        c=$(dirname "$(dirname "$c")")
        # /usr is the macOS java launcher shim, not a JDK home.
        if [ "$c" != /usr ] && [ "$c" != /usr/local ] &&
           [ -f "$c/bin/java$WC3_EXE" ] && [ -f "$c/bin/javac$WC3_EXE" ]; then
            wc3_abs "$c"
            return 0
        fi
    fi
    return 1
}

wc3_find_gradle() {
    local c f gradle_home
    local -a candidates
    gradle_home=$(wc3_posix_path "${GRADLE_USER_HOME:-$HOME/.gradle}")
    candidates=(
        "${WC3_GRADLE:-}"
        "$HOME/tools/gradle-8.10.2/bin/gradle"
        "$HOME/tools/gradle-8.10.2/bin/gradle.bat"
        "$gradle_home/wrapper/dists/gradle-8.10.2-all"
        "$gradle_home/wrapper/dists/gradle-8.10.2-bin"
        "$gradle_home"/wrapper/dists/gradle-8.10.2*
        "$HOME/.gradle/wrapper/dists/gradle-8.10.2-all"
        "$HOME/.gradle"/wrapper/dists/gradle-8.10.2*
    )
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        c=$(wc3_posix_path "$c")
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
        [ -d "$c" ] || continue
        f=$(find "$c" -type f \( -name gradle -o -name gradle.bat \) -print 2>/dev/null | head -n 1 || true)
        [ -n "$f" ] && { printf '%s\n' "$f"; return 0; }
    done
    if command -v gradle >/dev/null 2>&1; then
        command -v gradle
        return 0
    fi
    if command -v gradle.bat >/dev/null 2>&1; then
        command -v gradle.bat
        return 0
    fi
    return 1
}

wc3_find_cmake() {
    local c f
    local -a candidates
    candidates=("${WC3_CMAKE:-}")
    command -v cmake >/dev/null 2>&1 && candidates+=("$(command -v cmake)")
    local sdk
    sdk=$(wc3_find_sdk 2>/dev/null || true)
    [ -n "$sdk" ] && candidates+=("$sdk"/cmake/*/bin/cmake$WC3_EXE)
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
        if [ -d "$c" ]; then
            for f in cmake cmake.exe; do
                [ -f "$c/$f" ] && { printf '%s\n' "$c/$f"; return 0; }
            done
        fi
    done
    return 1
}

wc3_find_ninja() {
    local sdk=${1:-} c f
    local -a candidates
    candidates=("${WC3_NINJA:-}")
    command -v ninja >/dev/null 2>&1 && candidates+=("$(command -v ninja)")
    [ -n "$sdk" ] && candidates+=("$sdk"/cmake/*/bin/ninja$WC3_EXE)
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
        if [ -d "$c" ]; then
            for f in ninja ninja.exe; do
                [ -f "$c/$f" ] && { printf '%s\n' "$c/$f"; return 0; }
            done
        fi
    done
    return 1
}

wc3_find_gameactivity_aar() {
    local out=${1:-} gradle_home c
    gradle_home=$(wc3_posix_path "${GRADLE_USER_HOME:-$HOME/.gradle}")
    for c in "${WC3_GAMEACTIVITY_AAR:-}" \
             "${out:+$out/games-activity-4.4.2.aar}" \
             "$WC3_ROOT/apps/android_gameactivity/app/libs/games-activity-4.4.2.aar" \
             "$gradle_home/caches/modules-2/files-2.1/androidx.games/games-activity/4.4.2/"*/games-activity-4.4.2.aar \
             "$HOME/.gradle/caches/modules-2/files-2.1/androidx.games/games-activity/4.4.2/"*/games-activity-4.4.2.aar \
             "$HOME/.m2/repository/androidx/games/games-activity/4.4.2/games-activity-4.4.2.aar"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

wc3_find_gameactivity_pom() {
    local out=${1:-} gradle_home c
    gradle_home=$(wc3_posix_path "${GRADLE_USER_HOME:-$HOME/.gradle}")
    for c in "${WC3_GAMEACTIVITY_POM:-}" \
             "${out:+$out/games-activity-4.4.2.pom}" \
             "$WC3_ROOT/apps/android_gameactivity/app/libs/games-activity-4.4.2.pom" \
             "$gradle_home/caches/modules-2/files-2.1/androidx.games/games-activity/4.4.2/"*/games-activity-4.4.2.pom \
             "$HOME/.gradle/caches/modules-2/files-2.1/androidx.games/games-activity/4.4.2/"*/games-activity-4.4.2.pom \
             "$HOME/.m2/repository/androidx/games/games-activity/4.4.2/games-activity-4.4.2.pom"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

wc3_find_capstone() {
    local c py
    for c in "${WC3_CAPSTONE_LIB:-}" "${PREFIX:-}/lib/libcapstone.so" \
             "${PREFIX:-}/lib/libcapstone.dylib" \
             "$HOME/.local/lib/libcapstone.so" "$HOME/.local/lib/libcapstone.dylib" \
             "/usr/local/lib/libcapstone.so" "/usr/local/lib/libcapstone.dylib" \
             "/usr/local/lib64/libcapstone.so" \
             "/opt/homebrew/lib/libcapstone.dylib" \
             "/usr/lib/libcapstone.so" "/usr/lib64/libcapstone.so"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    py=$(wc3_find_python 2>/dev/null || true)
    [ -n "$py" ] || return 1
    "$py" - <<'PY' 2>/dev/null
import os, sys
try:
    import capstone
except Exception:
    raise SystemExit(1)
base = os.path.join(os.path.dirname(capstone.__file__), "lib")
for name in ("libcapstone.so", "libcapstone.dylib"):
    p = os.path.join(base, name)
    if os.path.isfile(p):
        print(p)
        break
else:
    sys.exit(1)
PY
}

wc3_find_ccache() {
    local c
    local -a candidates
    candidates=(
        "${WC3_CCACHE:-}"
        "${CCACHE:-}"
        "${PREFIX:-}/bin/ccache"
        "$HOME/tools/ccache/ccache"
    )
    if command -v ccache >/dev/null 2>&1; then
        candidates+=("$(command -v ccache)")
    fi
    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

wc3_find_compiler() {
    local c
    for c in "${WC3_CLANG:-}" "${WC3_CC:-}" clang gcc cc; do
        [ -n "$c" ] || continue
        if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

wc3_find_python() {
    local c
    for c in "${WC3_PYTHON:-}" python3 python; do
        [ -n "$c" ] || continue
        if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

wc3_sha256() {
    local py
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | sed 's/.* //'
    elif py=$(wc3_find_python 2>/dev/null); then
        "$py" - "$1" <<'PY'
import hashlib, sys
print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())
PY
    else
        return 1
    fi
}

wc3_file_size() {
    wc -c < "$1" | tr -d '[:space:]'
}

wc3_lift_tool() {
    local name=$1 p
    if [ "$WC3_IS_WINDOWS" = 1 ]; then
        p=$WC3_ROOT/build/lift-tools/$name.exe
    else
        p=$WC3_ROOT/build/lift-tools/$name
    fi
    printf '%s\n' "$p"
}

# True when a lift-tool source, the build script, or the generated hook header
# is newer than the probe target.  The probe is a representative product of the
# whole host toolchain, so one timestamp check covers all 48 executables.
wc3_lift_tools_stale() {
    local probe
    probe=$(wc3_lift_tool dxvk_android_probe)
    [ -f "$probe" ] || return 0
    [ -n "$(find "$WC3_ROOT/tools/lift/csrc" "$WC3_ROOT/tools/lift/csrc_local" \
        "$WC3_ROOT/tools/lift/posix_shim" "$WC3_ROOT/tools/lift/build_tools.sh" \
        "$WC3_ROOT/tools/lift/gen_state_hooks.py" "$WC3_ROOT/tools/lib/wc3_env.sh" \
        -type f -newer "$probe" -print 2>/dev/null | head -n 1)" ]
}

wc3_find_powershell() {
    local c
    for c in "${WC3_POWERSHELL:-}" pwsh powershell.exe powershell; do
        [ -n "$c" ] || continue
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

# Run a canonical PowerShell step on Windows/MSYS2.  The POSIX launchers use
# this instead of duplicating Windows-specific path and .exe handling.
wc3_delegate_ps1() {
    local script=$1
    shift
    local ps win_script
    ps=$(wc3_find_powershell) || {
        echo "PowerShell not found; run tools/android/${script##*/} directly" >&2
        return 2
    }
    win_script=$(wc3_win_path "$WC3_ROOT/$script")
    "$ps" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$win_script" "$@"
}
