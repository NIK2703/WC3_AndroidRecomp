# Build the canonical Android arm64 native product library.
#
# This script only invokes the new tree's CMake target.  It does not run any
# script from the old working tree.  Exit codes: 0 built, 1 failure,
# 2 unavailable (SDK/NDK/CMake or the GameActivity AAR is missing).

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NativeDebug,
    [string]$GamesActivityAar = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

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
    foreach ($r in ($roots | Select-Object -Unique)) {
        $clang = Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
        if (Test-Path $clang) {
            return [pscustomobject]@{
                Root  = (Resolve-Path $r).Path
                Clang = (Resolve-Path $clang).Path
            }
        }
    }
    return $null
}

$sdk = Get-AndroidSdk
if (-not $sdk) {
    Write-Host '[app] unavailable: Android SDK not found'
    exit 2
}
$ndk = Get-AndroidNdk
if (-not $ndk) {
    Write-Host '[app] unavailable: Android NDK with clang.exe not found'
    exit 2
}
$cmake = Join-Path $sdk 'cmake\3.22.1\bin\cmake.exe'
$ninja = Join-Path $sdk 'cmake\3.22.1\bin\ninja.exe'
$toolchain = Join-Path $ndk.Root 'build\cmake\android.toolchain.cmake'
if (-not (Test-Path $cmake) -or -not (Test-Path $ninja) -or
    -not (Test-Path $toolchain)) {
    Write-Host '[app] unavailable: CMake 3.22.1 or NDK toolchain missing'
    exit 2
}

if (-not $GamesActivityAar) {
    $cache = Join-Path $env:USERPROFILE `
        '.gradle\caches\modules-2\files-2.1\androidx.games\games-activity\4.4.2'
    if (Test-Path $cache) {
        $GamesActivityAar = Get-ChildItem $cache -Recurse `
            -Filter 'games-activity-4.4.2.aar' |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $GamesActivityAar -or -not (Test-Path $GamesActivityAar)) {
    Write-Host '[app] unavailable: games-activity-4.4.2.aar not found'
    exit 2
}
$GamesActivityAar = (Resolve-Path $GamesActivityAar).Path

$out = Join-Path $root 'build\android_app'
$ga = Join-Path $out 'gameactivity-aar'
if ($Clean) {
    Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item $ga -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $ga | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($GamesActivityAar, $ga)

$debugValue = if ($NativeDebug) { 'ON' } else { 'OFF' }
$configure = @(
    '-S', $root,
    '-B', (Join-Path $out 'cmake'),
    '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DANDROID_ABI=arm64-v8a',
    '-DANDROID_PLATFORM=android-26',
    "-DANDROID_NDK=$($ndk.Root)",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DWC3_DEBUG=$debugValue",
    '-DWC3_BUILD_PLATFORM_TEST=OFF',
    '-DWC3_BUILD_APP=ON',
    "-DGAMEACTIVITY_ROOT=$ga"
)
Write-Host "[app] ndk   $($ndk.Root)"
Write-Host "[app] aar   $GamesActivityAar"
Write-Host "[app] debug $($NativeDebug.IsPresent)"
$ErrorActionPreference = 'Continue'
& $cmake @configure 2>&1 | ForEach-Object { Write-Host $_ }
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    & $cmake --build (Join-Path $out 'cmake') --config Release 2>&1 |
        ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
if ($rc -ne 0) {
    Write-Host "[app] build failed ($rc)"
    exit 1
}

$so = Join-Path $out 'cmake\libwc3gameactivity.so'
if (-not (Test-Path $so)) {
    $found = Get-ChildItem (Join-Path $out 'cmake') -Recurse -Filter 'libwc3gameactivity.so' |
        Select-Object -First 1 -ExpandProperty FullName
    if ($found) { $so = $found }
}
if (-not (Test-Path $so)) {
    Write-Host '[app] build produced no libwc3gameactivity.so'
    exit 1
}

$readelf = Join-Path $ndk.Root 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
$dynSyms = (& $readelf --dyn-syms $so 2>&1) -join "`n"
$hasMain = $dynSyms -match '\bandroid_main\b'
$hasJni = $dynSyms -match '\bJNI_OnLoad\b'
$hasGa = $dynSyms -match 'Java_com_google_androidgamesdk_GameActivity_initializeNativeCode'
Write-Host "[app] so    $so ($((Get-Item $so).Length) bytes)"
Write-Host "[app] syms  android_main=$hasMain JNI_OnLoad=$hasJni GameActivity=$hasGa"
if (-not ($hasMain -and $hasJni -and $hasGa)) {
    Write-Host '[app] required GameActivity symbols are missing'
    exit 1
}
Write-Host '[app] built'
exit 0
