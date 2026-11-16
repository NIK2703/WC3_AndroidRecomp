# Auto-enable the IDEAPAD distcc worker for a normal native build.
#
# Dot-source this file (it has no side effects).  The normal entry point is the
# one-call trampoline used by build_app.ps1:
#
#   . (Join-Path $root 'tools\dist\distributed_env.ps1')
#   Invoke-Wc3AutoDistributedBuild -ScriptPath $PSCommandPath `
#       -Parameters $PSBoundParameters -Root $root -Variant $Variant
#
# If IDEAPAD is reachable, that function starts the tunnel and re-runs the
# calling script with WC3_DIST_ACTIVE=1; the original body runs unchanged in the
# child scope and the tunnel is stopped in finally when the child exits.  If
# distribution is unavailable, the function returns and the same script body
# runs locally.  Enable-Wc3DistributedBuild/Disable-Wc3DistributedBuild remain
# available for an explicit try/finally caller.
#
# Enable returns $null when distribution is disabled, when a caller already
# configured a transport, or when IDEAPAD cannot be reached.  A null result is
# a normal local build, not an error.  All four native variants are eligible:
# release/debug distribute ordinary corpus compiles, while the optimized
# variants additionally allow ThinLTO and PGO commands.  A final
# release_optimized build passes its merged profile to the tunnel, which copies
# it to the same absolute path on IDEAPAD before the first profile-use compile.
#
# Environment toggles:
#   WC3_DIST_AUTO=0      force a local build
#   WC3_DIST_JOBS=N      override Ninja's total job count
#   WC3_DIST=1           caller already owns the transport (build_dist.ps1)

function Get-Wc3DistPython {
    param([string]$Root)
    $msys = 'C:\msys64\ucrt64\bin\python.exe'
    if (Test-Path -LiteralPath $msys) { return $msys }
    $command = Get-Command python -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) { return $command.Source }
    return $null
}

function Set-Wc3DistEnv {
    param(
        [hashtable]$Previous,
        [string]$Name,
        [string]$Value
    )
    if (-not $Previous.ContainsKey($Name)) {
        $Previous[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
    }
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}

function Restore-Wc3DistEnv {
    param(
        [hashtable]$Previous,
        [string]$Name
    )
    if ($null -eq $Previous -or -not $Previous.ContainsKey($Name)) { return }
    $value = $Previous[$Name]
    if ($null -eq $value -or $value -eq '') {
        Remove-Item -LiteralPath "Env:\$Name" -ErrorAction SilentlyContinue
    } else {
        [Environment]::SetEnvironmentVariable($Name, $value, 'Process')
    }
}

function Remove-Wc3DistccState {
    param([string]$Path)
    if (-not $Path) { return }
    # A short-lived distcc client can still hold the directory when teardown
    # starts.  Retry briefly so a completed build does not leave state behind.
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        if (-not (Test-Path -LiteralPath $Path)) { return }
        Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $Path) {
            Start-Sleep -Milliseconds 200
        }
    }
}

function Enable-Wc3DistributedBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Variant = 'release',
        [int]$LocalSlots = 0,
        [int]$RemoteSlots = 0,
        [int]$RemotePort = 3632,
        # Merged PGO profile for the final release_optimized build.  The
        # tunnel copies it to the same absolute path on IDEAPAD; an empty value
        # leaves profile-use compiles on the local-only path.
        [string]$PgoProfile = ''
    )

    if ($env:WC3_DIST_ACTIVE -eq '1' -or $env:WC3_DIST -eq '1') {
        return $null
    }
    if ($env:WC3_DIST_AUTO -eq '0') {
        return $null
    }
    if ($Variant -notin @('release', 'debug',
            'release_optimized', 'release_optimized_pgo')) {
        return $null
    }
    $allowPgo = $Variant -in @('release_optimized', 'release_optimized_pgo')
    $allowLto = $Variant -eq 'release_optimized'
    if ($env:CCACHE_PREFIX -and
        $env:CCACHE_PREFIX -notmatch 'wc3_dist_cc\.py') {
        Write-Warning '[dist] CCACHE_PREFIX is already set; building locally (set WC3_DIST=1 to own the transport)'
        return $null
    }

    $requestedJobs = 0
    if ($env:WC3_DIST_JOBS) {
        if (-not [int]::TryParse($env:WC3_DIST_JOBS, [ref]$requestedJobs) -or
            $requestedJobs -le 0) {
            Write-Warning "[dist] WC3_DIST_JOBS is not a positive integer: $env:WC3_DIST_JOBS; building locally"
            return $null
        }
    }

    $distcc = 'C:\msys64\usr\bin\distcc.exe'
    if (-not (Test-Path -LiteralPath $distcc)) {
        Write-Warning "[dist] $distcc not found; building locally"
        return $null
    }
    $python = Get-Wc3DistPython -Root $Root
    if (-not $python) {
        Write-Warning '[dist] python not found; building locally'
        return $null
    }
    $wrapper = Join-Path $Root 'tools\dist\wc3_dist_cc.py'
    $tunnelScript = Join-Path $Root 'tools\dist\distcc_tunnel.ps1'
    if (-not (Test-Path -LiteralPath $wrapper) -or
        -not (Test-Path -LiteralPath $tunnelScript)) {
        Write-Warning '[dist] distributed build scripts are missing; building locally'
        return $null
    }

    if ($LocalSlots -le 0) {
        $LocalSlots = [Environment]::ProcessorCount
    }
    $LocalSlots = [Math]::Min([Math]::Max(1, $LocalSlots),
        [Environment]::ProcessorCount)

    $tunnel = $null
    $distccState = $null
    $previous = @{}
    try {
        $tunnelJson = & $tunnelScript -Action start `
            -LocalSlots $LocalSlots -RemoteSlots $RemoteSlots `
            -RemotePort $RemotePort -PgoProfile $PgoProfile
        $tunnel = $tunnelJson | ConvertFrom-Json
        if (-not $tunnel -or [int]$tunnel.RemoteSlots -le 0) {
            throw 'tunnel returned no remote slots'
        }
        $remote = [int]$tunnel.RemoteSlots
        $pgoReady = $false
        $pgoProperty = $tunnel.PSObject.Properties['PgoProfileSynced']
        if ($pgoProperty) { $pgoReady = [bool]$pgoProperty.Value }
        # Both variants have distributable work.  In debug the corpus commands
        # end in -g0 and go to IDEAPAD; the -g commands stay on the local-only
        # distcc path and are capped by LocalSlots, so a total of local+remote
        # Ninja jobs fills the remote slots without lifting the local cap.
        $jobs = if ($requestedJobs -gt 0) {
            $requestedJobs
        } else {
            $LocalSlots + $remote
        }

        $distccState = Join-Path $env:LOCALAPPDATA (
            'wc3-dist\distcc-state-online-{0}-{1}' -f $PID,
            (Get-Date).ToString('HHmmss'))
        New-Item -ItemType Directory -Force -Path $distccState | Out-Null

        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST' -Value '1'
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_ACTIVE' -Value '1'
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_TRANSPORT' -Value 'distcc'
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_ALLOW_PGO' -Value $(if ($allowPgo) { '1' } else { '0' })
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_ALLOW_LTO' -Value $(if ($allowLto) { '1' } else { '0' })
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_PGO_PROFILE_READY' -Value $(if ($pgoReady) { '1' } else { '0' })
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DIST_LOCAL_SLOTS' -Value "$LocalSlots"
        Set-Wc3DistEnv -Previous $previous -Name 'WC3_DISTCC_EXE' -Value 'C:/msys64/usr/bin/distcc.exe'
        Set-Wc3DistEnv -Previous $previous -Name 'CCACHE_PREFIX' -Value (
            '{0} {1}' -f ($python -replace '\\', '/'), ($wrapper -replace '\\', '/'))
        Set-Wc3DistEnv -Previous $previous -Name 'DISTCC_HOSTS' -Value (
            '127.0.0.1:{0}/{1} localhost/{2} --localslots={2} --localslots_cpp={2}' -f
            $tunnel.Port, $remote, $LocalSlots)
        Set-Wc3DistEnv -Previous $previous -Name 'DISTCC_DIR' -Value $distccState
        Set-Wc3DistEnv -Previous $previous -Name 'TMPDIR' -Value $env:TEMP
        Set-Wc3DistEnv -Previous $previous -Name 'CMAKE_BUILD_PARALLEL_LEVEL' -Value "$jobs"

        Write-Host ("[dist] auto enabled ({0}): target={1} local={2} remote={3} jobs={4} pgo={5} lto={6}" -f
            $Variant, $tunnel.Target, $LocalSlots, $remote, $jobs,
            $(if ($allowPgo) { 'on' } else { 'off' }),
            $(if ($allowLto) { 'on' } else { 'off' }))
        return [pscustomobject]@{
            Tunnel = $tunnel
            TunnelScript = $tunnelScript
            DistccState = $distccState
            LocalSlots = $LocalSlots
            RemoteSlots = $remote
            Jobs = $jobs
            PgoProfileReady = $pgoReady
            PreviousEnv = $previous
        }
    } catch {
        $message = $_.Exception.Message -replace '\s+', ' '
        if ($tunnel) {
            try {
                & $tunnelScript -Action stop -Quiet | Out-Null
            } catch { }
        }
        if ($distccState -and (Test-Path -LiteralPath $distccState)) {
            Remove-Wc3DistccState -Path $distccState
        }
        foreach ($name in @('WC3_DIST', 'WC3_DIST_ACTIVE', 'WC3_DIST_TRANSPORT',
                'WC3_DIST_ALLOW_PGO', 'WC3_DIST_ALLOW_LTO',
                'WC3_DIST_PGO_PROFILE_READY', 'WC3_DIST_LOCAL_SLOTS',
                'WC3_DISTCC_EXE', 'CCACHE_PREFIX', 'DISTCC_HOSTS',
                'DISTCC_DIR', 'TMPDIR', 'CMAKE_BUILD_PARALLEL_LEVEL')) {
            Restore-Wc3DistEnv -Previous $previous -Name $name
        }
        if ($env:WC3_DIST_DEBUG -eq '1') {
            Write-Warning "[dist] distributed build unavailable: $message"
        } else {
            Write-Warning '[dist] IDEAPAD unavailable; building locally (set WC3_DIST_DEBUG=1 for details)'
        }
        return $null
    }
}

function Disable-Wc3DistributedBuild {
    [CmdletBinding()]
    param([Parameter(Mandatory = $false)]$State)

    if (-not $State) { return }
    try {
        & $State.TunnelScript -Action stop -Quiet | Out-Null
    } catch {
        Write-Warning "[dist] tunnel stop failed: $($_.Exception.Message)"
    }
    if ($State.DistccState -and (Test-Path -LiteralPath $State.DistccState)) {
        Remove-Wc3DistccState -Path $State.DistccState
    }
    foreach ($name in @('WC3_DIST', 'WC3_DIST_ACTIVE', 'WC3_DIST_TRANSPORT',
            'WC3_DIST_ALLOW_PGO', 'WC3_DIST_ALLOW_LTO',
            'WC3_DIST_PGO_PROFILE_READY', 'WC3_DIST_LOCAL_SLOTS',
            'WC3_DISTCC_EXE', 'CCACHE_PREFIX', 'DISTCC_HOSTS',
            'DISTCC_DIR', 'TMPDIR', 'CMAKE_BUILD_PARALLEL_LEVEL')) {
        Restore-Wc3DistEnv -Previous $State.PreviousEnv -Name $name
    }
}

function Invoke-Wc3AutoDistributedBuild {
    # Re-run the calling build script once with the distcc environment already
    # installed.  The child sees WC3_DIST_ACTIVE=1, skips this function, and
    # executes the original script body unchanged.  Keeping the trampoline here
    # means build_app.ps1 needs one call instead of a try/finally around its
    # whole body, and every existing exit path still reaches cleanup.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        $Parameters,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Variant = 'release',
        [string]$PgoProfile = ''
    )

    if ($env:WC3_DIST_ACTIVE -eq '1' -or $env:WC3_DIST -eq '1') {
        return
    }
    $state = Enable-Wc3DistributedBuild -Root $Root -Variant $Variant `
        -PgoProfile $PgoProfile
    if (-not $state) {
        return
    }

    $code = 0
    try {
        if ($null -ne $Parameters) {
            & $ScriptPath @Parameters
        } else {
            & $ScriptPath
        }
        if ($null -ne $LASTEXITCODE) {
            $code = [int]$LASTEXITCODE
        }
    } finally {
        Disable-Wc3DistributedBuild -State $state
    }
    exit $code
}
