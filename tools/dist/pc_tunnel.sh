#!/usr/bin/env bash
# Manage the distcc worker on the desktop PC from this POSIX host.
#
# The PC address is never stored as a fixed IPv4 address.  The worker is
# identified by its Ethernet MAC, resolved to the current IPv6 link-local (or
# IPv4) neighbour entry on every start; an explicit WC3_PC_HOST/WC3_PC_IPV6
# override is available for diagnostics.  The transport is the same one used
# by tools/dist/distcc_tunnel.ps1: a local SSH forward to a distccd bound to
# 127.0.0.1 on the PC.
#
# Usage:
#   tools/dist/pc_tunnel.sh resolve [--json]
#   tools/dist/pc_tunnel.sh setup-key
#   tools/dist/pc_tunnel.sh start [--slots N] [--local-slots N] [--ndk <dir>]
#                                 [--force] [--quiet] [--json]
#   tools/dist/pc_tunnel.sh status [--json]
#   tools/dist/pc_tunnel.sh stop [--keep-remote] [--quiet]
#
# State and logs live in ${WC3_DIST_STATE_DIR:-$HOME/.wc3-dist}.  The start
# action prints shell assignments by default (WC3_PC_PORT, WC3_PC_SLOTS, ...)
# and JSON with --json.  Exit codes: 0 ok, 1 failure, 3 worker unavailable,
# 4 a live tunnel is already owned by another process.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
state_dir=${WC3_DIST_STATE_DIR:-$HOME/.wc3-dist}
state_file=$state_dir/pc-tunnel.env
config_file=$state_dir/pc-ssh.conf
probe_file=$state_dir/pc-probe.ps1
daemon_file=$state_dir/pc-daemon.cmd
remote_dir=${WC3_PC_REMOTE_DIR:-wc3-dist}
alias_name=wc3-pc-distcc
ssh_user=${WC3_PC_USER:-nikita}
ssh_port=${WC3_PC_SSH_PORT:-22}
remote_port=${WC3_PC_DISTCCD_PORT:-3632}
key=${WC3_PC_KEY:-$HOME/.ssh/id_wc3_pc}
mac=${WC3_PC_MAC:-d0:37:45:3b:03:b4}
name=${WC3_PC_NAME:-ideacentre}
# Keep explicit overrides separate from values read back from a previous state
# file; a stale state must not pin the slot count or compiler path.
pc_slots_env=${WC3_PC_SLOTS:-}
pc_local_slots_env=${WC3_DIST_LOCAL_SLOTS:-}
pc_distccd_env=${WC3_PC_DISTCCD:-}
pc_remote_ndk_env=${WC3_PC_REMOTE_NDK:-}
pc_owner_env=${WC3_PC_OWNER_PID:-}

case "$state_dir" in
    ''|/) printf '[pc-dist] WC3_DIST_STATE_DIR must be a private directory\n' >&2; exit 1 ;;
esac
mkdir -p "$state_dir"
chmod 700 "$state_dir" 2>/dev/null || true

info() {
    [ "${quiet:-0}" = 1 ] || printf '[pc-dist] %s\n' "$*" >&2
}

warn() {
    printf '[pc-dist] warning: %s\n' "$*" >&2
}

die() {
    printf '[pc-dist] %s\n' "$*" >&2
    exit "${2:-1}"
}

case "$remote_dir" in
    ''|*[!A-Za-z0-9._-]*)
        die 'WC3_PC_REMOTE_DIR must be a simple relative name (letters, digits, . _ -)'
        ;;
esac
case "$ssh_user" in
    ''|*[!A-Za-z0-9._-]*)
        die 'WC3_PC_USER contains characters that are not safe for an SSH user name'
        ;;
esac
case "$ssh_port" in
    ''|*[!0-9]*) die 'WC3_PC_SSH_PORT must be a port number' ;;
esac
case "$remote_port" in
    ''|*[!0-9]*) die 'WC3_PC_DISTCCD_PORT must be a port number' ;;
esac
[ "$ssh_port" -ge 1 ] && [ "$ssh_port" -le 65535 ] ||
    die 'WC3_PC_SSH_PORT is outside 1..65535'
[ "$remote_port" -ge 1 ] && [ "$remote_port" -le 65535 ] ||
    die 'WC3_PC_DISTCCD_PORT is outside 1..65535'

python=$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)
ssh_bin=${WC3_PC_SSH:-$(command -v ssh 2>/dev/null || true)}
scp_bin=${WC3_PC_SCP:-$(command -v scp 2>/dev/null || true)}
ssh_copy_id=$(command -v ssh-copy-id 2>/dev/null || true)

[ -n "$python" ] || die 'python3 not found'
[ -n "$ssh_bin" ] || die 'ssh not found'
[ -n "$scp_bin" ] || die 'scp not found'

cpu_count() {
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

free_port() {
    "$python" - <<'PY'
import socket
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

port_open() {
    "$python" - "$1" <<'PY'
import socket, sys
try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5):
        pass
except OSError:
    raise SystemExit(1)
PY
}

resolve_target() {
    if [ -n "${WC3_PC_HOST:-}" ]; then
        printf '%s\n' "$WC3_PC_HOST"
        return 0
    fi
    if [ -n "${WC3_PC_IPV6:-}" ]; then
        printf '%s\n' "$WC3_PC_IPV6"
        return 0
    fi
    if [ -n "${WC3_PC_IPV4:-}" ]; then
        printf '%s\n' "$WC3_PC_IPV4"
        return 0
    fi
    local json address
    if ! json=$("$python" "$root/tools/dist/pc_resolve.py" \
            --mac "$mac" --name "$name" --port "$ssh_port" --json 2>"$state_dir/pc-resolve.err"); then
        warn "could not resolve the PC by MAC $mac: $(tail -n 2 "$state_dir/pc-resolve.err" 2>/dev/null | tr '\n' ' ')"
        return 3
    fi
    address=$("$python" -c 'import json,sys; print(json.load(sys.stdin)["address"])' <<<"$json")
    [ -n "$address" ] || return 3
    printf '%s\n' "$address"
}

write_ssh_config() {
    local target=$1 escaped
    # ssh expands % in HostName; a literal link-local zone separator is %%.
    escaped=${target//%/%%}
    umask 077
    cat > "$config_file" <<EOF
Host $alias_name
    HostName $escaped
    Port $ssh_port
    User $ssh_user
    IdentityFile $key
    IdentitiesOnly yes
    BatchMode yes
    StrictHostKeyChecking accept-new
    HostKeyAlias wc3-pc
    LogLevel ERROR
    WarnWeakCrypto no
    ServerAliveInterval 15
    ServerAliveCountMax 4
    RequestTTY no
EOF
    chmod 600 "$config_file" 2>/dev/null || true
}

ssh_run() {
    "$ssh_bin" -F "$config_file" -o BatchMode=yes \
        -o ConnectTimeout="${WC3_PC_CONNECT_TIMEOUT:-8}" "$alias_name" "$@"
}

remote_ok() {
    ssh_run 'cmd /c exit 0' >/dev/null 2>&1
}

ensure_key() {
    if [ -f "$key" ] && [ ! -f "$key.pub" ]; then
        ssh-keygen -y -f "$key" > "$key.pub" 2>/dev/null ||
            die "cannot read the public key from $key; create $key.pub or remove the private key"
    fi
    if [ -f "$key" ] && [ -f "$key.pub" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$key")"
    chmod 700 "$(dirname "$key")" 2>/dev/null || true
    info "generating SSH key $key"
    ssh-keygen -q -t ed25519 -N '' -f "$key" \
        -C "wc3-dist@$(uname -n 2>/dev/null || echo termux)" ||
        die "ssh-keygen failed; create $key manually"
}

install_key_powershell() {
    local target=$1 encoded
    encoded=$("$python" - "$(cat "$key.pub")" <<'PY'
import base64
import sys

key = sys.argv[1].replace("'", "''")
script = """$ErrorActionPreference='Stop'
$key = '%s'
function Add-Key([string]$File, [bool]$Admin) {
    $dir = Split-Path -Parent $File
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not (Test-Path -LiteralPath $File)) {
        New-Item -ItemType File -Path $File | Out-Null
    }
    $lines = @(Get-Content -LiteralPath $File -ErrorAction SilentlyContinue)
    if ($lines -notcontains $key) {
        Add-Content -LiteralPath $File -Value $key -Encoding ascii
    }
    if ($Admin) {
        icacls $File /inheritance:r /grant:r "*S-1-5-32-544:(F)" "*S-1-5-18:(F)" | Out-Null
    } else {
        icacls $File /inheritance:r /grant:r "$($env:USERNAME):(R)" "*S-1-5-18:(R)" | Out-Null
    }
    if ($LASTEXITCODE -ne 0) { throw "icacls failed for $File" }
}
$installed = $false
$adminDir = ''
if ($env:ProgramData) { $adminDir = Join-Path $env:ProgramData 'ssh' }
if ($adminDir -and (Test-Path -LiteralPath $adminDir)) {
    try {
        Add-Key (Join-Path $adminDir 'administrators_authorized_keys') $true
        $installed = $true
    } catch {}
}
if ($env:USERPROFILE) {
    try {
        Add-Key (Join-Path $env:USERPROFILE '.ssh\\authorized_keys') $false
        $installed = $true
    } catch {}
}
if (-not $installed) { exit 1 }
exit 0
""" % key
print(base64.b64encode(script.encode("utf-16le")).decode("ascii"))
PY
)
    [ -n "$encoded" ] || return 1
    info 'ssh-copy-id did not work; trying a PowerShell key install (enter the PC password again)'
    "$ssh_bin" -p "$ssh_port" -o BatchMode=no \
        -o StrictHostKeyChecking=accept-new -o HostKeyAlias=wc3-pc \
        "$ssh_user@$target" \
        "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
}

read_state() {
    [ -f "$state_file" ] || return 1
    # shellcheck disable=SC1090
    . "$state_file"
    return 0
}

pid_alive() {
    [ -n "${1:-}" ] && [ "$1" -gt 0 ] 2>/dev/null && kill -0 "$1" 2>/dev/null
}

pid_is_tunnel() {
    local pid=${1:-}
    pid_alive "$pid" || return 1
    if [ -r "/proc/$pid/cmdline" ]; then
        cat "/proc/$pid/cmdline" 2>/dev/null | tr '\0' ' ' | grep -Fq "$alias_name"
    elif command -v ps >/dev/null 2>&1; then
        ps -p "$pid" -o command= 2>/dev/null | grep -Fq "$alias_name"
    else
        return 0
    fi
}

write_probe() {
    cat > "$probe_file" <<'PS1'
$ErrorActionPreference = 'SilentlyContinue'
function Test-Ndk([string]$Path) {
    if (-not $Path) { return $false }
    return (Test-Path -LiteralPath (Join-Path $Path 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'))
}
$candidates = New-Object System.Collections.Generic.List[string]
foreach ($value in @($env:ANDROID_NDK_ROOT, $env:ANDROID_NDK_HOME)) {
    if ($value) { $candidates.Add($value) }
}
$sdkRoots = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME,
    (Join-Path $env:LOCALAPPDATA 'Android\Sdk'), 'C:\Android\Sdk',
    'D:\Android\Sdk', 'E:\Android\Sdk')
foreach ($sdk in $sdkRoots) {
    if ($sdk) { $candidates.Add((Join-Path $sdk 'ndk')) }
}
$ndk = ''
foreach ($candidate in $candidates) {
    if ($ndk) { break }
    if (-not $candidate -or -not (Test-Path -LiteralPath $candidate)) { continue }
    if (Test-Ndk $candidate) {
        $ndk = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
    $children = Get-ChildItem -LiteralPath $candidate -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($child in $children) {
        if (Test-Ndk $child.FullName) { $ndk = $child.FullName; break }
    }
}
$distccd = ''
$distccdCandidates = @(
    (Join-Path $env:USERPROFILE 'wc3-dist\bin\distccd.exe'),
    'E:\wc3-dist\bin\distccd.exe',
    'C:\msys64\usr\bin\distccd.exe',
    'C:\msys64\ucrt64\bin\distccd.exe',
    'C:\msys64\mingw64\bin\distccd.exe',
    'D:\msys64\usr\bin\distccd.exe')
foreach ($candidate in $distccdCandidates) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
        $distccd = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
}
if (-not $distccd) {
    $command = Get-Command distccd.exe -ErrorAction SilentlyContinue
    if ($command) { $distccd = $command.Source }
}
if (-not $distccd) {
    foreach ($msys in @('C:\msys64', 'D:\msys64', 'E:\msys64')) {
        if (-not (Test-Path -LiteralPath $msys)) { continue }
        $hit = Get-ChildItem -LiteralPath $msys -Filter distccd.exe -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { $distccd = $hit.FullName; break }
    }
}
$revision = ''
if ($ndk -and (Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) {
    $line = Get-Content -LiteralPath (Join-Path $ndk 'source.properties') `
        -ErrorAction SilentlyContinue |
        Select-String -Pattern '^Pkg\.Revision\s*=\s*(.+)$' | Select-Object -First 1
    if ($line) { $revision = $line.Matches[0].Groups[1].Value.Trim() }
}
$version = ''
if ($distccd) {
    $version = (& $distccd --version 2>&1 | Select-Object -First 1)
}
$cpus = [Environment]::ProcessorCount
$envCpus = 0
if ([int]::TryParse([string]$env:NUMBER_OF_PROCESSORS, [ref]$envCpus) -and $envCpus -gt $cpus) { $cpus = $envCpus }
"WC3_PC_NDK=$ndk"
"WC3_PC_NDK_REVISION=$revision"
"WC3_PC_DISTCCD=$distccd"
"WC3_PC_DISTCCD_VERSION=$version"
"WC3_PC_CPUS=$cpus"
"WC3_PC_TEMP=$env:TEMP"
"WC3_PC_USERPROFILE=$env:USERPROFILE"
PS1
}

probe_remote() {
    write_probe
    ssh_run "cmd /c mkdir $remote_dir" >/dev/null 2>&1 || true
    "$scp_bin" -F "$config_file" -o BatchMode=yes \
        -o ConnectTimeout="${WC3_PC_CONNECT_TIMEOUT:-8}" \
        "$probe_file" "$alias_name:$remote_dir/pc-probe.ps1" >/dev/null 2>&1 ||
        die 'could not upload the remote probe script with scp'
    local output
    output=$(ssh_run "cmd /c powershell -NoProfile -ExecutionPolicy Bypass -File $remote_dir\\pc-probe.ps1" 2>"$state_dir/pc-probe.err") ||
        die "remote probe failed: $(tail -n 2 "$state_dir/pc-probe.err" 2>/dev/null | tr '\n' ' ')"
    probe_value() {
        printf '%s\n' "$output" | sed -n "s/^$1=//p" | tail -n 1 | tr -d '\r'
    }
    probed_ndk=$(probe_value WC3_PC_NDK)
    probed_revision=$(probe_value WC3_PC_NDK_REVISION)
    probed_distccd=$(probe_value WC3_PC_DISTCCD)
    probed_distccd_version=$(probe_value WC3_PC_DISTCCD_VERSION)
    probed_cpus=$(probe_value WC3_PC_CPUS)
}

local_ndk_revision() {
    local ndk=$1 file
    file=$ndk/source.properties
    [ -f "$file" ] || return 0
    awk -F' = ' '/^Pkg[.]Revision/ {print $2; exit}' "$file" | tr -d '\r'
}

write_daemon() {
    local ndk=$1 distccd=$2 slots=$3 ndk_bin log_path
    ndk_bin=$(printf '%s' "$ndk" | tr '/' '\\')
    ndk_bin="$ndk_bin\\toolchains\\llvm\\prebuilt\\windows-x86_64\\bin"
    log_path="%USERPROFILE%\\$remote_dir\\distccd.log"
    umask 077
    cat > "$daemon_file" <<EOF
@echo off
setlocal
set "PATH=$ndk_bin;%PATH%"
set "TMPDIR=%TEMP%"
set "TMP=%TEMP%"
"$distccd" --no-detach --listen 127.0.0.1 --port $remote_port --allow 127.0.0.1 --enable-tcp-insecure --jobs $slots --log-file="$log_path" --log-level=info
EOF
    # cmd.exe expects CRLF batch files; scp preserves these bytes.
    "$python" - "$daemon_file" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
path.write_bytes(data)
PY
}

stop_tunnel() {
    local keep_remote=${1:-0} pid="" cfg=""
    if read_state; then
        pid=${WC3_PC_SSH_PID:-}
        cfg=${WC3_PC_CONFIG:-}
    fi
    if pid_is_tunnel "$pid"; then
        kill "$pid" 2>/dev/null || true
        local i=0
        while pid_is_tunnel "$pid" && [ "$i" -lt 25 ]; do
            sleep 0.2
            i=$((i + 1))
        done
        pid_is_tunnel "$pid" && kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    if [ "$keep_remote" != 1 ] && [ -n "$cfg" ] && [ -f "$cfg" ]; then
        # The login shell can be PowerShell, which parses cmd-style redirection
        # before cmd.exe sees it.  Suppress output locally instead.
        "$ssh_bin" -F "$cfg" -o BatchMode=yes -o ConnectTimeout=5 "$alias_name" \
            'cmd /c taskkill /F /IM distccd.exe' >/dev/null 2>&1 || true
    fi
    rm -f "$state_file" "$config_file"
}

action_resolve() {
    local json=${1:-0} out
    out=$(resolve_target) || exit 3
    if [ "$json" = 1 ]; then
        "$python" - "$out" <<'PY'
import json, sys
address = sys.argv[1]
print(json.dumps({"address": address,
                  "family": "ipv6" if ":" in address else "ipv4"}))
PY
    else
        printf '%s\n' "$out"
    fi
}

action_setup_key() {
    local target
    target=$(resolve_target) || exit 3
    ensure_key
    write_ssh_config "$target"
    if remote_ok; then
        info "SSH key already accepted by $ssh_user@$target"
        return 0
    fi
    [ -n "$ssh_copy_id" ] || info 'ssh-copy-id not found; trying PowerShell'
    if [ -n "$ssh_copy_id" ]; then
        info "copying $key.pub to $ssh_user@$target (the PC will ask for its password)"
        if "$ssh_copy_id" -i "$key.pub" -p "$ssh_port" \
            -o BatchMode=no -o StrictHostKeyChecking=accept-new \
            -o HostKeyAlias=wc3-pc "$ssh_user@$target"; then
            if remote_ok; then
                info 'SSH key accepted'
                return 0
            fi
        fi
    fi
    if install_key_powershell "$target"; then
        if remote_ok; then
            info 'SSH key accepted'
            return 0
        fi
    fi
    cat >&2 <<EOF
[pc-dist] automatic key copy failed.  Add this public key to
[pc-dist]   %USERPROFILE%\\.ssh\\authorized_keys
[pc-dist] or, for an administrator account, to
[pc-dist]   %ProgramData%\\ssh\\administrators_authorized_keys
[pc-dist] on the PC:
[pc-dist]
$(cat "$key.pub")
EOF
    exit 3
}

action_start() {
    local force=0 slots_arg=0 local_slots_arg=0
    local target ndk_arg=${pc_remote_ndk_env:-}
    local probed_ndk='' probed_revision='' probed_distccd=''
    local probed_distccd_version='' probed_cpus=''
    while [ $# -gt 0 ]; do
        case "$1" in
            --force) force=1; shift ;;
            --slots) slots_arg=${2:-0}; shift 2 ;;
            --slots=*) slots_arg=${1#*=}; shift ;;
            --local-slots) local_slots_arg=${2:-0}; shift 2 ;;
            --local-slots=*) local_slots_arg=${1#*=}; shift ;;
            --ndk) ndk_arg=${2:-}; shift 2 ;;
            --ndk=*) ndk_arg=${1#*=}; shift ;;
            --quiet) quiet=1; shift ;;
            --json) json=1; shift ;;
            *) die "start: unknown argument: $1" ;;
        esac
    done

    if read_state && pid_is_tunnel "${WC3_PC_SSH_PID:-}"; then
        if [ "$force" != 1 ]; then
            local owner=${WC3_PC_OWNER_PID:-0}
            if pid_alive "$owner" && [ "$owner" != "$$" ]; then
                die "a pc tunnel is already active on 127.0.0.1:${WC3_PC_LOCAL_PORT:-?} (pid ${WC3_PC_SSH_PID}, owner pid $owner); stop it or use --force" 4
            fi
            info 'replacing a stale tunnel whose owning build is gone'
        fi
        stop_tunnel 1
    elif [ -f "$state_file" ]; then
        stop_tunnel 1
    fi

    target=$(resolve_target) || exit 3
    info "worker $ssh_user@$target"
    ensure_key
    write_ssh_config "$target"
    remote_ok || die "cannot log in to $ssh_user@$target with $key; run '$0 setup-key' (or set WC3_PC_KEY)" 3

    probed_ndk=${ndk_arg:-}
    probed_revision=''
    probed_distccd=${pc_distccd_env:-}
    probed_distccd_version=''
    probed_cpus=''
    if [ -z "$probed_ndk" ] || [ -z "$probed_distccd" ]; then
        probe_remote
        [ -n "$probed_ndk" ] || probed_ndk=$ndk_arg
        [ -n "$probed_distccd" ] || probed_distccd=${pc_distccd_env:-}
    else
        # An explicit pair still gets the CPU count from the remote shell.
        probed_cpus=$(ssh_run 'cmd /c powershell -NoProfile -Command "[Environment]::ProcessorCount"' 2>/dev/null | tr -d '\r\n ' || true)
    fi
    [ -n "$probed_ndk" ] || die 'no remote NDK; set WC3_PC_REMOTE_NDK'
    [ -n "$probed_distccd" ] || die 'no remote distccd; set WC3_PC_DISTCCD'
    [ -n "$ndk_arg" ] && probed_ndk=$ndk_arg
    [ -n "$pc_distccd_env" ] && probed_distccd=$pc_distccd_env
    case "$probed_cpus" in ''|*[!0-9]*) probed_cpus=0 ;; esac

    local local_ndk=${WC3_DIST_NDK_ROOT:-${WC3_NDK:-${ANDROID_NDK_HOME:-}}}
    if [ -n "$local_ndk" ] && [ -n "$probed_revision" ]; then
        local revision
        revision=$(local_ndk_revision "$local_ndk")
        if [ -n "$revision" ] && [ "$revision" != "$probed_revision" ] &&
           [ "${WC3_PC_ALLOW_VERSION_MISMATCH:-0}" != 1 ]; then
            die "PC NDK $probed_revision does not match local NDK $revision; set WC3_PC_ALLOW_VERSION_MISMATCH=1 to override"
        fi
    fi

    local local_slots remote_slots
    local_slots=${local_slots_arg:-${pc_local_slots_env:-0}}
    [ "$local_slots" -gt 0 ] 2>/dev/null || local_slots=$(cpu_count)
    local cap
    cap=$(cpu_count)
    [ "$local_slots" -gt "$cap" ] 2>/dev/null && local_slots=$cap
    [ "$local_slots" -ge 1 ] 2>/dev/null || local_slots=1

    remote_slots=${slots_arg:-${pc_slots_env:-0}}
    if [ "$remote_slots" -le 0 ] 2>/dev/null; then
        remote_slots=$probed_cpus
    fi
    [ "$remote_slots" -gt 0 ] 2>/dev/null ||
        die 'the PC reported no CPU count; set WC3_PC_SLOTS or --slots'

    local local_port
    local_port=${WC3_PC_LOCAL_PORT:-$(free_port)}

    write_daemon "$probed_ndk" "$probed_distccd" "$remote_slots"
    ssh_run "cmd /c mkdir $remote_dir" >/dev/null 2>&1 || true
    "$scp_bin" -F "$config_file" -o BatchMode=yes \
        -o ConnectTimeout="${WC3_PC_CONNECT_TIMEOUT:-8}" \
        "$daemon_file" "$alias_name:$remote_dir/pc-daemon.cmd" >/dev/null 2>&1 ||
        die 'could not upload the remote daemon script with scp'
    # Remove a daemon left by a crashed tunnel.  The one-build rule applies to
    # the PC worker just as it does to IDEAPAD and the phone.  Keep cmd-style
    # redirection out of the remote string: the PC login shell can be
    # PowerShell, which would try to open a file named "nul".
    ssh_run 'cmd /c taskkill /F /IM distccd.exe' >/dev/null 2>&1 || true

    : > "$state_dir/pc-tunnel.out.log"
    : > "$state_dir/pc-tunnel.err.log"
    "$ssh_bin" -F "$config_file" -o BatchMode=yes -o ExitOnForwardFailure=yes \
        -o ConnectTimeout="${WC3_PC_CONNECT_TIMEOUT:-8}" -o ServerAliveInterval=15 \
        -L "127.0.0.1:$local_port:127.0.0.1:$remote_port" \
        "$alias_name" "cmd /c $remote_dir\\pc-daemon.cmd" \
        </dev/null >"$state_dir/pc-tunnel.out.log" 2>"$state_dir/pc-tunnel.err.log" &
    local tunnel_pid=$!

    local ready=0 i=0
    while [ "$i" -lt 120 ]; do
        if ! pid_alive "$tunnel_pid"; then
            break
        fi
        if port_open "$local_port" && pid_alive "$tunnel_pid"; then
            ready=1
            break
        fi
        i=$((i + 1))
        sleep 0.25
    done
    if [ "$ready" != 1 ]; then
        kill "$tunnel_pid" 2>/dev/null || true
        wait "$tunnel_pid" 2>/dev/null || true
        ssh_run 'cmd /c taskkill /F /IM distccd.exe' >/dev/null 2>&1 || true
        local detail
        detail=$(tail -n 4 "$state_dir/pc-tunnel.err.log" 2>/dev/null | tr '\n' ' ')
        [ -n "$detail" ] || detail=$(tail -n 4 "$state_dir/pc-tunnel.out.log" 2>/dev/null | tr '\n' ' ')
        die "distcc tunnel did not come up: $detail"
    fi

    umask 077
    cat > "$state_file" <<EOF
WC3_PC_STATUS=ready
WC3_PC_TARGET=$(printf '%q' "$target")
WC3_PC_USER=$(printf '%q' "$ssh_user")
WC3_PC_SSH_PORT=$ssh_port
WC3_PC_PORT=$remote_port
WC3_PC_LOCAL_PORT=$local_port
WC3_PC_SLOTS=$remote_slots
WC3_PC_LOCAL_SLOTS=$local_slots
WC3_PC_SSH_PID=$tunnel_pid
WC3_PC_CONFIG=$(printf '%q' "$config_file")
WC3_PC_NDK=$(printf '%q' "$probed_ndk")
WC3_PC_REMOTE_NDK=$(printf '%q' "$probed_ndk")
WC3_PC_NDK_REVISION=$(printf '%q' "$probed_revision")
WC3_PC_DISTCCD=$(printf '%q' "$probed_distccd")
WC3_PC_DISTCCD_VERSION=$(printf '%q' "$probed_distccd_version")
WC3_PC_STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
WC3_PC_OWNER_PID=${pc_owner_env:-$PPID}
EOF
    chmod 600 "$state_file" 2>/dev/null || true
    info "ready target=$target local_port=$local_port slots=$remote_slots local_slots=$local_slots"
    print_state
}

print_state() {
    local text
    text=$(cat "$state_file")
    if [ -n "${WC3_PC_STATUS:-}" ]; then
        text=$(printf '%s\n' "$text" |
            sed "s/^WC3_PC_STATUS=.*/WC3_PC_STATUS=$WC3_PC_STATUS/")
    fi
    if [ "${json:-0}" = 1 ]; then
        printf '%s\n' "$text" | "$python" -c '
import json, shlex, sys
out = {}
for line in sys.stdin:
    if "=" not in line:
        continue
    key, value = line.rstrip("\n").split("=", 1)
    parts = shlex.split(value)
    out[key] = parts[0] if parts else ""
print(json.dumps(out, sort_keys=True))
'
    else
        printf '%s\n' "$text"
    fi
}

action_status() {
    if ! read_state; then
        if [ "${json:-0}" = 1 ]; then
            printf '{}\n'
        else
            printf 'WC3_PC_STATUS=stopped\n'
        fi
        return 0
    fi
    if pid_is_tunnel "${WC3_PC_SSH_PID:-}" && port_open "${WC3_PC_LOCAL_PORT:-0}"; then
        WC3_PC_STATUS=ready
    else
        WC3_PC_STATUS=stale
    fi
    print_state
}

action_stop() {
    local keep_remote=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --keep-remote) keep_remote=1; shift ;;
            --quiet) quiet=1; shift ;;
            *) die "stop: unknown argument: $1" ;;
        esac
    done
    stop_tunnel "$keep_remote"
    info 'tunnel stopped'
}

action=${1:-start}
[ $# -gt 0 ] && shift
json=0
case "$action" in
    -h|--help|help)
        sed -n '2,22p' "$0"
        exit 0
        ;;
    resolve)
        [ "${1:-}" = "--json" ] && json=1
        action_resolve "$json"
        ;;
    setup-key)
        action_setup_key
        ;;
    start)
        set -- "$@"
        action_start "$@"
        ;;
    status)
        [ "${1:-}" = "--json" ] && json=1
        action_status
        ;;
    stop)
        action_stop "$@"
        ;;
    *)
        die "unknown action: $action (resolve|setup-key|start|status|stop)"
        ;;
esac
