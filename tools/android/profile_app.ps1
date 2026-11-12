# Capture CPU, scheduling and native-heap profiles for the debug APK.
#
# simpleperf is an NDK host tool, not an APK component: it attaches to the
# profileable/debuggable package from adb.  Perfetto is the system service on
# the device; the host script pushes a small textproto config and pulls the
# trace.  No profiler binary is bundled into libwc3gameactivity.so.
#
# Usage:
#   powershell -File tools/android/profile_app.ps1 -Tool simpleperf
#   powershell -File tools/android/profile_app.ps1 -Tool perfetto -Seconds 30
#   powershell -File tools/android/profile_app.ps1 -Tool heap -Launch
#   powershell -File tools/android/profile_app.ps1 -Tool all
#   powershell -File tools/android/profile_app.ps1 -LibDir C:\ab\off
#
# The app must be running in the main menu before a capture unless -Launch is
# passed.  -Launch brings the Activity to the foreground even when the Java
# process survived a previous window close.  For heapprofd, launch the app
# after starting the trace if startup allocations matter; this script starts
# the app first for a steady-state view.  simpleperf also writes
# simpleperf-flat.txt and simpleperf-threads.txt next to the callgraph, plus
# simpleperf-summary.txt and one simpleperf-tid-<tid>-flat.txt for the hottest
# threads.

[CmdletBinding()]
param(
    [ValidateSet('simpleperf', 'perfetto', 'heap', 'all')]
    [string]$Tool = 'simpleperf',

    [string]$Package = 'wc3.recomp.debug',

    [string]$Activity = '.Wc3GameActivity',

    [string]$ProcessName = '',

    [ValidateRange(5, 300)]
    [int]$Seconds = 20,

    [string]$Serial = '',

    [switch]$Launch,

    [switch]$OffCpu,

    [switch]$NoReport,

    [string]$OutputDir = '',

    # Optional directory holding the unstripped libwc3gameactivity.so that
    # matches the installed APK.  The A/B run keeps OFF and ON artifacts in
    # separate directories; without this switch the current build tree is
    # used and an OFF capture would be symbolized with the ON library.
    [string]$LibDir = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:adb = $null
$script:adbPrefix = @()
$script:ndk = $null
$script:python = $null
$script:libDir = $LibDir

function Get-AndroidSdk {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME) { $candidates += $env:ANDROID_HOME }
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA 'Android\Sdk')
    }
    $candidates += 'E:\Android\Sdk'
    foreach ($candidate in $candidates) {
        if ($candidate -and
            (Test-Path (Join-Path $candidate 'platform-tools\adb.exe'))) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function Get-AndroidNdk {
    $roots = @()
    if ($env:ANDROID_NDK_HOME) { $roots += $env:ANDROID_NDK_HOME }
    if ($env:ANDROID_NDK_ROOT) { $roots += $env:ANDROID_NDK_ROOT }
    $sdk = Get-AndroidSdk
    if ($sdk) {
        $parent = Join-Path $sdk 'ndk'
        if (Test-Path $parent) {
            $roots += @(Get-ChildItem $parent -Directory |
                Sort-Object Name -Descending |
                ForEach-Object { $_.FullName })
        }
    }
    foreach ($candidate in ($roots | Select-Object -Unique)) {
        $report = Join-Path $candidate 'simpleperf\report.py'
        if (Test-Path $report) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function Get-Python {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw 'python.exe not found; simpleperf report.py needs Python 3'
    }
    & $cmd.Source --version *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "python.exe is not usable: $($cmd.Source)"
    }
    return $cmd.Source
}

function Get-AdbPrefix {
    $prefix = @()
    if ($Serial) {
        $prefix = @('-s', $Serial)
    }
    return $prefix
}

function Quote-NativeArg {
    param([string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdbArgs,
        [switch]$AllowFailure
    )
    $prefix = $script:adbPrefix
    & $script:adb @prefix @AdbArgs
    $rc = $LASTEXITCODE
    if ($rc -ne 0 -and -not $AllowFailure) {
        throw "adb failed ($rc): $($AdbArgs -join ' ')"
    }
    return $rc
}

function Assert-Device {
    $devices = @(& $script:adb devices)
    if ($LASTEXITCODE -ne 0) {
        throw 'adb devices failed'
    }
    $wanted = $Serial
    if (-not $wanted) {
        $online = @($devices | Select-Object -Skip 1 |
            Where-Object { $_ -match '\sdevice$' })
        if ($online.Count -eq 0) {
            throw 'no online adb device; connect it or pass -Serial'
        }
        if ($online.Count -gt 1) {
            throw 'more than one adb device; pass -Serial'
        }
        return
    }
    $line = $devices | Where-Object { $_ -match "^$([regex]::Escape($wanted))\s" }
    if (-not $line -or $line -notmatch '\sdevice$') {
        throw "adb device not online: $wanted"
    }
}

function Assert-Package {
    $prefix = $script:adbPrefix
    $path = (& $script:adb @prefix shell pm path $Package 2>&1) |
        Out-String
    if ($LASTEXITCODE -ne 0 -or $path -notmatch 'package:') {
        throw "package is not installed: $Package"
    }
}

function Get-AppProcessName {
    if ($ProcessName) {
        return $ProcessName
    }
    # Wc3RocActivity runs in :roc, Wc3GameActivity in the package process.
    if ($Activity -match 'Roc') {
        return ($Package + ':roc')
    }
    return $Package
}

function Get-AppComponent {
    $name = $Activity
    if ($name.StartsWith('.')) {
        $name = $Package + $name
    }
    return ($Package + '/' + $name)
}

function Get-AppPid {
    $prefix = $script:adbPrefix
    $process = Get-AppProcessName
    $appPid = (& $script:adb @prefix shell pidof $process 2>$null) |
        Out-String
    $appPid = $appPid.Trim()
    if ($LASTEXITCODE -ne 0 -or -not $appPid) {
        return $null
    }
    return ($appPid -split '\s+')[0]
}

function Start-App {
    $component = Get-AppComponent
    Invoke-Adb -AdbArgs @('shell', 'am', 'start', '-n', $component) |
        Out-Null
    for ($i = 0; $i -lt 30; $i++) {
        if (Get-AppPid) {
            Start-Sleep -Milliseconds 500
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "app did not start: $component"
}

function Ensure-AppRunning {
    # A foreground Activity and a live guest are different states: the Java
    # process survives after the window is destroyed.  With -Launch the intent
    # is authoritative, so bring the Activity up even when the package still
    # has a PID from a previous run.
    if ($Launch) {
        Start-App
        $appPid = Get-AppPid
        if (-not $appPid) {
            throw "app did not start: $(Get-AppComponent)"
        }
        return $appPid
    }
    $appPid = Get-AppPid
    if ($appPid) {
        return $appPid
    }
    throw "package $Package is not running; start it in the main menu or pass -Launch"
}

function Get-OutputDirectory {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    if ($OutputDir) {
        if ([IO.Path]::IsPathRooted($OutputDir)) {
            $dir = $OutputDir
        } else {
            $dir = Join-Path $root $OutputDir
        }
    } else {
        $dir = Join-Path $root "build\profiles\$stamp"
    }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return (Resolve-Path $dir).Path
}

function Get-UnstrippedLibDirectory {
    $candidates = @()
    if ($script:libDir) {
        $candidates += $script:libDir
    } else {
        $candidates += @(
            (Join-Path $root 'build\android_app\debug\cmake'),
            (Join-Path $root 'build\android_app\debug\jniLibs\arm64-v8a')
        )
    }
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        if (Test-Path (Join-Path $candidate 'libwc3gameactivity.so')) {
            return (Resolve-Path $candidate).Path
        }
    }
    throw ('unstripped debug libwc3gameactivity.so not found; pass -LibDir ' +
           'with the directory that matches the installed APK, or build the ' +
           'debug variant first (build_apk.ps1 -NativeDebug)')
}

function Save-AdbExecOut {
    param(
        [string]$DevicePath,
        [string]$Destination
    )
    $prefix = $script:adbPrefix
    $adbArgs = @($prefix) + @('exec-out', 'run-as', $Package, 'cat', $DevicePath)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:adb
    $psi.Arguments = (($adbArgs | ForEach-Object { Quote-NativeArg $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            throw 'cannot start adb exec-out'
        }
        $stream = [IO.File]::Open(
            $Destination,
            [IO.FileMode]::Create,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $process.StandardOutput.BaseStream.CopyTo($stream)
        } finally {
            $stream.Dispose()
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "adb exec-out failed ($($process.ExitCode)): $DevicePath"
        }
    } finally {
        $process.Dispose()
    }
}

function Write-SimpleperfSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputDirectory,
        [Parameter(Mandatory = $true)]
        [string]$PerfData,
        [Parameter(Mandatory = $true)]
        [int]$CaptureSeconds,
        [int]$TopThreads = 5
    )
    $flatPath = Join-Path $OutputDirectory 'simpleperf-flat.txt'
    $threadPath = Join-Path $OutputDirectory 'simpleperf-threads.txt'
    if (-not (Test-Path $flatPath)) {
        Write-Warning "simpleperf flat report missing: $flatPath"
        return
    }

    $flatLines = @(Get-Content -LiteralPath $flatPath)
    $sampleCount = [int64]0
    $eventCount = [int64]0
    foreach ($line in $flatLines) {
        $match = [regex]::Match($line, '^Samples:\s+(\d+)')
        if ($match.Success) {
            $sampleCount = [int64]$match.Groups[1].Value
            continue
        }
        $match = [regex]::Match($line, '^Event count:\s+(\d+)')
        if ($match.Success) {
            $eventCount = [int64]$match.Groups[1].Value
        }
    }

    # report.py aligns columns with padding, so use two or more spaces as the
    # separator.  A symbol can contain spaces; taking the last group keeps it
    # intact instead of splitting on every blank.
    $symbols = New-Object System.Collections.Generic.List[object]
    foreach ($line in $flatLines) {
        $match = [regex]::Match(
            $line,
            '^\s*([0-9]+(?:\.[0-9]+)?)%\s{2,}(.+?)\s{2,}(.+?)\s*$')
        if (-not $match.Success) {
            continue
        }
        $symbols.Add([pscustomobject]@{
            Percent = [double]::Parse($match.Groups[1].Value,
                [Globalization.CultureInfo]::InvariantCulture)
            Dso = $match.Groups[2].Value
            Symbol = $match.Groups[3].Value
        })
    }

    $threads = New-Object System.Collections.Generic.List[object]
    if (Test-Path $threadPath) {
        foreach ($line in @(Get-Content -LiteralPath $threadPath)) {
            $match = [regex]::Match(
                $line,
                '^\s*([0-9]+(?:\.[0-9]+)?)%\s+(.+?)\s+(\d+)\s*$')
            if (-not $match.Success) {
                continue
            }
            $threads.Add([pscustomobject]@{
                Percent = [double]::Parse($match.Groups[1].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
                Command = $match.Groups[2].Value.Trim()
                Tid = [int]$match.Groups[3].Value
            })
        }
    } else {
        Write-Warning "simpleperf thread report missing: $threadPath"
    }

    if ($eventCount -gt 0) {
        $cpuSeconds = $eventCount / 1000000000.0
    } else {
        # The capture uses task-clock at 1000 Hz, so this is a fallback only.
        $cpuSeconds = $sampleCount / 1000.0
    }
    $cores = if ($CaptureSeconds -gt 0) {
        $cpuSeconds / $CaptureSeconds
    } else {
        0.0
    }

    $topSymbols = @($symbols | Sort-Object Percent -Descending |
        Select-Object -First 25)
    $topThreadList = @($threads | Sort-Object Percent -Descending |
        Select-Object -First $TopThreads)

    # The native-TLS A/B replaces __emutls_get_address with __tls_get_addr
    # (global-dynamic) or removes it entirely (initial-exec).  Keep both in the
    # same group so the baseline and the A/B remain comparable.
    $tlsPercent = ($symbols | Where-Object {
        $_.Symbol -eq '__emutls_get_address' -or
        $_.Symbol -eq '__tls_get_addr' -or
        $_.Symbol -eq '__tls_guard' -or
        $_.Symbol -eq '[linker]tlsdesc_resolver_dynamic' -or
        $_.Symbol -eq '[linker]tlsdesc_resolver_static' -or
        $_.Symbol -eq 'pthread_getspecific'
    } | Measure-Object -Property Percent -Sum).Sum
    if ($null -eq $tlsPercent) { $tlsPercent = 0.0 }
    $nativeX87Percent = ($symbols | Where-Object {
        $_.Symbol -match '^x87_'
    } | Measure-Object -Property Percent -Sum).Sum
    if ($null -eq $nativeX87Percent) { $nativeX87Percent = 0.0 }
    $dxvkPercent = ($symbols | Where-Object {
        $_.Symbol -match '^wc3_dxvk'
    } | Measure-Object -Property Percent -Sum).Sum
    if ($null -eq $dxvkPercent) { $dxvkPercent = 0.0 }
    $copyPercent = ($symbols | Where-Object {
        $_.Symbol -match '^(thin_lock_buffer|thin_unlock_buffer|__memmove)'
    } | Measure-Object -Property Percent -Sum).Sum
    if ($null -eq $copyPercent) { $copyPercent = 0.0 }

    $culture = [Globalization.CultureInfo]::InvariantCulture
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('WC3 simpleperf summary')
    $lines.Add([string]::Format($culture,
        'capture: {0}s, {1} samples, {2:F3} CPU-s, {3:F3} cores',
        $CaptureSeconds, $sampleCount, $cpuSeconds, $cores))
    $lines.Add('')
    $lines.Add('Top symbols by self time:')
    foreach ($symbol in $topSymbols) {
        $lines.Add([string]::Format($culture, '  {0,6:F2}%  {1}',
            $symbol.Percent, $symbol.Symbol))
    }
    $lines.Add('')
    $lines.Add('Grouped self time (may overlap only within a group):')
    $lines.Add([string]::Format($culture,
        '  TLS runtime (emutls/tlsdesc): {0:F2}%', $tlsPercent))
    $lines.Add([string]::Format($culture,
        '  native x87 helpers:           {0:F2}%', $nativeX87Percent))
    $lines.Add([string]::Format($culture,
        '  DXVK bridge:                 {0:F2}%', $dxvkPercent))
    $lines.Add([string]::Format($culture,
        '  thin bridge copies:          {0:F2}%', $copyPercent))

    $perThreadReports = New-Object System.Collections.Generic.List[string]
    if ($TopThreads -gt 0 -and $topThreadList.Count -gt 0) {
        $report = Join-Path $script:ndk 'simpleperf\report.py'
        $lines.Add('')
        $lines.Add('Top threads by self time:')
        foreach ($thread in $topThreadList) {
            $lines.Add([string]::Format($culture, '  {0,6:F2}%  {1} (tid {2})',
                $thread.Percent, $thread.Command, $thread.Tid))
            $tidReport = Join-Path $OutputDirectory (
                'simpleperf-tid-{0}-flat.txt' -f $thread.Tid)
            & $script:python $report `
                -i $PerfData `
                --tids ([string]$thread.Tid) `
                --sort dso,symbol `
                --percent-limit 0.05 `
                -o $tidReport
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "simpleperf tid $($thread.Tid) report failed"
            } else {
                $perThreadReports.Add([IO.Path]::GetFileName($tidReport))
            }
        }
    }
    if ($perThreadReports.Count -gt 0) {
        $lines.Add('')
        $lines.Add('Per-thread flat reports: ' + ($perThreadReports -join ', '))
    }

    $summaryPath = Join-Path $OutputDirectory 'simpleperf-summary.txt'
    [IO.File]::WriteAllLines($summaryPath, [string[]]$lines.ToArray())
    Write-Host "[profile] summary $summaryPath"
}

function Invoke-Simpleperf {
    param(
        [string]$OutputDirectory,
        [string]$AppPid
    )
    if (-not $AppPid) {
        throw 'simpleperf capture needs a running app pid'
    }
    $ndkTool = Join-Path $script:ndk `
        'simpleperf\bin\android\arm64\simpleperf'
    if (-not (Test-Path $ndkTool)) {
        throw "simpleperf binary missing: $ndkTool"
    }
    $report = Join-Path $script:ndk 'simpleperf\report.py'
    $builder = Join-Path $script:ndk 'simpleperf\binary_cache_builder.py'
    $libDir = Get-UnstrippedLibDirectory
    $perfData = Join-Path $OutputDirectory 'simpleperf.data'
    $deviceTool = '/data/local/tmp/wc3-simpleperf'
    $appTool = 'files/wc3-simpleperf'
    $deviceData = 'files/perf.data'

    Write-Host "[profile] simpleperf pid=$AppPid duration=${Seconds}s"
    try {
        # app_profiler.py writes /data/local/tmp/perf.data and then switches to
        # the app uid.  Current Android denies that write (771 shell_data_file
        # plus SELinux), so run the NDK simpleperf under run-as and keep both
        # the binary and perf.data in the app's own files directory.
        Invoke-Adb -AdbArgs @('push', $ndkTool, $deviceTool) | Out-Null
        Invoke-Adb -AdbArgs @('shell', 'chmod', '755', $deviceTool) | Out-Null
        Invoke-Adb -AdbArgs @(
            'shell', 'run-as', $Package, 'mkdir', '-p', 'files') | Out-Null
        Invoke-Adb -AdbArgs @(
            'shell', 'run-as', $Package, 'cp', $deviceTool, $appTool) |
            Out-Null
        Invoke-Adb -AdbArgs @(
            'shell', 'run-as', $Package, 'chmod', '700', $appTool) |
            Out-Null
        Invoke-Adb -AdbArgs @(
            'shell', 'run-as', $Package, 'rm', '-f', $deviceData) |
            Out-Null
        # --in-app suppresses simpleperf's fatal debug.perf_event_max_sample_rate
        # property write.  perf_harden=0 is still required for the perf event;
        # it is best-effort because this only works on a test/debug device.
        Invoke-Adb -AdbArgs @(
            'shell', 'setprop', 'security.perf_harden', '0') `
            -AllowFailure | Out-Null

        $recordArgs = @(
            'shell', 'run-as', $Package, $appTool, 'record',
            '--in-app',
            '-o', $deviceData,
            '-e', 'task-clock:u',
            '-f', '1000',
            '--duration', ([string]$Seconds),
            '-g',
            '-p', $AppPid)
        if ($OffCpu) {
            $recordArgs += '--trace-offcpu'
        }
        Invoke-Adb -AdbArgs $recordArgs | Out-Null
        Save-AdbExecOut -DevicePath $deviceData -Destination $perfData
    } finally {
        Invoke-Adb -AdbArgs @(
            'shell', 'run-as', $Package, 'rm', '-f', $deviceData, $appTool) `
            -AllowFailure | Out-Null
        Invoke-Adb -AdbArgs @('shell', 'rm', '-f', $deviceTool) `
            -AllowFailure | Out-Null
    }

    if (-not (Test-Path $perfData) -or
        (Get-Item $perfData).Length -le 0) {
        throw "simpleperf produced no data: $perfData"
    }

    if (-not $NoReport) {
        Push-Location $OutputDirectory
        try {
            & $script:python $builder `
                -i $perfData `
                -lib $libDir `
                --ndk_path $script:ndk `
                --disable_adb_root
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'simpleperf binary cache is incomplete'
            }
            $reportPath = Join-Path $OutputDirectory 'simpleperf-report.txt'
            & $script:python $report `
                -i $perfData `
                -g `
                --percent-limit 0.5 `
                -o $reportPath
            if ($LASTEXITCODE -ne 0) {
                throw 'simpleperf report.py failed'
            }
            Write-Host "[profile] report $reportPath"

            # The default callgraph is large.  Keep two small, stable views
            # next to it: self time by symbol (what burns CPU) and self time
            # by thread (where it runs).  Both use the same binary cache.
            $flatReport = Join-Path $OutputDirectory 'simpleperf-flat.txt'
            & $script:python $report `
                -i $perfData `
                --sort dso,symbol `
                --percent-limit 0.01 `
                -o $flatReport
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'simpleperf flat report failed'
            } else {
                Write-Host "[profile] flat report $flatReport"
            }

            $threadReport = Join-Path $OutputDirectory 'simpleperf-threads.txt'
            & $script:python $report `
                -i $perfData `
                --sort comm,tid `
                --percent-limit 0.01 `
                -o $threadReport
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'simpleperf thread report failed'
            } else {
                Write-Host "[profile] thread report $threadReport"
            }

            Write-SimpleperfSummary -OutputDirectory $OutputDirectory `
                -PerfData $perfData -CaptureSeconds $Seconds
        } finally {
            Pop-Location
        }
    }
    Write-Host "[profile] simpleperf $perfData"
}

function Expand-PerfettoConfig {
    param(
        [string]$Template,
        [string]$Destination,
        [string]$AppPid
    )
    if (-not (Test-Path $Template)) {
        throw "perfetto config template missing: $Template"
    }
    $text = [IO.File]::ReadAllText($Template)
    $text = $text.Replace('__WC3_PACKAGE__', $Package)
    $text = $text.Replace('__WC3_PID__', $AppPid)
    $text = $text.Replace('__WC3_DURATION_MS__',
                          ([string]($Seconds * 1000)))
    [IO.File]::WriteAllText($Destination, $text)
}

function Invoke-Perfetto {
    param(
        [string]$OutputDirectory,
        [ValidateSet('cpu', 'heap')]
        [string]$Kind,
        [string]$AppPid
    )
    $prefix = $script:adbPrefix
    $api = ((& $script:adb @prefix shell getprop ro.build.version.sdk) |
        Out-String).Trim()
    if ($api -notmatch '^\d+$') {
        throw 'cannot read ro.build.version.sdk from the device'
    }
    if ([int]$api -lt 29) {
        throw "Perfetto textproto capture requires Android 10 (API 29); device is API $api"
    }

    if ($Kind -eq 'heap' -and -not $AppPid) {
        throw 'heap capture needs a running app pid'
    }
    $templateName = if ($Kind -eq 'heap') { 'wc3_heap.cfg' } else { 'wc3_cpu.cfg' }
    $template = Join-Path $root "tools\android\perfetto\$templateName"
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $localCfg = Join-Path $OutputDirectory "perfetto-$Kind-$stamp.cfg"
    $localTrace = Join-Path $OutputDirectory "perfetto-$Kind.perfetto-trace"
    # The perfetto service cannot read /data/local/tmp on current Android, so
    # feed the textproto over stdin.  It writes to its own trace directory.
    $deviceTrace = "/data/misc/perfetto-traces/wc3-$Kind-$stamp.perfetto-trace"

    Expand-PerfettoConfig -Template $template -Destination $localCfg `
        -AppPid $AppPid
    Write-Host "[profile] perfetto $Kind duration=${Seconds}s"
    $prefix = $script:adbPrefix
    try {
        [IO.File]::ReadAllText($localCfg) |
            & $script:adb @prefix shell perfetto --txt -c - -o $deviceTrace
        if ($LASTEXITCODE -ne 0) {
            throw 'perfetto capture failed'
        }
        Invoke-Adb -AdbArgs @('pull', $deviceTrace, $localTrace) | Out-Null
        Write-Host "[profile] perfetto $localTrace"
    } finally {
        Invoke-Adb -AdbArgs @('shell', 'rm', '-f', $deviceTrace) `
            -AllowFailure | Out-Null
    }
}

try {
    $sdk = Get-AndroidSdk
    if (-not $sdk) {
        throw 'Android SDK not found (ANDROID_SDK_ROOT/ANDROID_HOME/E:\Android\Sdk)'
    }
    $script:adb = Join-Path $sdk 'platform-tools\adb.exe'
    if (-not (Test-Path $script:adb)) {
        throw "adb not found: $($script:adb)"
    }
    $script:ndk = Get-AndroidNdk
    if (-not $script:ndk) {
        throw 'Android NDK with simpleperf/report.py not found'
    }
    if ($Tool -ne 'perfetto' -and $Tool -ne 'heap') {
        $script:python = Get-Python
    }
    if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = $sdk }
    if (-not $env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT = $sdk }
    $env:PATH = "$(Join-Path $sdk 'platform-tools');$env:PATH"
    if ($Serial) { $env:ANDROID_SERIAL = $Serial }

    $script:adbPrefix = Get-AdbPrefix
    Assert-Device
    Assert-Package
    $out = Get-OutputDirectory
    $appPid = Ensure-AppRunning
    Write-Host "[profile] output $out"

    $tools = @()
    if ($Tool -eq 'all') {
        $tools = @('simpleperf', 'perfetto', 'heap')
    } else {
        $tools = @($Tool)
    }
    foreach ($current in $tools) {
        switch ($current) {
            'simpleperf' { Invoke-Simpleperf -OutputDirectory $out -AppPid $appPid }
            'perfetto' { Invoke-Perfetto -OutputDirectory $out -Kind cpu -AppPid $appPid }
            'heap' { Invoke-Perfetto -OutputDirectory $out -Kind heap -AppPid $appPid }
        }
    }
    Write-Host '[profile] done'
    exit 0
} catch {
    Write-Host "[profile] $($_.Exception.Message)"
    exit 1
}
