# Start or stop the hotspot phone's Termux distcc worker.
#
# The phone is the local-network DHCP/DNS server.  It is resolved on every
# start, not cached: first as the default interface's DNS IPv4 server, then as
# the default gateway, then through its MAC-derived IPv6 link-local address or
# its IPv4 neighbor entry.  The IPv4 default route only locates the hotspot
# interface; the candidates themselves are IPv4 and IPv6.  adb auto-start is
# IPv4-only.  A live tunnel stays bound to the address it opened; a new address
# is picked up by the next start.  WC3_PHONE_HOST overrides the resolver and is
# static, so leave it unset when the phone address changes.
#
# The worker is a normal Termux (not root) distccd.  phone_worker.sh creates
# shims named after the exact Windows compiler paths that distcc sends, so the
# preprocessed translation unit is compiled by the same Android NDK r29 that
# the host build uses.  When Termux can call `su`, the worker also moves
# distccd to the top-app cpuset so the big cores are available; otherwise it
# stays on the background cpuset and reports the smaller slot count.
#
# If port 8022 is closed, the start action tries to bring Termux's sshd up
# through wireless adb: it connects to the resolved phone, switches to the
# Termux UID with `su`, asks a live runsvdir to start the sshd service, or
# starts sshd directly when Termux's service supervisor is gone.  This needs
# adb over Wi-Fi (5555 by default) and root `su` granted to the adb shell.  Set
# WC3_PHONE_AUTOSTART=0 to keep the old skip-if-down behavior; when adb or root
# is unavailable the phone is skipped and the caller falls back to IDEAPAD or
# the local build.
#
#   $p = .\tools\dist\phone_tunnel.ps1 -Action start
#   .\tools\dist\phone_tunnel.ps1 -Action resolve
#   .\tools\dist\phone_tunnel.ps1 -Action stop
#
# State lives in %LOCALAPPDATA%\wc3-dist\phone-tunnel.json.
[CmdletBinding()]
param(
    [ValidateSet('start', 'stop', 'status', 'resolve')]
    [string]$Action = 'start',
    [string]$PhoneHost = '',
    [string]$PhoneUser = '',
    [int]$PhoneSshPort = 0,
    [string]$PhoneKey = '',
    [int]$Slots = 0,
    [int]$LocalPort = 0,
    [string]$RemoteHome = '',
    [string]$PhoneNdk = '',
    [string]$HostNdk = '',
    [switch]$Force,
    [switch]$NoAutostart,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell builds a case-insensitive environment dictionary when it
# starts a child process.  An inherited HTTP_PROXY/http_proxy pair therefore
# fails with "element already added" before ssh is launched.  Keep one
# spelling of every variable; Windows environment names are case-insensitive.
function Resolve-Wc3ProcessEnvCaseCollisions {
    $seen = @{}
    foreach ($key in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if ($seen.ContainsKey($key)) {
            [Environment]::SetEnvironmentVariable($key, $null, 'Process')
        } else {
            $seen[$key] = $true
        }
    }
}
Resolve-Wc3ProcessEnvCaseCollisions

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ssh = 'C:\Program Files\Git\usr\bin\ssh.exe'
$scp = 'C:\Program Files\Git\usr\bin\scp.exe'
$stateDir = Join-Path $env:LOCALAPPDATA 'wc3-dist'
$statePath = Join-Path $stateDir 'phone-tunnel.json'
$sshAlias = 'wc3-phone'
$workerScript = Join-Path $PSScriptRoot 'phone_worker.sh'

if (-not $PhoneHost) {
    $PhoneHost = $env:WC3_PHONE_HOST
}
if (-not $PhoneUser) {
    $PhoneUser = if ($env:WC3_PHONE_USER) { $env:WC3_PHONE_USER } else { 'u0_a360' }
}
if ($PhoneSshPort -le 0) {
    $PhoneSshPort = if ($env:WC3_PHONE_SSH_PORT) {
        [int]$env:WC3_PHONE_SSH_PORT
    } else { 8022 }
}
if (-not $PhoneKey) {
    $PhoneKey = if ($env:WC3_PHONE_KEY) {
        $env:WC3_PHONE_KEY
    } else {
        Join-Path $env:USERPROFILE '.ssh\id_marble'
    }
}
if (-not $RemoteHome) {
    $RemoteHome = if ($env:WC3_PHONE_HOME) {
        $env:WC3_PHONE_HOME.TrimEnd('/')
    } else {
        '/data/data/com.termux/files/home'
    }
}
if (-not $PhoneNdk) {
    $PhoneNdk = if ($env:WC3_PHONE_NDK) {
        $env:WC3_PHONE_NDK.TrimEnd('/')
    } else {
        "$RemoteHome/tools/android-ndk-r29"
    }
}
if (-not $HostNdk) {
    if ($env:WC3_HOST_NDK) {
        $HostNdk = $env:WC3_HOST_NDK
    } elseif ($env:ANDROID_NDK_ROOT) {
        $HostNdk = $env:ANDROID_NDK_ROOT
    } elseif ($env:ANDROID_NDK_HOME) {
        $HostNdk = $env:ANDROID_NDK_HOME
    } else {
        $HostNdk = 'E:\Android\Sdk\ndk\29.0.14206865'
    }
}
if ($Slots -le 0 -and $env:WC3_DIST_PHONE_SLOTS) {
    $parsedSlots = 0
    if ([int]::TryParse($env:WC3_DIST_PHONE_SLOTS, [ref]$parsedSlots) -and
        $parsedSlots -gt 0) {
        $Slots = $parsedSlots
    }
}
$phoneMac = if ($env:WC3_PHONE_MAC) {
    $env:WC3_PHONE_MAC
} else {
    '12-23-73-4E-0F-F4'
}
$boost = if ($env:WC3_PHONE_BOOST -eq '0') { '0' } else { '1' }
$allowMismatch = if ($env:WC3_PHONE_ALLOW_VERSION_MISMATCH -eq '1') { '1' } else { '0' }
$autostart = -not $NoAutostart -and $env:WC3_PHONE_AUTOSTART -ne '0'
$adbPort = 5555
if ($env:WC3_PHONE_ADB_PORT) {
    $parsedAdbPort = 0
    if ([int]::TryParse($env:WC3_PHONE_ADB_PORT, [ref]$parsedAdbPort) -and
        $parsedAdbPort -gt 0 -and $parsedAdbPort -le 65535) {
        $adbPort = $parsedAdbPort
    } else {
        Write-Warning ("[phone] WC3_PHONE_ADB_PORT is not a valid port: {0}; using 5555" -f
            $env:WC3_PHONE_ADB_PORT)
    }
}
$adbSerialOverride = $env:WC3_PHONE_ADB_SERIAL
$adbPathOverride = $env:WC3_PHONE_ADB
$phoneUidOverride = $env:WC3_PHONE_UID

function Write-Info([string]$Message) {
    if (-not $Quiet) { Write-Host "[phone] $Message" }
}

function Get-State {
    if (-not (Test-Path -LiteralPath $statePath)) { return $null }
    try {
        return Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Normalize-Mac([string]$Mac) {
    $hex = ($Mac -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
    if ($hex.Length -ne 12) { return $null }
    $parts = for ($i = 0; $i -lt 12; $i += 2) {
        $hex.Substring($i, 2)
    }
    return ($parts -join '-')
}

function Convert-MacToLinkLocal([string]$Mac) {
    $clean = $Mac -replace '[^0-9A-Fa-f]', ''
    if ($clean.Length -ne 12) { return $null }
    [byte[]]$bytes = for ($i = 0; $i -lt 12; $i += 2) {
        [Convert]::ToByte($clean.Substring($i, 2), 16)
    }
    $bytes[0] = [byte]($bytes[0] -bxor 0x02)
    $hex = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
    $eui = $hex.Substring(0, 6) + 'fffe' + $hex.Substring(6)
    $groups = for ($i = 0; $i -lt 16; $i += 4) {
        $value = $eui.Substring($i, 4).TrimStart('0')
        if (-not $value) { $value = '0' }
        $value
    }
    return ('fe80::{0}:{1}:{2}:{3}' -f $groups[0], $groups[1], $groups[2], $groups[3])
}

function Test-Tcp([string]$Address, [int]$Port, [int]$TimeoutMs = 800) {
    $client = $null
    try {
        $ip = $null
        if ([System.Net.IPAddress]::TryParse($Address, [ref]$ip)) {
            $client = New-Object System.Net.Sockets.TcpClient($ip.AddressFamily)
            $task = $client.ConnectAsync($ip, $Port)
        } else {
            $client = New-Object System.Net.Sockets.TcpClient
            $task = $client.ConnectAsync($Address, $Port)
        }
        if (-not $task.Wait($TimeoutMs)) { return $false }
        return $true
    } catch {
        return $false
    } finally {
        if ($client) { $client.Dispose() }
    }
}

function Get-PhoneCandidates {
    if ($PhoneHost) {
        return [pscustomobject]@{
            Address        = $PhoneHost
            Source         = 'override'
            InterfaceIndex = 0
        }
    }

    $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.NextHop -and $_.NextHop -ne '0.0.0.0' } |
        Sort-Object RouteMetric |
        Select-Object -First 1
    if (-not $route) {
        throw 'no IPv4 default route; cannot locate the hotspot phone'
    }
    $ifIndex = [int]$route.ifIndex
    $localIps = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty IPAddress)
    $mac = Normalize-Mac $phoneMac
    $candidates = New-Object System.Collections.Generic.List[object]
    $seen = @{}

    function Add-Candidate([string]$Address, [string]$Source, [int]$Index) {
        if (-not $Address) { return }
        $key = $Address.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { return }
        $seen[$key] = $true
        $candidates.Add([pscustomobject]@{
            Address        = $Address
            Source         = $Source
            InterfaceIndex = $Index
        })
    }

    foreach ($server in @(Get-DnsClientServerAddress -InterfaceIndex $ifIndex `
            -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty ServerAddresses)) {
        if ($server -match '^\d+\.' -and $server -notin $localIps) {
            Add-Candidate $server 'dns' $ifIndex
        }
    }
    if ($route.NextHop -ne '127.0.0.1' -and $route.NextHop -notin $localIps) {
        Add-Candidate $route.NextHop 'gateway' $ifIndex
    }
    if ($mac) {
        foreach ($neighbor in @(Get-NetNeighbor -ErrorAction SilentlyContinue |
                Where-Object { $_.LinkLayerAddress -eq $mac })) {
            if ($neighbor.AddressFamily -eq 'IPv4') {
                Add-Candidate $neighbor.IPAddress 'mac-ipv4' ([int]$neighbor.ifIndex)
            } elseif ($neighbor.AddressFamily -eq 'IPv6') {
                Add-Candidate ("{0}%{1}" -f $neighbor.IPAddress, $neighbor.ifIndex) `
                    'mac-ipv6' ([int]$neighbor.ifIndex)
            }
        }
        $linkLocal = Convert-MacToLinkLocal $mac
        if ($linkLocal) {
            Add-Candidate ("{0}%{1}" -f $linkLocal, $ifIndex) 'mac-eui64' $ifIndex
        }
    }

    return $candidates.ToArray()
}

function Resolve-PhoneTarget {
    $candidates = @(Get-PhoneCandidates)
    foreach ($candidate in $candidates) {
        if (Test-Tcp $candidate.Address $PhoneSshPort) {
            return [pscustomobject]@{
                Address        = $candidate.Address
                Source         = $candidate.Source
                InterfaceIndex = $candidate.InterfaceIndex
                Mac            = (Normalize-Mac $phoneMac)
                Candidates     = $candidates
            }
        }
    }
    if ($PhoneHost) {
        throw "WC3_PHONE_HOST is set to $PhoneHost but port $PhoneSshPort is not reachable"
    }
    $tried = if ($candidates.Count -gt 0) {
        ($candidates | ForEach-Object { "{0}({1})" -f $_.Address, $_.Source }) -join ', '
    } else { 'none' }
    throw "phone SSH port $PhoneSshPort is not reachable; tried $tried"
}

function Get-AdbPath {
    if ($adbPathOverride) {
        if (Test-Path -LiteralPath $adbPathOverride) {
            return (Resolve-Path -LiteralPath $adbPathOverride).Path
        }
        throw "WC3_PHONE_ADB is set to $adbPathOverride but that file was not found"
    }
    $command = Get-Command adb.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $command) {
        $command = Get-Command adb -ErrorAction SilentlyContinue |
            Select-Object -First 1
    }
    if ($command) { return $command.Source }

    $localSdk = if ($env:LOCALAPPDATA) {
        Join-Path $env:LOCALAPPDATA 'Android\Sdk'
    } else { '' }
    $bases = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME,
        'E:\Android\Sdk', $localSdk)
    foreach ($base in $bases) {
        if (-not $base) { continue }
        $candidate = Join-Path $base 'platform-tools\adb.exe'
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Get-AdbDeviceState {
    param([string]$Adb, [string]$Serial)
    try {
        $lines = & $Adb devices 2>$null
    } catch {
        return ''
    }
    foreach ($line in @($lines)) {
        $text = ([string]$line).Trim()
        if (-not $text -or $text -match '^List of devices') { continue }
        $parts = $text -split '\s+'
        if ($parts.Count -ge 2 -and $parts[0] -eq $Serial) {
            return $parts[1]
        }
    }
    return ''
}

function Start-PhoneSsh {
    # adb is the out-of-band path: it works while Termux's sshd is down.  The
    # nested su keeps the sshd running as the Termux UID rather than root.  A
    # live runsvdir is preferred because it restores the normal supervised
    # service; without it, start sshd directly, which is the cold-start case.
    param([object[]]$Candidates)

    $adb = Get-AdbPath
    if (-not $adb) {
        throw 'adb not found; cannot auto-start the Termux sshd'
    }
    if ($phoneUidOverride -and $phoneUidOverride -notmatch '^\d+$') {
        throw "WC3_PHONE_UID is not numeric: $phoneUidOverride"
    }

    $autostartScript = @'
#!/data/data/com.termux/files/usr/bin/sh
set -u
export HOME=/data/data/com.termux/files/home
export PREFIX=/data/data/com.termux/files/usr
export PATH="$PREFIX/bin:$PATH"
export LD_PRELOAD="$PREFIX/lib/libtermux-exec-ld-preload.so"
export TMPDIR="$PREFIX/tmp"
export SVDIR="$PREFIX/var/service"
cd "$HOME" || exit 1
mkdir -p "$HOME/.wc3-dist"
port='__PORT__'
use_service='__USE_SERVICE__'
if [ ! -x "$PREFIX/bin/sshd" ]; then
    echo 'WC3_PHONE_AUTOSTART=missing-sshd'
    exit 1
fi
if [ "$use_service" = '1' ] && ps -ef | grep -q '[r]unsvdir' &&
        [ -d "$SVDIR/sshd" ]; then
    if SVDIR="$SVDIR" sv up sshd >/dev/null 2>&1; then
        sleep 1
        if ps -ef | grep -q '[s]shd -D -e'; then
            echo 'WC3_PHONE_AUTOSTART=service'
            exit 0
        fi
    fi
fi
# The supervised service is the normal case.  If it was down, a different
# main sshd may already exist on the requested port; do not start a duplicate.
if [ "$use_service" = '1' ] && ps -ef | grep -q '[s]shd -D -e'; then
    echo 'WC3_PHONE_AUTOSTART=already'
    exit 0
fi
nohup "$PREFIX/bin/sshd" -D -e -p "$port" \
    -o "PidFile=$HOME/.wc3-dist/sshd-autostart.pid" \
    >>"$HOME/.wc3-dist/sshd-autostart.log" 2>&1 < /dev/null &
pid=$!
echo "$pid" > "$HOME/.wc3-dist/sshd-autostart.pid"
sleep 1
echo "WC3_PHONE_AUTOSTART=direct pid=$pid"
tail -n 8 "$HOME/.wc3-dist/sshd-autostart.log" 2>/dev/null || true
'@

    $useService = if ($PhoneSshPort -eq 8022) { '1' } else { '0' }
    $autostartScript = $autostartScript -replace '__PORT__', "$PhoneSshPort"
    $autostartScript = $autostartScript -replace '__USE_SERVICE__', $useService
    $autostartScript = $autostartScript -replace "`r", ''

    $attempts = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in @($Candidates)) {
        if (-not $candidate -or -not $candidate.Address) { continue }
        $address = [string]$candidate.Address
        # Wireless adb is an IPv4 transport here.  Skip link-local IPv6
        # candidates and any scoped address; SSH itself can still use them.
        if ($address -match '%') { continue }
        $parsedAddress = $null
        if ([System.Net.IPAddress]::TryParse($address, [ref]$parsedAddress) -and
            $parsedAddress.AddressFamily -ne
                [System.Net.Sockets.AddressFamily]::InterNetwork) {
            continue
        }

        $serial = if ($adbSerialOverride) {
            $adbSerialOverride
        } else {
            '{0}:{1}' -f $address, $adbPort
        }
        if (-not $adbSerialOverride) {
            if (-not (Test-Tcp $address $adbPort 800)) {
                $attempts.Add(("{0} (adb port {1} closed)" -f
                    $address, $adbPort)) | Out-Null
                continue
            }
            Write-Info ("trying adb auto-start on {0}" -f $serial)
            $connectOutput = (& $adb connect $serial 2>&1 | Out-String).Trim()
            $state = Get-AdbDeviceState $adb $serial
            if ($state -ne 'device') {
                $detail = if ($state) { $state } else { 'not connected' }
                $attempts.Add(("{0} (adb {1}: {2})" -f
                    $serial, $detail, ($connectOutput -replace '\s+', ' '))) |
                    Out-Null
                continue
            }
        } else {
            $state = Get-AdbDeviceState $adb $serial
            if ($state -ne 'device') {
                $detail = if ($state) { $state } else { 'not connected' }
                $attempts.Add(("{0} (adb {1})" -f $serial, $detail)) | Out-Null
                continue
            }
        }

        $uid = ''
        if ($phoneUidOverride) {
            $uid = $phoneUidOverride
        } else {
            $uidText = (& $adb -s $serial shell `
                "su -c 'stat -c %u /data/data/com.termux'" 2>&1 |
                Out-String)
            foreach ($line in ($uidText -split "`r?`n")) {
                $trim = $line.Trim()
                if ($trim -match '^\d+$') { $uid = $trim; break }
            }
            if (-not $uid) {
                $packageText = (& $adb -s $serial shell `
                    'dumpsys package com.termux | grep -m1 userId' 2>&1 |
                    Out-String)
                if ($packageText -match 'userId=(\d+)') {
                    $uid = $Matches[1]
                }
            }
            if (-not $uid -and $PhoneUser -match '^u0_a(\d+)$') {
                $uid = [string](10000 + [int]$Matches[1])
            }
        }
        if (-not $uid) {
            $attempts.Add(("{0} (Termux UID not found)" -f $address)) |
                Out-Null
            continue
        }
        $uid = [string]([int]$uid)

        $shellCommand = "su -c `"su $uid -c 'sh -s'`""
        $scriptOutput = ($autostartScript | & $adb -s $serial shell `
            $shellCommand 2>&1 | Out-String).Trim()
        $opened = $false
        for ($i = 0; $i -lt 40; $i++) {
            if (Test-Tcp $address $PhoneSshPort 800) {
                $opened = $true
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if ($opened) {
            Write-Info ("adb auto-start ok on {0}: {1}" -f
                $address, ($scriptOutput -replace '\s+', ' '))
            return $candidate
        }
        $attempts.Add(("{0} (ssh port {1} still closed; {2})" -f
            $address, $PhoneSshPort,
            ($scriptOutput -replace '\s+', ' '))) | Out-Null
    }

    $detail = if ($attempts.Count -gt 0) {
        '; tried ' + ($attempts -join ', ')
    } else { '' }
    throw ("adb auto-start could not bring up the Termux sshd on port {0}{1}" -f
        $PhoneSshPort, $detail)
}

function New-SshConfig([string]$Target) {
    $escaped = $Target -replace '%', '%%'
    $path = Join-Path $env:TEMP ("wc3-phone-{0}.conf" -f $PID)
    $lines = @(
        "Host $sshAlias",
        "    HostName $escaped",
        "    User $PhoneUser",
        "    Port $PhoneSshPort",
        "    IdentityFile $PhoneKey",
        "    StrictHostKeyChecking accept-new",
        "    HostKeyAlias wc3-phone",
        "    WarnWeakCrypto no",
        "    ServerAliveInterval 15",
        "    LogLevel ERROR"
    )
    Set-Content -LiteralPath $path -Value $lines -Encoding ascii
    return $path
}

function Invoke-Remote([string]$Config, [string]$Command) {
    # Use ProcessStartInfo rather than the call operator: Windows PowerShell
    # turns native stderr into error records, which $ErrorActionPreference=Stop
    # would surface as exceptions for harmless remote messages.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ssh
    $argItems = @(
        '-F', $Config,
        '-o', 'BatchMode=yes',
        '-o', 'ConnectTimeout=8',
        '-o', 'ServerAliveInterval=10',
        $sshAlias,
        $Command
    )
    $quoted = foreach ($item in $argItems) {
        if ($item -match '[\s"]') {
            '"' + ($item -replace '"', '\"') + '"'
        } else {
            $item
        }
    }
    $psi.Arguments = $quoted -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output   = @($stdout -split "`r?`n" | Where-Object { $_ -ne '' })
        Error    = $stderr
    }
}

function Get-HostRevision {
    $props = Join-Path $HostNdk 'source.properties'
    if (-not (Test-Path -LiteralPath $props)) { return '' }
    $line = Get-Content -LiteralPath $props -ErrorAction SilentlyContinue |
        Where-Object { $_ -match '^Pkg\.Revision\s*=' } |
        Select-Object -First 1
    if (-not $line) { return '' }
    return (($line -split '=', 2)[1]).Trim()
}

function Get-RemoteInfo([string]$Config) {
    $command = "nproc; cat $PhoneNdk/source.properties 2>/dev/null; " +
        'command -v distccd; command -v su'
    $result = Invoke-Remote $Config $command
    if ($result.ExitCode -ne 0 -and $result.Output.Count -eq 0) {
        throw 'could not query the phone over SSH'
    }
    $lines = @($result.Output | ForEach-Object { $_.Trim() } |
        Where-Object { $_ })
    $nproc = 0
    foreach ($line in $lines) {
        if ($line -match '^\d+$') {
            $nproc = [int]$line
            break
        }
    }
    $revision = ''
    $revisionLine = $lines | Where-Object { $_ -match '^Pkg\.Revision\s*=' } |
        Select-Object -First 1
    if ($revisionLine) {
        $revision = (($revisionLine -split '=', 2)[1]).Trim()
    }
    $distccd = @($lines | Where-Object { $_ -match '/distccd$' }).Count -gt 0
    $su = @($lines | Where-Object { $_ -match '/su$' }).Count -gt 0
    return [pscustomobject]@{
        Nproc    = $nproc
        Revision = $revision
        Distccd  = $distccd
        Su       = $su
    }
}

function Write-LfFile([string]$Source, [string]$Destination) {
    $text = Get-Content -LiteralPath $Source -Raw
    $text = $text -replace "`r`n", "`n" -replace "`r", "`n"
    [IO.File]::WriteAllText($Destination, $text,
        (New-Object System.Text.UTF8Encoding($false)))
}

function Get-FreeLoopbackPort {
    param([int]$RequestedPort = 0)
    $listener = New-Object System.Net.Sockets.TcpListener(
        [System.Net.IPAddress]::Loopback, $RequestedPort)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Stop-PhoneTunnel {
    param([switch]$RemoteCleanup)
    $state = Get-State
    if (-not $state) {
        if ($RemoteCleanup) {
            try {
                $target = Resolve-PhoneTarget
                $config = New-SshConfig $target.Address
                Invoke-Remote $config ("pkill -f '[d]istccd' 2>/dev/null; " +
                    'termux-wake-unlock 2>/dev/null') | Out-Null
                Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
            } catch { }
        }
        return
    }
    $pidValue = 0
    if ([int]::TryParse([string]$state.Pid, [ref]$pidValue)) {
        $process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
        if ($process) {
            Stop-Process -Id $pidValue -Force -ErrorAction SilentlyContinue
            try { $process.WaitForExit(5000) | Out-Null } catch { }
        }
    }
    if ($RemoteCleanup -and $state.Config -and (Test-Path -LiteralPath $state.Config)) {
        try {
            Invoke-Remote $state.Config ("pkill -f '[d]istccd' 2>/dev/null; " +
                'termux-wake-unlock 2>/dev/null') | Out-Null
        } catch { }
    }
    if ($state.Config -and (Test-Path -LiteralPath $state.Config)) {
        Remove-Item -LiteralPath $state.Config -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
}

if ($Action -eq 'resolve') {
    $resolved = Resolve-PhoneTarget
    $resolved | ConvertTo-Json -Depth 4 -Compress
    exit 0
}

if ($Action -eq 'status') {
    $state = Get-State
    if ($state) { $state | ConvertTo-Json -Compress } else { '{}' }
    exit 0
}

if ($Action -eq 'stop') {
    Stop-PhoneTunnel -RemoteCleanup
    Write-Info 'phone tunnel stopped'
    exit 0
}

$existing = Get-State
if ($existing -and -not $Force) {
    $existingPid = 0
    if ([int]::TryParse([string]$existing.Pid, [ref]$existingPid)) {
        $existingProcess = Get-Process -Id $existingPid -ErrorAction SilentlyContinue
        if ($existingProcess) {
            throw ("a phone distcc tunnel is already active on port {0} (ssh pid {1}); " +
                'stop it first or pass -Force') -f $existing.Port, $existing.Pid
        }
    }
}
Stop-PhoneTunnel -RemoteCleanup

if (-not (Test-Path -LiteralPath $ssh)) { throw "ssh not found: $ssh" }
if (-not (Test-Path -LiteralPath $scp)) { throw "scp not found: $scp" }
if (-not (Test-Path -LiteralPath $PhoneKey)) { throw "phone SSH key not found: $PhoneKey" }
if (-not (Test-Path -LiteralPath $workerScript)) {
    throw "phone worker script not found: $workerScript"
}
if (-not (Test-Path -LiteralPath $stateDir)) {
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
}

$process = $null
$config = $null
try {
    try {
        $target = Resolve-PhoneTarget
    } catch {
        $resolveError = $_.Exception.Message
        if (-not $autostart) { throw }
        Write-Info ("phone SSH is not reachable ({0}); trying adb auto-start" -f
            $resolveError)
        try {
            $candidates = @(Get-PhoneCandidates)
            Start-PhoneSsh -Candidates $candidates | Out-Null
            $target = Resolve-PhoneTarget
        } catch {
            throw ("phone SSH is not reachable ({0}); adb auto-start failed: {1}" -f
                $resolveError, $_.Exception.Message)
        }
    }
    $config = New-SshConfig $target.Address
    Write-Info ("resolved {0} via {1}" -f $target.Address, $target.Source)

    $info = Get-RemoteInfo $config
    $hostRevision = Get-HostRevision
    if ($hostRevision -and $info.Revision -and
        $info.Revision -ne $hostRevision -and $allowMismatch -ne '1') {
        throw ("phone NDK revision {0} does not match host revision {1}; " +
            'set WC3_PHONE_ALLOW_VERSION_MISMATCH=1 to override') -f
            $info.Revision, $hostRevision
    }
    if (-not $info.Revision) {
        Write-Warning ("[phone] could not read Pkg.Revision from $PhoneNdk/source.properties")
    }

    $mkdir = Invoke-Remote $config ("mkdir -p {0}/.wc3-dist" -f $RemoteHome)
    if ($mkdir.ExitCode -ne 0) {
        throw ("could not create {0}/.wc3-dist" -f $RemoteHome)
    }

    $clangRel = Join-Path $HostNdk `
        'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
    $clangxxRel = Join-Path $HostNdk `
        'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
    $tempDir = Join-Path $env:TEMP ("wc3-phone-{0}" -f $PID)
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    $workerLocal = Join-Path $tempDir 'phone_worker.sh'
    $envLocal = Join-Path $tempDir 'worker.env'
    Write-LfFile $workerScript $workerLocal
    $envLines = @(
        "PHONE_NDK='$PhoneNdk'",
        "CLANG_REL='$clangRel'",
        "CLANGXX_REL='$clangxxRel'",
        "EXPECTED_REVISION='$hostRevision'",
        "BOOST='$boost'",
        "FORCE_JOBS='$Slots'",
        "ALLOW_VERSION_MISMATCH='$allowMismatch'"
    ) -join "`n"
    [IO.File]::WriteAllText($envLocal, $envLines + "`n",
        (New-Object System.Text.UTF8Encoding($false)))

    $remoteDir = "$RemoteHome/.wc3-dist"
    & $scp -F $config -o BatchMode=yes -o ConnectTimeout=8 `
        $workerLocal $envLocal ("{0}:{1}/" -f $sshAlias, $remoteDir) 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "scp to the phone failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $tempDir -Recurse -Force `
        -ErrorAction SilentlyContinue

    $port = Get-FreeLoopbackPort -RequestedPort $LocalPort
    $logBase = Join-Path $stateDir ("phone-tunnel-{0}" -f $PID)
    $stdoutLog = "$logBase.out.log"
    $stderrLog = "$logBase.err.log"
    $remoteCommand = "sh $remoteDir/phone_worker.sh"
    $sshArgs = @(
        '-F', $config,
        '-o', 'BatchMode=yes',
        '-o', 'ExitOnForwardFailure=yes',
        '-o', 'ConnectTimeout=8',
        '-o', 'ServerAliveInterval=15',
        '-L', ('127.0.0.1:{0}:127.0.0.1:3632' -f $port),
        $sshAlias,
        $remoteCommand
    )
    $process = Start-Process -FilePath $ssh -ArgumentList $sshArgs -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog

    $ready = $false
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 250
        if ($process.HasExited) { break }
        $listener = Get-NetTCPConnection -LocalPort $port -State Listen `
            -ErrorAction SilentlyContinue
        if ($listener) { $ready = $true; break }
    }
    if (-not $ready) {
        throw 'phone distcc tunnel did not bind its loopback port'
    }

    # The worker prints its final slot count only after distccd is listening
    # and the cpuset boost (if any) has been verified.  The marker travels over
    # the tunnel's stdout, so no second SSH session is needed; fall back to the
    # pre-check nproc on a slow device.
    $phoneSlots = 0
    $phoneBoosted = $false
    $haveBoostMarker = $false
    for ($i = 0; $i -lt 80; $i++) {
        if (Test-Path -LiteralPath $stdoutLog) {
            $text = Get-Content -LiteralPath $stdoutLog -Raw `
                -ErrorAction SilentlyContinue
            if ($text -match 'WC3_PHONE_SLOTS=(\d+)') {
                $phoneSlots = [int]$Matches[1]
            }
            if ($text -match 'WC3_PHONE_BOOSTED=(\d)') {
                $phoneBoosted = ($Matches[1] -eq '1')
                $haveBoostMarker = $true
            }
            if ($phoneSlots -gt 0 -and $haveBoostMarker) { break }
        }
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($phoneSlots -le 0) {
        $phoneSlots = if ($info.Nproc -gt 0) { $info.Nproc } else { 1 }
        Write-Warning ("[phone] worker did not publish slots; using nproc={0}" -f $phoneSlots)
    }

    $state = [pscustomobject]@{
        Pid          = $process.Id
        OwnerPid     = $PID
        Port         = $port
        RemotePort   = 3632
        Slots        = $phoneSlots
        Config       = $config
        Target       = $target.Address
        Source       = $target.Source
        Mac          = $target.Mac
        NdkRevision  = $info.Revision
        Boosted      = $phoneBoosted
        Started      = (Get-Date).ToString('o')
    }
    $state | ConvertTo-Json -Compress | Set-Content -LiteralPath $statePath -Encoding ascii
    Write-Info ("ready target={0} source={1} local_port={2} slots={3}" -f `
        $target.Address, $target.Source, $port, $phoneSlots)
    $state | ConvertTo-Json -Compress
} catch {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    if ($config -and (Test-Path -LiteralPath $config)) {
        try {
            Invoke-Remote $config ("pkill -f '[d]istccd' 2>/dev/null; " +
                'termux-wake-unlock 2>/dev/null') | Out-Null
        } catch { }
        Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
    }
    $stderr = ''
    if ($stdoutLog -and (Test-Path -LiteralPath $stdoutLog)) {
        $stderr += Get-Content -LiteralPath $stdoutLog -Raw
    }
    if ($stderrLog -and (Test-Path -LiteralPath $stderrLog)) {
        $stderr += Get-Content -LiteralPath $stderrLog -Raw
    }
    $detail = if ($stderr) { "`n$($stderr.Trim())" } else { '' }
    throw ("$($_.Exception.Message)$detail")
}
