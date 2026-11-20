# Run the canonical Android build with ccache misses compiled on every
# available distributed worker: IDEAPAD and/or the hotspot phone.
#
# This is an opt-in accelerator, not a second build pipeline: it sets
# CCACHE_PREFIX before calling tools/android/build_app.ps1, so ccache keeps
# owning the cache and the build graph.
#
# Capacity model (distcc transport):
#   * the local machine contributes ProcessorCount compiler slots;
#   * IDEAPAD contributes its own logical CPU count as slots;
#   * the hotspot phone contributes its boosted top-app CPU count, or nproc
#     when Termux cannot use root to reach the big cores;
#   * Ninja starts local plus every available worker, distcc fills the
#     workers, and runs at most the local slot count on this machine.
# Either worker is optional: -NoIdeapad, -NoPhone, or both.  If neither
# worker is available the canonical build runs locally.  release is the
# optimized product and uses IDEAPAD by default; it excludes the phone because
# the phone has no merged PGO profile.  Set WC3_DIST_PHONE_OPTIMIZED=1 to
# include the phone.
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Variant = 'release',
    [ValidateSet('distcc', 'ssh')]
    [string]$Transport = 'distcc',
    [int]$Jobs = 0,
    [int]$LocalSlots = 0,
    [int]$RemoteSlots = 0,
    [int]$RemotePort = 3632,
    # Merged profile for a final release build.  The distcc tunnel copies it
    # to IDEAPAD; the SSH transport leaves profile-use local.
    [string]$PgoProfile = '',
    # PGO mode forwarded to build_app.ps1.  Empty uses the build_app default
    # (release -> use, debug -> off).  The training pass is -Pgo generate.
    [string]$Pgo = '',
    # Legacy name for the custom SSH transport's remote slot count.
    [int]$Slots = 0,
    # Either worker can be skipped; the other one is still used if available.
    [switch]$NoIdeapad,
    [switch]$NoPhone,
    [int]$PhoneSlots = 0,
    [int]$PhoneLocalPort = 0,
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
# build_app.ps1 defaults release to -Pgo use.  Mirror that here so the tunnel
# copies the merged profile for the final build, but never require it for the
# internal training pass.
$buildPgo = $Pgo
$hasPgoArg = $false
$buildArgList = @($BuildArgs)
for ($i = 0; $i -lt $buildArgList.Count; $i++) {
    $arg = [string]$buildArgList[$i]
    if ($arg -match '^-Pgo:(?<value>off|generate|use)$') {
        $buildPgo = $Matches.value
        $hasPgoArg = $true
    } elseif ($arg -eq '-Pgo' -and $i + 1 -lt $buildArgList.Count) {
        $buildPgo = [string]$buildArgList[$i + 1]
        $hasPgoArg = $true
    }
}
if (-not $buildPgo) {
    $buildPgo = if ($Variant -eq 'release') { 'use' } else { 'off' }
}
if ($buildPgo -notin @('off', 'generate', 'use')) {
    throw "invalid PGO mode '$buildPgo'; expected off, generate, or use"
}
if ($Variant -eq 'release' -and $buildPgo -eq 'use' -and -not $PgoProfile) {
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
$allowPgo = $Variant -eq 'release' -and $buildPgo -ne 'off'
$allowLto = $Variant -eq 'release' -and $buildPgo -ne 'generate'
$useIdeapad = -not $NoIdeapad
if ($env:WC3_DIST_IDEAPAD -eq '0') { $useIdeapad = $false }
$usePhone = -not $NoPhone
if ($env:WC3_DIST_PHONE -eq '0') { $usePhone = $false }
if ($Variant -eq 'release' -and
    $env:WC3_DIST_PHONE_OPTIMIZED -ne '1') {
    $usePhone = $false
}

$tunnel = $null
$distccState = $null
$tunnelScript = Join-Path $PSScriptRoot 'distcc_tunnel.ps1'
$phoneSlots = 0
$phonePort = 0
$phoneTarget = ''
$started = Get-Date

try {
    if ($Transport -eq 'distcc') {
        $tunnelJson = & $tunnelScript -Action start -LocalSlots $LocalSlots `
            -RemoteSlots $RemoteSlots -RemotePort $RemotePort `
            -PgoProfile $PgoProfile -NoIdeapad:(-not $useIdeapad) `
            -NoPhone:(-not $usePhone) -PhoneSlots $PhoneSlots `
            -PhoneLocalPort $PhoneLocalPort
        $tunnel = $tunnelJson | ConvertFrom-Json
        if (-not $tunnel) { throw 'distcc tunnel returned no state' }
        # The state is authoritative: a requested worker may be unavailable.
        $RemoteSlots = [int]$tunnel.RemoteSlots
        if ($tunnel.PhoneSlots) { $phoneSlots = [int]$tunnel.PhoneSlots }
        if ($tunnel.PhonePort) { $phonePort = [int]$tunnel.PhonePort }
        if ($tunnel.PhoneTarget) { $phoneTarget = [string]$tunnel.PhoneTarget }

        $remoteTotal = $RemoteSlots + $phoneSlots
        if ($remoteTotal -le 0) {
            # Both workers are optional.  With neither one this is still the
            # canonical build, just local.
            Remove-Item Env:\DISTCC_HOSTS -ErrorAction SilentlyContinue
            Remove-Item Env:\CCACHE_PREFIX -ErrorAction SilentlyContinue
            Remove-Item Env:\WC3_DIST_TRANSPORT -ErrorAction SilentlyContinue
            $env:WC3_DIST = '0'
            $env:WC3_DIST_LOCAL_SLOTS = "$LocalSlots"
            if ($Jobs -le 0) { $Jobs = $LocalSlots }
            Write-Warning '[dist] no workers available; building locally'
        } else {
            if ($Jobs -le 0) {
                # Both variants have distributable objects.  In debug the
                # corpus objects end in -g0 and run remotely; -g commands use
                # the localhost-only host list and remain capped by LocalSlots,
                # so local plus every worker does not raise the local limit.
                $Jobs = $LocalSlots + $RemoteSlots + $phoneSlots
            }
            $env:WC3_DIST = '1'
            $env:WC3_DIST_TRANSPORT = 'distcc'
            $env:WC3_DIST_LOCAL_SLOTS = "$LocalSlots"
            $env:WC3_DISTCC_EXE = 'C:/msys64/usr/bin/distcc.exe'
            $env:CCACHE_PREFIX = ('{0} {1}' -f ($python -replace '\\', '/'),
                                  ($wrapper -replace '\\', '/'))
            $hosts = @()
            if ($RemoteSlots -gt 0) {
                $hosts += '127.0.0.1:{0}/{1}' -f $tunnel.Port, $RemoteSlots
            }
            if ($phoneSlots -gt 0) {
                $hosts += '127.0.0.1:{0}/{1}' -f $phonePort, $phoneSlots
            }
            $hosts += 'localhost/{0} --localslots={0} --localslots_cpp={0}' -f $LocalSlots
            $env:DISTCC_HOSTS = $hosts -join ' '
            $distccState = Join-Path $env:LOCALAPPDATA (
                'wc3-dist\distcc-state-{0}-{1}' -f $PID, $started.ToString('HHmmss'))
            New-Item -ItemType Directory -Force -Path $distccState | Out-Null
            $env:DISTCC_DIR = $distccState
            $env:TMPDIR = $env:TEMP
        }
    } else {
        # The legacy SSH transport only knows IDEAPAD.  With IDEAPAD disabled
        # there is no worker for this transport, so keep the build canonical
        # and local instead of silently dialing the disabled peer.
        if (-not $useIdeapad) {
            Remove-Item Env:\DISTCC_HOSTS -ErrorAction SilentlyContinue
            Remove-Item Env:\CCACHE_PREFIX -ErrorAction SilentlyContinue
            Remove-Item Env:\WC3_DIST_TRANSPORT -ErrorAction SilentlyContinue
            $env:WC3_DIST = '0'
            $RemoteSlots = 0
            if ($Jobs -le 0) { $Jobs = $LocalSlots }
            Write-Warning '[dist] the SSH transport needs IDEAPAD; building locally'
        } else {
            if ($RemoteSlots -le 0) {
                $slotText = & $tunnelScript -Action slots 2>$null
                $slotLine = $slotText | Select-Object -Last 1
                $parsedSlots = 0
                if ($slotLine -and [int]::TryParse(([string]$slotLine).Trim(), [ref]$parsedSlots) -and $parsedSlots -gt 0) {
                    $RemoteSlots = $parsedSlots
                } else {
                    throw ('could not read IDEAPAD slot count; ' +
                        'pass -RemoteSlots or -Slots')
                }
            }
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

    $remoteTotal = $RemoteSlots + $phoneSlots
    $share = if ($remoteTotal + $LocalSlots -gt 0) {
        [math]::Round(100.0 * $remoteTotal / ($remoteTotal + $LocalSlots))
    } else { 0 }
    Write-Host ("[dist] transport={0} local_slots={1} remote_slots={2} phone_slots={3} jobs={4} remote_cpu_share={5}% pgo={6} lto={7}" -f
        $Transport, $LocalSlots, $RemoteSlots, $phoneSlots, $Jobs, $share,
        $(if ($allowPgo) { 'on' } else { 'off' }),
        $(if ($allowLto) { 'on' } else { 'off' }))
    $prefixText = if ($env:CCACHE_PREFIX) { $env:CCACHE_PREFIX } else { 'off' }
    Write-Host "[dist] ccache_prefix=$prefixText"
    if ($tunnel) {
        $phoneLog = if ($phoneSlots -gt 0) {
            '{0}@127.0.0.1:{1}/{2}' -f $phoneTarget, $phonePort, $phoneSlots
        } else { 'off' }
        $ideapadLog = if ($RemoteSlots -gt 0) {
            '{0}@127.0.0.1:{1}/{2}' -f $tunnel.Target, $tunnel.Port, $RemoteSlots
        } else { 'off' }
        Write-Host ("[dist] workers ideapad={0} phone={1}" -f `
            $ideapadLog, $phoneLog)
    }

    if ($DryRun) {
        if ($remoteTotal -gt 0) {
            Write-Host '[dist] dry run: tunnel and environment are ready; build not started'
        } else {
            Write-Host '[dist] dry run: no workers available; build would run locally'
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
        if ($Pgo -and -not $hasPgoArg) {
            $appArgs['Pgo'] = $Pgo
        }
        & (Join-Path $root 'tools\android\build_app.ps1') -Variant $Variant `
            @appArgs @BuildArgs
        $code = $LASTEXITCODE
    }
} finally {
    if ($Transport -eq 'distcc' -and -not $KeepTunnel) {
        & (Join-Path $PSScriptRoot 'distcc_tunnel.ps1') -Action stop `
            -NoIdeapad:(-not $useIdeapad) -Quiet | Out-Null
    }
    if ($distccState -and (Test-Path -LiteralPath $distccState)) {
        Remove-Item -LiteralPath $distccState -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit $code
