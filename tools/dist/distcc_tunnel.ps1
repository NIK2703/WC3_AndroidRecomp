# Start or stop the distributed workers and their TCP-over-SSH tunnels:
# IDEAPAD and the local hotspot phone (POCO F5, Termux).
#
# Either worker is optional and independent.  Each has its own SSH tunnel and
# remote distccd; the caller can use both, one of them, or neither (a plain
# local build).  The phone is resolved on every start by phone_tunnel.ps1; it
# is not tied to IDEAPAD.
#
#   $t = .\tools\dist\distcc_tunnel.ps1 -Action start
#   $t = .\tools\dist\distcc_tunnel.ps1 -Action start -NoIdeapad
#   $t = .\tools\dist\distcc_tunnel.ps1 -Action start -NoPhone
#   ...
#   .\tools\dist\distcc_tunnel.ps1 -Action stop
#   .\tools\dist\distcc_tunnel.ps1 -Action slots   # IDEAPAD CPU count only
#
# The start action prints one JSON object with Port, RemoteSlots, LocalSlots,
# PhonePort, PhoneSlots and Pid.  A worker that is unavailable has zero slots
# and an empty port; both unavailable is still a successful start that the
# caller can run as a local build.  State lives in
# %LOCALAPPDATA%\wc3-dist\distcc-tunnel.json.
[CmdletBinding()]
param(
    [ValidateSet('start', 'stop', 'status', 'slots')]
    [string]$Action = 'start',
    [string]$HostName = '',
    [int]$LocalSlots = 0,
    [int]$RemoteSlots = 0,
    [int]$RemotePort = 3632,
    # Optional fixed loopback port.  Useful when a running build already has
    # DISTCC_HOSTS pointing at a port and its tunnel must be restored.
    [int]$LocalPort = 0,
    # Replace a live tunnel.  The default refuses so a diagnostic run cannot
    # steal the tunnel of an active build.
    [switch]$Force,
    [string]$RemoteRoot = 'E:/wc3-dist',
    # Merged profile for a final release build.  It is copied to the
    # same absolute path on IDEAPAD, so the command line stays identical on
    # both sides and distcc's local preprocessor still sees a real file.
    [string]$PgoProfile = '',
    # Either worker can be disabled independently.  The environment variables
    # WC3_DIST_IDEAPAD=0 and WC3_DIST_PHONE=0 do the same for callers that do
    # not pass switches.
    [switch]$NoIdeapad,
    [switch]$NoPhone,
    # Optional fixed phone slot count and loopback port.  Zero means autodetect.
    [int]$PhoneSlots = 0,
    [int]$PhoneLocalPort = 0,
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
$key = Join-Path $env:USERPROFILE '.ssh\id_ideapad'
$stateDir = Join-Path $env:LOCALAPPDATA 'wc3-dist'
$statePath = Join-Path $stateDir 'distcc-tunnel.json'
$phoneTunnelScript = Join-Path $PSScriptRoot 'phone_tunnel.ps1'
$sshAlias = 'ideapad-distcc'

$useIdeapad = -not $NoIdeapad
if ($env:WC3_DIST_IDEAPAD -eq '0') { $useIdeapad = $false }
$usePhone = -not $NoPhone
if ($env:WC3_DIST_PHONE -eq '0') { $usePhone = $false }

if ($LocalSlots -le 0) {
    $LocalSlots = [Environment]::ProcessorCount
}
if ($LocalSlots -gt [Environment]::ProcessorCount) {
    $LocalSlots = [Environment]::ProcessorCount
}

function Write-Info([string]$Message) {
    if (-not $Quiet) { Write-Host "[distcc] $Message" }
}

function Get-State {
    if (-not (Test-Path -LiteralPath $statePath)) { return $null }
    try {
        return Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Resolve-Ideapad {
    if ($HostName) { return $HostName.Trim() }
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $pythonCommand) { throw 'python.exe not found in PATH' }
    $wrapper = Join-Path $root 'tools\dist\wc3_dist_cc.py'
    $resolved = & $pythonCommand.Source $wrapper --print-host
    if ($LASTEXITCODE -ne 0 -or -not $resolved) {
        throw 'could not resolve the IDEAPAD IPv6 link-local address'
    }
    return ($resolved | Select-Object -Last 1).Trim()
}

function New-SshConfig([string]$Target) {
    # ssh expands % in HostName, so the link-local zone separator is escaped.
    $escaped = $Target -replace '%', '%%'
    $path = Join-Path $env:TEMP ("wc3-ideapad-{0}.conf" -f $PID)
    $lines = @(
        "Host $sshAlias",
        "    HostName $escaped",
        "    User nikita",
        "    IdentityFile $key",
        "    WarnWeakCrypto no",
        "    StrictHostKeyChecking accept-new",
        "    HostKeyAlias ideapad",
        "    LogLevel ERROR"
    )
    Set-Content -LiteralPath $path -Value $lines -Encoding ascii
    return $path
}

function Invoke-Remote([string]$Config, [string]$Command) {
    & $ssh -F $Config -o BatchMode=yes -o ConnectTimeout=8 `
        -o ServerAliveInterval=10 $sshAlias $Command 2>$null
    return $LASTEXITCODE
}

function Get-RemoteSlots([string]$Config) {
    # Avoid a cmd-only variable expansion: the SSH login shell may be
    # PowerShell.  ProcessorCount is the same logical CPU count that the
    # local side uses, so the default remains all available cores.
    $value = & $ssh -F $Config -o BatchMode=yes -o ConnectTimeout=8 `
        $sshAlias 'cmd /c powershell -NoProfile -Command "[Environment]::ProcessorCount"' 2>$null
    $line = $value | Select-Object -Last 1
    $parsed = 0
    if ($line -and [int]::TryParse(([string]$line).Trim(), [ref]$parsed) -and $parsed -gt 0) {
        return $parsed
    }
    throw 'could not read the IDEAPAD CPU count; pass -RemoteSlots'
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

function Stop-Tunnel {
    param([switch]$RemoteCleanup)
    $state = Get-State
    if ($state) {
        $pidValue = 0
        if ([int]::TryParse([string]$state.Pid, [ref]$pidValue) -and
            $pidValue -gt 0) {
            $process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
            if ($process) {
                Stop-Process -Id $pidValue -Force -ErrorAction SilentlyContinue
                try { $process.WaitForExit(5000) | Out-Null } catch { }
            }
        }
        $hasConfig = [bool]($state.Config -and
            (Test-Path -LiteralPath $state.Config))
        if ($RemoteCleanup -and $hasConfig) {
            try {
                Invoke-Remote $state.Config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
            } catch { }
        }
        if ($hasConfig) {
            Remove-Item -LiteralPath $state.Config -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
    }
    # The phone has its own state file and may outlive a crashed main tunnel.
    # Stop it even when the main state is missing.
    if ($phoneTunnelScript -and (Test-Path -LiteralPath $phoneTunnelScript)) {
        try {
            & $phoneTunnelScript -Action stop -Quiet | Out-Null
        } catch { }
    }
}

if ($Action -eq 'status') {
    $state = Get-State
    if ($state) { $state | ConvertTo-Json -Compress } else { '{}' }
    exit 0
}

if ($Action -eq 'slots') {
    # Report the worker's own logical CPU count without starting a tunnel.
    # build_dist.ps1 uses this for the legacy SSH transport so its default is
    # all of IDEAPAD's cores instead of a fixed eight.
    try {
        $target = Resolve-Ideapad
        $config = New-SshConfig $target
        try {
            Get-RemoteSlots $config
        } finally {
            Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
        }
        exit 0
    } catch {
        if ($env:WC3_DIST_DEBUG -eq '1') {
            Write-Warning "[dist] IDEAPAD slot probe failed: $($_.Exception.Message)"
        }
        exit 3
    }
}

if ($Action -eq 'stop') {
    if (-not (Get-State) -and $useIdeapad) {
        # No local state (crashed build or reboot): still try to remove a
        # stale remote daemon through a fresh dynamic resolution.
        try {
            $target = Resolve-Ideapad
            $config = New-SshConfig $target
            Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
            Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
        } catch { }
    }
    Stop-Tunnel -RemoteCleanup
    Write-Info 'tunnel stopped'
    exit 0
}

# start: replace a stale tunnel, then bring up a fresh daemon.
$existing = Get-State
if ($existing -and -not $Force) {
    $existingPid = 0
    if ([int]::TryParse([string]$existing.Pid, [ref]$existingPid) -and
        $existingPid -gt 0) {
        $existingProcess = Get-Process -Id $existingPid `
            -ErrorAction SilentlyContinue
        if ($existingProcess) {
            throw ("a distcc tunnel is already active on port {0} (ssh pid {1}, started {2}); " +
                'refusing to replace it. Stop the owning build first, or pass -Force.') -f
                $existing.Port, $existing.Pid, $existing.Started
        }
    }
}
Stop-Tunnel -RemoteCleanup
if (-not (Test-Path -LiteralPath $stateDir)) {
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
}

$target = ''
$config = ''
$targetSlots = 0
$port = 0
$process = $null
$ideapadStarted = $false
if ($useIdeapad) {
    try {
        if (-not (Test-Path -LiteralPath $ssh)) {
            throw "ssh not found: $ssh"
        }
        if (-not (Test-Path -LiteralPath $key)) {
            throw "ssh key not found: $key"
        }
        $target = Resolve-Ideapad
        $config = New-SshConfig $target
        # A previous manual test or a crashed build may have left distccd
        # running; remove it before binding the remote port below.
        try {
            Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
        } catch { }
        $targetSlots = if ($RemoteSlots -gt 0) {
            $RemoteSlots
        } else {
            Get-RemoteSlots $config
        }
        if ($targetSlots -le 0) {
            throw 'IDEAPAD reported no compiler slots; pass -RemoteSlots'
        }
        $port = Get-FreeLoopbackPort -RequestedPort $LocalPort

        $logBase = Join-Path $stateDir ("distccd-{0}" -f $PID)
        $stdoutLog = "$logBase.out.log"
        $stderrLog = "$logBase.err.log"
        $remoteDaemon = @(
            "$RemoteRoot/bin/distccd.exe",
            '--no-detach',
            '--listen', '127.0.0.1',
            '--port', "$RemotePort",
            '--allow', '127.0.0.1',
            '--enable-tcp-insecure',
            '--jobs', "$targetSlots",
            "--log-file=$RemoteRoot/distccd-tunnel.log",
            '--log-level=info'
        ) -join ' '

        $sshArgs = @(
            '-F', $config,
            '-o', 'BatchMode=yes',
            '-o', 'ExitOnForwardFailure=yes',
            '-o', 'ConnectTimeout=8',
            '-o', 'ServerAliveInterval=15',
            '-L', ('127.0.0.1:{0}:127.0.0.1:{1}' -f $port, $RemotePort),
            $sshAlias,
            $remoteDaemon
        )
        $process = Start-Process -FilePath $ssh -ArgumentList $sshArgs `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $ready = $false
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 250
            if ($process.HasExited) { break }
            $listener = Get-NetTCPConnection -LocalPort $port -State Listen `
                -ErrorAction SilentlyContinue
            if ($listener) { $ready = $true; break }
        }
        if (-not $ready) {
            $stderr = if (Test-Path -LiteralPath $stderrLog) {
                Get-Content -LiteralPath $stderrLog -Raw
            } else { '' }
            throw "distcc tunnel did not come up`n$stderr"
        }
        $ideapadStarted = $true
        Write-Info ("IDEAPAD target={0} local_port={1} slots={2}" -f `
            $target, $port, $targetSlots)
    } catch {
        $message = $_.Exception.Message -replace '\s+', ' '
        Write-Warning ("[distcc] IDEAPAD unavailable: {0}" -f $message)
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        if ($config -and (Test-Path -LiteralPath $config)) {
            try {
                Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' |
                    Out-Null
            } catch { }
            Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
        }
        $target = ''
        $config = ''
        $targetSlots = 0
        $port = 0
        $process = $null
    }
}

$pgoSynced = $false
$pgoRemote = ''
if ($ideapadStarted -and $PgoProfile) {
    try {
        $profilePath = (Resolve-Path -LiteralPath $PgoProfile).Path
        if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
            throw "profile is not a file: $profilePath"
        }
        if (-not (Test-Path -LiteralPath $scp)) {
            $scpCommand = Get-Command scp.exe -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if (-not $scpCommand) { throw "scp not found: $scp" }
            $scp = $scpCommand.Source
        }
        # Use the same absolute path on the peer.  This keeps the distcc
        # command line byte-for-byte identical and lets its local preprocessor
        # open the profile even though the remote compiler reads it later.
        $pgoRemote = $profilePath -replace '\\', '/'
        $remoteDir = (Split-Path -Parent $profilePath) -replace '/', '\'
        # Let cmd create the parent chain.  An already existing directory makes
        # mkdir return 1; its stderr is suppressed and scp below is the real
        # check that the path is usable.
        $mk = 'cmd /c mkdir "{0}" 2>nul' -f $remoteDir
        Invoke-Remote $config $mk | Out-Null
        & $scp -F $config -o BatchMode=yes -o ConnectTimeout=8 `
            -o StrictHostKeyChecking=accept-new $profilePath `
            ("{0}:{1}" -f $sshAlias, $pgoRemote) 2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "scp failed with exit code $LASTEXITCODE"
        }
        $pgoSynced = $true
        Write-Info ("PGO profile synced to {0}" -f $pgoRemote)
    } catch {
        Write-Warning (("[distcc] PGO profile sync failed: {0}; " +
            'profile-use compiles stay local') -f $_.Exception.Message)
        $pgoSynced = $false
        $pgoRemote = ''
    }
}

$phone = $null
$phonePort = 0
$phoneSlots = 0
$phoneTarget = ''
if ($usePhone) {
    if (Test-Path -LiteralPath $phoneTunnelScript) {
        try {
            $phoneOutput = & $phoneTunnelScript -Action start `
                -Slots $PhoneSlots -LocalPort $PhoneLocalPort -Quiet:$Quiet
            $phoneJson = @($phoneOutput | Where-Object { $_ -match '^\s*\{' }) |
                Select-Object -Last 1
            if (-not $phoneJson) {
                throw 'phone tunnel returned no state'
            }
            $phone = $phoneJson | ConvertFrom-Json
            $phonePort = [int]$phone.Port
            $phoneSlots = [int]$phone.Slots
            $phoneTarget = [string]$phone.Target
            Write-Info ("phone target={0} source={1} local_port={2} slots={3}" -f `
                $phone.Target, $phone.Source, $phone.Port, $phone.Slots)
        } catch {
            $message = $_.Exception.Message -replace '\s+', ' '
            Write-Warning ("[distcc] phone worker unavailable: {0}" -f $message)
            $phone = $null
            $phonePort = 0
            $phoneSlots = 0
            $phoneTarget = ''
        }
    } else {
        Write-Warning ("[distcc] phone tunnel script not found: {0}" -f $phoneTunnelScript)
    }
}

$state = [pscustomobject]@{
    Pid              = if ($process) { $process.Id } else { 0 }
    OwnerPid         = $PID
    Port             = $port
    RemotePort       = $RemotePort
    LocalSlots       = $LocalSlots
    RemoteSlots      = $targetSlots
    Config           = $config
    Target           = $target
    Started          = (Get-Date).ToString('o')
    PgoProfile       = $pgoRemote
    PgoProfileSynced = $pgoSynced
    Phone            = $phone
    PhonePort        = $phonePort
    PhoneSlots       = $phoneSlots
    PhoneTarget      = $phoneTarget
}
$phoneText = if ($phoneSlots -gt 0) {
    "{0}@127.0.0.1:{1}/{2}" -f $phoneTarget, $phonePort, $phoneSlots
} else { 'off' }
$ideapadText = if ($targetSlots -gt 0) {
    "{0}@127.0.0.1:{1}/{2}" -f $target, $port, $targetSlots
} else { 'off' }
if ($targetSlots + $phoneSlots -le 0) {
    Write-Warning '[distcc] no workers available; the caller will build locally'
    $state | ConvertTo-Json -Compress -Depth 4
    exit 0
}
$state | ConvertTo-Json -Compress -Depth 4 |
    Set-Content -LiteralPath $statePath -Encoding ascii
Write-Info ("ready ideapad={0} phone={1} local_slots={2}" -f `
    $ideapadText, $phoneText, $LocalSlots)
$state | ConvertTo-Json -Compress -Depth 4
