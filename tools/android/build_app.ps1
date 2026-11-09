# Build the canonical Android arm64 native product library.
#
# This script only invokes the new tree's CMake target.  It does not run any
# script from the old working tree.  Exit codes: 0 built, 1 failure,
# 2 unavailable (SDK/NDK/CMake or the GameActivity AAR is missing).
#
# ADR-030 / BV-3: -Variant is the single atomic input.  It selects the output
# directory, WC3_DEBUG and the diagnostic tag set, so the native half of an
# APK can no longer disagree with its Gradle half.  Debug stays optimised
# (BV-1.1): CMAKE_BUILD_TYPE is Release for both variants.

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Variant = 'release',
    [switch]$Clean,
    [string]$GamesActivityAar = '',
    [string]$CorpusRoot = '',
    [string]$DebugTags = '',
    [switch]$PackDynRelocs,
    [ValidateSet('android', 'relr', 'android+relr')]
    [string]$PackDynRelocsMode = 'android',
    [switch]$Icf,
    [ValidateSet('safe', 'all')]
    [string]$IcfMode = 'safe',
    [switch]$FnoPlt,
    [switch]$KeepDynsym,
    # O-51/O-52: opt-in native ELF TLS.  It also switches the legacy
    # emulated-TLS libsoftfloat.a input to a build of the pinned 302-source
    # surface, so this is an A/B experiment until T2 validates dlopen TLS.
    [switch]$NativeTls,
    # O-55: opt-in A/B only.  The default keeps the NDK stack canary and
    # Fortify on every non-corpus target.
    [switch]$NoHostHardening
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($CorpusRoot) {
    $corpusPath = if ([IO.Path]::IsPathRooted($CorpusRoot)) {
        $CorpusRoot
    } else {
        Join-Path $root $CorpusRoot
    }
    $CorpusRoot = (Resolve-Path $corpusPath).Path
}

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

# BV-3.1: per-variant build tree prevents a stale .so from another variant.
$out = Join-Path $root "build\android_app\$Variant"
$ga = Join-Path $out 'gameactivity-aar'
if ($Clean) {
    Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item $ga -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $ga | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($GamesActivityAar, $ga)

$debugValue = if ($Variant -eq 'debug') { 'ON' } else { 'OFF' }
# BV-5.3: release never activates a tag.  Debug gets a default host-shell set
# so the existing T2 smoke markers (boot/surface/PE) stay observable; callers
# may narrow it with -DebugTags 'GRAPHICS;HLE_SYNC'.
if ($Variant -eq 'release') {
    $DebugTags = ''
} elseif (-not $DebugTags) {
    $DebugTags = 'BOOT;ASSETS;INPUT;GRAPHICS;JNI;MEMORY;CRASH'
}
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
    "-DWC3_VARIANT=$Variant",
    "-DWC3_DEBUG=$debugValue",
    "-DWC3_DEBUG_TAGS=$DebugTags",
    '-DWC3_USE_CCACHE=ON',
    '-DWC3_REQUIRE_CCACHE=ON',
    '-DWC3_BUILD_PLATFORM_TEST=OFF',
    '-DWC3_BUILD_APP=ON',
    "-DGAMEACTIVITY_ROOT=$ga"
)
if ($CorpusRoot) {
    $configure += "-DWC3_RELIFTED_CORPUS_ROOT=$CorpusRoot"
}
# O-43/O-70 experiment: the product default stays unpacked until T2 proves
# the bionic API 26 loader and PM-22/GB-5.2 show a benefit.  Pass both values
# on every configure, not only when the switch is set: CMake cache entries
# survive a previous -PackDynRelocs run, and an omitted OFF would silently
# leave the next "default" build packed.  The mode keeps APS2 (`android`),
# RELR and the combined table selectable for the A/B run.
$packDynRelocsValue = if ($PackDynRelocs) { 'ON' } else { 'OFF' }
$configure += "-DWC3_PACK_DYN_RELOCS=$packDynRelocsValue"
$configure += "-DWC3_PACK_DYN_RELOCS_MODE=$PackDynRelocsMode"
# O-52: pass the choice on every configure; an omitted value would inherit a
# stale cache entry from a previous A/B build.
$nativeTlsValue = if ($NativeTls) { 'ON' } else { 'OFF' }
$configure += "-DWC3_NATIVE_TLS=$nativeTlsValue"
$hostHardeningValue = if ($NoHostHardening) { 'OFF' } else { 'ON' }
$configure += "-DWC3_HOST_HARDENING=$hostHardeningValue"
# O-64/O-65: pass ON and OFF on every configure.  CMake cache entries survive
# an earlier experiment, so omitting OFF here would silently keep ICF or
# -fno-plt in the next "default" build.
$icfValue = if ($Icf) { 'ON' } else { 'OFF' }
$configure += "-DWC3_ICF=$icfValue"
$configure += "-DWC3_ICF_MODE=$IcfMode"
$configure += "-DWC3_ICF_PRINT_SECTIONS=$icfValue"
$fnoPltValue = if ($FnoPlt) { 'ON' } else { 'OFF' }
$configure += "-DWC3_FNO_PLT=$fnoPltValue"
# O-42: release ships only the JNI surface.  Debug keeps all dynamic symbols so
# on-device crash logs retain L_<va> names; -KeepDynsym is the explicit escape
# hatch for a release triage build.
$hideDynsym = ($Variant -eq 'release') -and (-not $KeepDynsym)
if ($hideDynsym) {
    $configure += '-DWC3_HIDE_DYNSYM=ON'
} else {
    $configure += '-DWC3_HIDE_DYNSYM=OFF'
}
Write-Host "[app] ndk   $($ndk.Root)"
Write-Host "[app] pack_dyn_relocs $($PackDynRelocs.IsPresent) ($PackDynRelocsMode)"
Write-Host "[app] icf $($Icf.IsPresent) ($IcfMode)"
Write-Host "[app] fno_plt $($FnoPlt.IsPresent)"
Write-Host "[app] native_tls $($nativeTlsValue -eq 'ON')"
Write-Host "[app] host_hardening $($hostHardeningValue -eq 'ON')"
Write-Host "[app] hide_dynsym $hideDynsym"
Write-Host "[app] aar   $GamesActivityAar"
Write-Host "[app] debug $($Variant -eq 'debug')"
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
