# Build the release_optimized Android product.
#
# This target is release plus ThinLTO, PGO and corpus Polly.  PGO is a real
# two-stage flow:
#   1. build release_optimized_pgo with -fprofile-generate (IRPGO by default;
#      -PgoKind instr keeps the historical front-end profile);
#   2. install and launch it on the test device, then pull the .profraw files;
#   3. keep one complete cumulative snapshot per PID, merge them with
#      llvm-profdata, verify the instrumentation kind, and build
#      release_optimized with -fprofile-use and -flto=thin.
#
# The final APK has applicationId wc3.recomp and uses the release Gradle build
# type.  Only the native build directory and compiler profile flags differ from
# wc3-release.apk.  The training APK is built with the same release inputs, but
# is deliberately not LTO and is replaced/restored after collection.
#
# Usage:
#   powershell -File tools/android/build_release_optimized.ps1
#   powershell -File tools/android/build_release_optimized.ps1 -Install
#   powershell -File tools/android/build_release_optimized.ps1 -RefreshProfile
#   powershell -File tools/android/build_release_optimized.ps1 -NoPolly
#
# The test-device address is never stored here.  It is resolved from the active
# IPv4 DNS server and connected as <dns-ipv4>:5555 on every fresh profile run.

[CmdletBinding()]
param(
    [string]$PgoProfile = 'build/pgo/wc3.profdata',
    [string]$PgoRawDir = '/data/data/wc3.recomp/files/pgo',
    [ValidateSet('instr', 'ir')]
    [string]$PgoKind = 'ir',
    [ValidateRange(30, 3600)]
    [int]$ProfileSeconds = 120,
    # Polly is part of the release_optimized default.  This switch keeps the
    # A/B path available without falling back to build_app/build_apk directly.
    [switch]$NoPolly,
    [switch]$RefreshProfile,
    [switch]$Install,
    [string]$Serial = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$apkScript = Join-Path $PSScriptRoot 'build_apk.ps1'
$outDir = Join-Path $root 'build\android_app'
$releaseApk = Join-Path $outDir 'wc3-release.apk'
$optimizedApk = Join-Path $outDir 'wc3-release_optimized.apk'
$profilePath = if ([IO.Path]::IsPathRooted($PgoProfile)) {
    $PgoProfile
} else {
    Join-Path $root $PgoProfile
}
$profileDir = Split-Path -Parent $profilePath
$rawOutDir = Join-Path $profileDir 'raw'
New-Item -ItemType Directory -Force $profileDir | Out-Null
New-Item -ItemType Directory -Force $rawOutDir | Out-Null

function Get-AndroidSdk {
    $candidates = @()
    if ($env:ANDROID_SDK_ROOT) { $candidates += $env:ANDROID_SDK_ROOT }
    if ($env:ANDROID_HOME)     { $candidates += $env:ANDROID_HOME }
    if ($env:LOCALAPPDATA)     { $candidates += (Join-Path $env:LOCALAPPDATA 'Android\Sdk') }
    $candidates += 'E:\Android\Sdk'
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'platform-tools\adb.exe'))) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

function Get-NdkRoot {
    param([string]$Sdk)
    $roots = @()
    if ($env:ANDROID_NDK_HOME) { $roots += $env:ANDROID_NDK_HOME }
    if ($env:ANDROID_NDK_ROOT) { $roots += $env:ANDROID_NDK_ROOT }
    $parent = Join-Path $Sdk 'ndk'
    if (Test-Path $parent) {
        $roots += @(Get-ChildItem $parent -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { $_.FullName })
    }
    foreach ($r in ($roots | Select-Object -Unique)) {
        $profdata = Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-profdata.exe'
        if (Test-Path $profdata) {
            return [pscustomobject]@{
                Root = (Resolve-Path $r).Path
                Profdata = (Resolve-Path $profdata).Path
            }
        }
    }
    return $null
}

$sdk = Get-AndroidSdk
if (-not $sdk) { throw 'Android SDK not found' }
$ndk = Get-NdkRoot -Sdk $sdk
if (-not $ndk) { throw 'NDK llvm-profdata.exe not found' }
$adb = Join-Path $sdk 'platform-tools\adb.exe'

function Get-ProfileKind {
    param(
        [string]$Path,
        [string]$Profdata
    )
    $lines = @(& $Profdata show $Path 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "llvm-profdata show failed for $Path"
    }
    $text = $lines -join "`n"
    if ($text -match 'Instrumentation level:\s*IR\b') {
        return 'ir'
    }
    if ($text -match 'Instrumentation level:\s*Front-end\b') {
        return 'instr'
    }
    throw "cannot determine instrumentation kind for $Path"
}

function Resolve-TestSerial {
    param([string]$Requested)
    if ($Requested) {
        & $adb connect $Requested 2>&1 | ForEach-Object { Write-Host $_ }
        return $Requested
    }
    $dns = @(Get-DnsClientServerAddress -AddressFamily IPv4 |
        Select-Object -ExpandProperty ServerAddresses |
        Where-Object { $_ -and $_ -notmatch '^127\.' } |
        Select-Object -First 1)
    if (-not $dns -or -not $dns[0]) {
        throw 'No active IPv4 DNS server; pass -Serial <ip:port>'
    }
    $serial = "$($dns[0]):5555"
    Write-Host "[pgo] adb connect $serial"
    & $adb connect $serial 2>&1 | ForEach-Object { Write-Host $_ }
    return $serial
}

function Invoke-BuildApk {
    param(
        [string]$Variant,
        [string]$Pgo,
        [string]$Serial,
        [switch]$ForceInstall
    )
    $apkArgs = @{
        Variant = $Variant
        Pgo = $Pgo
        PgoKind = $PgoKind
        PgoProfile = $PgoProfile
        PgoRawDir = $PgoRawDir
    }
    if ($Serial) { $apkArgs['Serial'] = $Serial }
    if ($NoPolly) { $apkArgs['Polly'] = $false }
    if ($Install -or $ForceInstall) { $apkArgs['Install'] = $true }
    $logDir = Join-Path $root 'build\tmp'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $log = Join-Path $logDir ("pgo_{0}.log" -f $Variant)
    Write-Host "[pgo] build $Variant (PGO $Pgo), log $log"
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $apkScript @apkArgs 2>&1 |
        Tee-Object -FilePath $log |
        ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($rc -ne 0) { throw "APK build failed for $Variant (exit $rc)" }
}

function Invoke-AdbShell {
    param(
        [string]$Serial,
        [string]$Command
    )
    & $adb -s $Serial shell $Command 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "adb shell failed: $Command"
    }
}

$profileBuildInstalled = $false
$profileExists = Test-Path -LiteralPath $profilePath
$existingKind = $null
if ($profileExists) {
    try {
        $existingKind = Get-ProfileKind -Path $profilePath `
            -Profdata $ndk.Profdata
        Write-Host "[pgo] existing profile kind $existingKind"
    } catch {
        Write-Host "[pgo] existing profile is not readable: $($_.Exception.Message)"
        $existingKind = $null
    }
}
$needProfile = $RefreshProfile -or -not $profileExists -or
    ($existingKind -ne $PgoKind)
if ($needProfile) {
    if ($existingKind -and $existingKind -ne $PgoKind) {
        Write-Host "[pgo] profile kind $existingKind != requested $PgoKind; retraining"
    } elseif ($RefreshProfile) {
        Write-Host '[pgo] refresh requested'
    } else {
        Write-Host '[pgo] profile missing or unreadable; training'
    }
    $serial = Resolve-TestSerial -Requested $Serial
    Invoke-BuildApk -Variant 'release_optimized_pgo' -Pgo 'generate' `
        -Serial $serial -ForceInstall
    $profileBuildInstalled = $true

    # The release package is not debuggable, but the test device is rooted.
    # Do not pre-create the directory as root: a root-created directory carries
    # app_data_file:s0 without the package's MLS categories, and the app write
    # is then denied even though UNIX ownership is correct.  The writer in
    # pgo_profile.c creates the directory as the app UID, which inherits the
    # package categories.
    Invoke-AdbShell -Serial $serial -Command "am force-stop wc3.recomp" | Out-Null
    Invoke-AdbShell -Serial $serial -Command "su -c 'rm -rf $PgoRawDir'" | Out-Null
    Invoke-AdbShell -Serial $serial -Command "logcat -c" | Out-Null
    Invoke-AdbShell -Serial $serial -Command "am start -W -n wc3.recomp/wc3.recomp.Wc3GameActivity" | ForEach-Object { Write-Host $_ }
    Write-Host "[pgo] training for $ProfileSeconds s (writer snapshots every 30 s; keep the game running)"
    Start-Sleep -Seconds $ProfileSeconds
    Invoke-AdbShell -Serial $serial -Command "am force-stop wc3.recomp" | Out-Null
    # Do not use -t here: it selects the last N lines of the whole buffer
    # before the tag filter, so normal system chatter can hide the writer
    # lines.  The buffer was cleared before the launch, so an unlimited dump
    # of this tag is small.
    $writerLog = Invoke-AdbShell -Serial $serial -Command "logcat -d -s wc3-pgo"
    $writerLog | ForEach-Object { Write-Host $_ }
    if (-not (($writerLog | Out-String) -match 'rc=0')) {
        throw 'PGO writer did not report rc=0; inspect the wc3-pgo logcat line'
    }

    $listing = & $adb -s $serial shell "su -c 'ls -1 $PgoRawDir/*.profraw 2>/dev/null'" 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($listing | Out-String).Trim()) {
        throw "no .profraw files produced in $PgoRawDir; inspect build/pgo and logcat"
    }
    $exportDir = '/sdcard/Download/wc3-pgo'
    Invoke-AdbShell -Serial $serial -Command "su -c 'rm -rf $exportDir && mkdir -p $exportDir && cp $PgoRawDir/*.profraw $exportDir/ && chmod 0666 $exportDir/*.profraw'" | Out-Null
    Remove-Item $rawOutDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $rawOutDir | Out-Null
    # adb writes its progress line to stderr.  With ErrorActionPreference=Stop,
    # Windows PowerShell 5 turns that line into a terminating NativeCommandError
    # before the exit-code check, so scope Continue around the pull only.
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $adb -s $serial pull $exportDir $rawOutDir 2>&1 |
        ForEach-Object { Write-Host $_ }
    $pullRc = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($pullRc -ne 0) { throw 'adb pull of .profraw files failed' }
    $allRaws = @(Get-ChildItem $rawOutDir -Recurse -Filter '*.profraw' -File)
    if ($allRaws.Count -eq 0) { throw "no .profraw files pulled into $rawOutDir" }
    $allRaws | ForEach-Object {
        Write-Host "[pgo] raw $($_.Name) ($($_.Length) bytes)"
    }
    # A process that exits before its first 30 s snapshot leaves a zero-byte
    # file.  It has no counters and llvm-profdata rejects it; never merge it.
    $raws = @($allRaws | Where-Object { $_.Length -gt 0 })
    $emptyRaws = @($allRaws | Where-Object { $_.Length -eq 0 })
    if ($emptyRaws.Count -gt 0) {
        Write-Host "[pgo] skipping $($emptyRaws.Count) empty snapshot(s)"
    }
    if ($raws.Count -eq 0) {
        throw "all pulled .profraw files are empty in $rawOutDir"
    }

    # The writer alternates cumulative a/b snapshots.  Merging both slots
    # double-counts every counter that exists in both files.  Keep one
    # complete snapshot per process id; separate training launches keep
    # separate PIDs and are all merged.
    $groups = @($raws | Group-Object {
        if ($_.Name -match '^wc3-(\d+)(?:-[ab])?\.profraw$') {
            $matches[1]
        } else {
            $_.BaseName
        }
    })
    $selected = @()
    foreach ($group in $groups) {
        $latest = @($group.Group |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1)[0]
        if (-not $latest) { continue }
        if ($group.Count -gt 1) {
            Write-Host "[pgo] pid/group $($group.Name): keeping $($latest.Name), dropping $($group.Count - 1) snapshot(s)"
        }
        $selected += $latest
    }
    if ($selected.Count -eq 0) {
        throw "no usable .profraw snapshot selected from $rawOutDir"
    }
    $selected | ForEach-Object {
        Write-Host "[pgo] merge $($_.Name) ($($_.Length) bytes)"
    }
    & $ndk.Profdata merge "-o=$profilePath" @($selected.FullName) 2>&1 |
        ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $profilePath)) {
        throw "llvm-profdata merge failed for $profilePath"
    }
    & $ndk.Profdata show $profilePath 2>&1 |
        Select-Object -First 25 | ForEach-Object { Write-Host $_ }
    $mergedKind = Get-ProfileKind -Path $profilePath `
        -Profdata $ndk.Profdata
    if ($mergedKind -ne $PgoKind) {
        throw "merged profile kind '$mergedKind' != requested '$PgoKind'"
    }
    Set-Content -LiteralPath "$profilePath.kind" -Value $PgoKind
    Write-Host "[pgo] profile $profilePath ($((Get-Item $profilePath).Length) bytes, kind $mergedKind)"
} else {
    Write-Host "[pgo] reusing $PgoKind profile $profilePath"
    Set-Content -LiteralPath "$profilePath.kind" -Value $PgoKind
}

$finalSerial = if ($Install) { Resolve-TestSerial -Requested $Serial } else { '' }
Invoke-BuildApk -Variant 'release_optimized' -Pgo 'use' -Serial $finalSerial

# Do not leave the instrumented training build installed unless the caller
# explicitly asked for the optimized APK.  Restore the already-built release
# artifact, which is the package state the user had before this run.
if (-not $Install -and $profileBuildInstalled -and (Test-Path -LiteralPath $releaseApk)) {
    $serial = Resolve-TestSerial -Requested $Serial
    Write-Host '[pgo] restoring installed release APK'
    & $adb -s $serial install -r $releaseApk 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw 'restoring release APK failed' }
}

if (-not (Test-Path -LiteralPath $optimizedApk)) {
    throw "optimized APK missing: $optimizedApk"
}
Write-Host "[pgo] built $optimizedApk ($((Get-Item $optimizedApk).Length) bytes)"
