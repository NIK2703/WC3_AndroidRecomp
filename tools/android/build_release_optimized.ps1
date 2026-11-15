# Build the release_optimized Android product.
#
# This target is release plus ThinLTO and PGO.  PGO is a real two-stage flow:
#   1. build release_optimized_pgo with -fprofile-instr-generate;
#   2. install and launch it on the test device, then pull the .profraw files;
#   3. merge them with llvm-profdata and build release_optimized with
#      -fprofile-instr-use and -flto=thin.
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
#
# The test-device address is never stored here.  It is resolved from the active
# IPv4 DNS server and connected as <dns-ipv4>:5555 on every fresh profile run.

[CmdletBinding()]
param(
    [string]$PgoProfile = 'build/pgo/wc3.profdata',
    [string]$PgoRawDir = '/data/data/wc3.recomp/files/pgo',
    [int]$ProfileSeconds = 45,
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
        PgoProfile = $PgoProfile
        PgoRawDir = $PgoRawDir
    }
    if ($Serial) { $apkArgs['Serial'] = $Serial }
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
if ($RefreshProfile -or -not (Test-Path -LiteralPath $profilePath)) {
    $serial = Resolve-TestSerial -Requested $Serial
    Write-Host "[pgo] profile missing or refresh requested: $profilePath"
    Invoke-BuildApk -Variant 'release_optimized_pgo' -Pgo 'generate' -Serial $serial -ForceInstall
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
    Write-Host "[pgo] training for $ProfileSeconds s (writer snapshots after 20 s)"
    Start-Sleep -Seconds $ProfileSeconds
    Invoke-AdbShell -Serial $serial -Command "am force-stop wc3.recomp" | Out-Null
    $writerLog = Invoke-AdbShell -Serial $serial -Command "logcat -d -t 300 -s wc3-pgo"
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
    & $adb -s $serial pull $exportDir $rawOutDir 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw 'adb pull of .profraw files failed' }
    $raws = @(Get-ChildItem $rawOutDir -Recurse -Filter '*.profraw' -File)
    if ($raws.Count -eq 0) { throw "no .profraw files pulled into $rawOutDir" }
    $raws | ForEach-Object { Write-Host "[pgo] raw $($_.Name) ($($_.Length) bytes)" }
    & $ndk.Profdata merge "-o=$profilePath" @($raws.FullName) 2>&1 |
        ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $profilePath)) {
        throw "llvm-profdata merge failed for $profilePath"
    }
    & $ndk.Profdata show $profilePath 2>&1 |
        Select-Object -First 25 | ForEach-Object { Write-Host $_ }
    Write-Host "[pgo] profile $profilePath ($((Get-Item $profilePath).Length) bytes)"
} else {
    Write-Host "[pgo] reusing profile $profilePath"
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
