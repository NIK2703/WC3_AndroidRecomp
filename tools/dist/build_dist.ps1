# Run the canonical Android build with ccache misses compiled on IDEAPAD.
#
# This is an opt-in accelerator, not a second build pipeline: it sets
# CCACHE_PREFIX before calling tools/android/build_app.ps1, so ccache keeps
# owning the cache and the build graph.
#
# Capacity model (distcc transport):
#   * the local machine contributes ProcessorCount compiler slots;
#   * IDEAPAD contributes its own NUMBER_OF_PROCESSORS slots;
#   * Ninja starts local+remote jobs, distcc fills the remote slots first and
#     runs at most the local slot count on this machine.
# The faster machine therefore receives proportionally more work instead of a
# fixed 50/50 split, and the local compiler concurrency never exceeds its
# processor count.
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug', 'release_optimized', 'release_optimized_pgo')]
    [string]$Variant = 'release',
    [ValidateSet('distcc', 'ssh')]
    [string]$Transport = 'distcc',
    [int]$Jobs = 0,
    [int]$LocalSlots = 0,
    [int]$RemoteSlots = 0,
    [int]$RemotePort = 3632,
    # Merged profile for a final release_optimized build.  The distcc tunnel
    # copies it to IDEAPAD; the SSH transport leaves profile-use local.
    [string]$PgoProfile = '',
    # Legacy name for the custom SSH transport's remote slot count.
    [int]$Slots = 0,
    [switch]$KeepTunnel,
    # Validate environment/tunnel setup without invoking build_app.ps1.
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildArgs
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
# The MSYS2 UCRT interpreter is the one used for the verified distcc prefix;
# fall back to PATH only when the toolchain is installed elsewhere.
$msysPython = 'C:\msys64\ucrt64\bin\python.exe'
if (Test-Path -LiteralPath $msysPython) {
    $python = $msysPython
} else {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $pythonCommand) { throw 'python.exe not found in PATH' }
    $python = $pythonCommand.Source
}
$wrapper = Join-Path $root 'tools\dist\wc3_dist_cc.py'
if (-not (Test-Path -LiteralPath $wrapper)) {
    throw "distributed compiler wrapper not found: $wrapper"
}

if ($LocalSlots -le 0) { $LocalSlots = [Environment]::ProcessorCount }
if ($LocalSlots -gt [Environment]::ProcessorCount) {
    $LocalSlots = [Environment]::ProcessorCount
}
if ($Slots -gt 0 -and $RemoteSlots -le 0) { $RemoteSlots = $Slots }
# The canonical final optimized build consumes this profile.  Default it here
# as well so an explicit build_dist invocation gets the same remote profile
# copy as the automatic build_app path.
if ($Variant -eq 'release_optimized' -and -not $PgoProfile) {
    $PgoProfile = 'build/pgo/wc3.profdata'
}
if ($PgoProfile) {
    $profileCandidate = if ([IO.Path]::IsPathRooted($PgoProfile)) {
        $PgoProfile
    } else {
        Join-Path $root $PgoProfile
    }
    if (-not (Test-Path -LiteralPath $profileCandidate -PathType Leaf)) {
        throw "PGO profile not found: $profileCandidate"
    }
    $PgoProfile = (Resolve-Path -LiteralPath $profileCandidate).Path
}
$allowPgo = $Variant -in @('release_optimized', 'release_optimized_pgo')
$allowLto = $Variant -eq 'release_optimized'

$tunnel = $null
$distccState = $null
$started = Get-Date

try {
    if ($Transport -eq 'distcc') {
        $tunnelScript = Join-Path $PSScriptRoot 'distcc_tunnel.ps1'
        $tunnelJson = & $tunnelScript -Action start -LocalSlots $LocalSlots `
            -RemoteSlots $RemoteSlots -RemotePort $RemotePort `
            -PgoProfile $PgoProfile
        $tunnel = $tunnelJson | ConvertFrom-Json
        if ($RemoteSlots -le 0) { $RemoteSlots = [int]$tunnel.RemoteSlots }
        if ($RemoteSlots -le 0) { throw 'IDEAPAD reported no compiler slots' }

        if ($Jobs -le 0) {
            # Both variants have distributable objects.  In debug the corpus
            # objects end in -g0 and run remotely; -g commands use the
            # localhost-only host list and remain capped by LocalSlots, so the
            # total local+remote jobs does not raise the local compiler limit.
            $Jobs = $LocalSlots + $RemoteSlots
        }
        $env:WC3_DIST = '1'
        $env:WC3_DIST_TRANSPORT = 'distcc'
        $env:WC3_DIST_LOCAL_SLOTS = "$LocalSlots"
        $env:WC3_DISTCC_EXE = 'C:/msys64/usr/bin/distcc.exe'
        $env:CCACHE_PREFIX = ('{0} {1}' -f ($python -replace '\\', '/'),
                              ($wrapper -replace '\\', '/'))
        $env:DISTCC_HOSTS = (
            '127.0.0.1:{0}/{1} localhost/{2} --localslots={2} --localslots_cpp={2}' -f
            $tunnel.Port, $RemoteSlots, $LocalSlots)
        $distccState = Join-Path $env:LOCALAPPDATA (
            'wc3-dist\distcc-state-{0}-{1}' -f $PID, $started.ToString('HHmmss'))
        New-Item -ItemType Directory -Force -Path $distccState | Out-Null
        $env:DISTCC_DIR = $distccState
        $env:TMPDIR = $env:TEMP
    } else {
        if ($RemoteSlots -le 0) { $RemoteSlots = 8 }
        if ($Jobs -le 0) {
            $Jobs = $LocalSlots + $RemoteSlots
        }
        $env:WC3_DIST = '1'
        $env:WC3_DIST_TRANSPORT = 'ssh'
        $env:CCACHE_PREFIX = ('{0} {1}' -f ($python -replace '\\', '/'),
                              ($wrapper -replace '\\', '/'))
        $env:WC3_DIST_SLOTS = "$RemoteSlots"
        Remove-Item Env:\DISTCC_HOSTS -ErrorAction SilentlyContinue
    }

    $pgoReady = $false
    if ($tunnel) {
        $pgoProperty = $tunnel.PSObject.Properties['PgoProfileSynced']
        if ($pgoProperty) { $pgoReady = [bool]$pgoProperty.Value }
    }
    $env:WC3_DIST_ALLOW_PGO = if ($allowPgo) { '1' } else { '0' }
    $env:WC3_DIST_ALLOW_LTO = if ($allowLto) { '1' } else { '0' }
    $env:WC3_DIST_PGO_PROFILE_READY = if ($pgoReady) { '1' } else { '0' }
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
    # build_app.ps1 must not probe or replace the transport this caller owns.
    $env:WC3_DIST_ACTIVE = '1'

    $share = if ($RemoteSlots + $LocalSlots -gt 0) {
        [math]::Round(100.0 * $RemoteSlots / ($RemoteSlots + $LocalSlots))
    } else { 0 }
    Write-Host ("[dist] transport={0} local_slots={1} remote_slots={2} jobs={3} remote_cpu_share={4}% pgo={5} lto={6}" -f
        $Transport, $LocalSlots, $RemoteSlots, $Jobs, $share,
        $(if ($allowPgo) { 'on' } else { 'off' }),
        $(if ($allowLto) { 'on' } else { 'off' }))
    Write-Host "[dist] ccache_prefix=$env:CCACHE_PREFIX"
    if ($tunnel) {
        Write-Host ("[dist] tunnel local_port={0} target={1}" -f $tunnel.Port, $tunnel.Target)
    }

    if ($DryRun) {
        if ($tunnel) {
            Write-Host '[dist] dry run: tunnel and environment are ready; build not started'
        } else {
            Write-Host '[dist] dry run: environment is ready; build not started'
        }
        $code = 0
    } else {
        $appArgs = @{}
        $hasProfileArg = @($BuildArgs | Where-Object {
            $_ -match '^-PgoProfile(:|$)'
        }).Count -gt 0
        if ($PgoProfile -and -not $hasProfileArg) {
            $appArgs['PgoProfile'] = $PgoProfile
        }
        & (Join-Path $root 'tools\android\build_app.ps1') -Variant $Variant `
            @appArgs @BuildArgs
        $code = $LASTEXITCODE
    }
} finally {
    if ($Transport -eq 'distcc' -and -not $KeepTunnel) {
        & (Join-Path $PSScriptRoot 'distcc_tunnel.ps1') -Action stop -Quiet | Out-Null
    }
    if ($distccState -and (Test-Path -LiteralPath $distccState)) {
        Remove-Item -LiteralPath $distccState -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit $code
