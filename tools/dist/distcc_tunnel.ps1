# Start or stop the IDEAPAD distcc worker and its TCP-over-SSH tunnel.
#
# The tunnel binds a free loopback port on this machine and forwards to the
# remote distccd listening on 127.0.0.1.  The remote processor count decides
# how many distccd workers are started, so the local scheduler can use the
# whole machine without hardcoding its size.
#
#   $t = .\tools\dist\distcc_tunnel.ps1 -Action start
#   ...
#   .\tools\dist\distcc_tunnel.ps1 -Action stop
#
# The start action prints one JSON object with Port, RemoteSlots, LocalSlots
# and Pid.  State lives in %LOCALAPPDATA%\wc3-dist\distcc-tunnel.json.
[CmdletBinding()]
param(
    [ValidateSet('start', 'stop', 'status')]
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
    # Merged profile for a final release_optimized build.  It is copied to the
    # same absolute path on IDEAPAD, so the command line stays identical on
    # both sides and distcc's local preprocessor still sees a real file.
    [string]$PgoProfile = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ssh = 'C:\Program Files\Git\usr\bin\ssh.exe'
$scp = 'C:\Program Files\Git\usr\bin\scp.exe'
$key = Join-Path $env:USERPROFILE '.ssh\id_ideapad'
$stateDir = Join-Path $env:LOCALAPPDATA 'wc3-dist'
$statePath = Join-Path $stateDir 'distcc-tunnel.json'
$sshAlias = 'ideapad-distcc'

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
    $value = & $ssh -F $Config -o BatchMode=yes -o ConnectTimeout=8 `
        $sshAlias 'echo %NUMBER_OF_PROCESSORS%' 2>$null
    $parsed = 0
    if ([int]::TryParse((($value | Select-Object -Last 1).Trim()), [ref]$parsed) -and $parsed -gt 0) {
        return $parsed
    }
    throw 'could not read NUMBER_OF_PROCESSORS from IDEAPAD; pass -RemoteSlots'
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
    if (-not $state) { return }
    $pidValue = [int]$state.Pid
    $process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
    if ($process) {
        Stop-Process -Id $pidValue -Force -ErrorAction SilentlyContinue
        try { $process.WaitForExit(5000) | Out-Null } catch { }
    }
    if ($RemoteCleanup -and (Test-Path -LiteralPath $state.Config)) {
        try {
            Invoke-Remote $state.Config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
        } catch { }
    }
    if (Test-Path -LiteralPath $state.Config) {
        Remove-Item -LiteralPath $state.Config -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
}

if ($Action -eq 'status') {
    $state = Get-State
    if ($state) { $state | ConvertTo-Json -Compress } else { '{}' }
    exit 0
}

if ($Action -eq 'stop') {
    if (Get-State) {
        Stop-Tunnel -RemoteCleanup
    } else {
        # No local state (crashed build or reboot): still try to remove a
        # stale remote daemon through a fresh dynamic resolution.
        try {
            $target = Resolve-Ideapad
            $config = New-SshConfig $target
            Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
            Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
        } catch { }
    }
    Write-Info 'tunnel stopped'
    exit 0
}

# start: replace a stale tunnel, then bring up a fresh daemon.
$existing = Get-State
if ($existing -and -not $Force) {
    $existingProcess = Get-Process -Id ([int]$existing.Pid) `
        -ErrorAction SilentlyContinue
    if ($existingProcess) {
        throw ("a distcc tunnel is already active on port {0} (ssh pid {1}, started {2}); " +
            'refusing to replace it. Stop the owning build first, or pass -Force.') -f
            $existing.Port, $existing.Pid, $existing.Started
    }
}
Stop-Tunnel -RemoteCleanup
if (-not (Test-Path -LiteralPath $ssh)) { throw "ssh not found: $ssh" }
if (-not (Test-Path -LiteralPath $key)) { throw "ssh key not found: $key" }
if (-not (Test-Path -LiteralPath $stateDir)) {
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
}

$target = Resolve-Ideapad
$config = New-SshConfig $target
# A previous manual test or a crashed build may have left distccd running;
# remove it before binding the remote port below.
try {
    Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
} catch { }
try {
    $targetSlots = if ($RemoteSlots -gt 0) { $RemoteSlots } else { Get-RemoteSlots $config }
} catch {
    Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
    throw
}
if ($targetSlots -le 0) {
    Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
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
$process = Start-Process -FilePath $ssh -ArgumentList $sshArgs -PassThru `
    -WindowStyle Hidden -RedirectStandardOutput $stdoutLog `
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
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    try {
        Invoke-Remote $config 'taskkill /F /IM distccd.exe >nul 2>&1' | Out-Null
    } catch { }
    $stderr = if (Test-Path -LiteralPath $stderrLog) {
        Get-Content -LiteralPath $stderrLog -Raw
    } else { '' }
    Remove-Item -LiteralPath $config -Force -ErrorAction SilentlyContinue
    throw "distcc tunnel did not come up`n$stderr"
}

$pgoSynced = $false
$pgoRemote = ''
if ($PgoProfile) {
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

$state = [pscustomobject]@{
    Pid              = $process.Id
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
}
$state | ConvertTo-Json -Compress | Set-Content -LiteralPath $statePath -Encoding ascii
Write-Info ("ready target={0} local_port={1} local_slots={2} remote_slots={3}" -f `
    $target, $port, $LocalSlots, $targetSlots)
$state | ConvertTo-Json -Compress
