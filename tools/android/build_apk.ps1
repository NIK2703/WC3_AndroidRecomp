# Build the canonical Android debug APK from this tree.
#
# Native code is built by build_app.ps1/CMake; Gradle only packages the Java
# activity and the already-built libwc3gameactivity.so.  No script from the old
# working tree is executed.  Exit codes: 0 built, 1 failure, 2 unavailable.

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Release,
    [switch]$NativeDebug,
    [switch]$Install,
    [switch]$Launch,
    [switch]$PackDynRelocs,
    [ValidateSet('android', 'relr', 'android+relr')]
    [string]$PackDynRelocsMode = 'android',
    [switch]$Icf,
    [ValidateSet('safe', 'all')]
    [string]$IcfMode = 'safe',
    [switch]$FnoPlt,
    [switch]$KeepDynsym,
    # O-52/O-55: forward the unmeasured A/B switches to build_app.ps1.
    [switch]$NativeTls,
    [switch]$NoHostHardening,
    [string]$Serial = '',
    [string]$GamesActivityAar = '',
    [string]$DebugTags = '',
    [string]$CorpusRoot = ''
)

$ErrorActionPreference = 'Stop'
if ($Release -and $NativeDebug) {
    Write-Host '[apk] -NativeDebug is incompatible with -Release'
    exit 1
}
$variant = if ($Release) { 'release' } else { 'debug' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectDir = Join-Path $root 'apps\android_gameactivity'
$outDir = Join-Path $root 'build\android_app'

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

function Get-Jdk {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += $env:JAVA_HOME }
    if ($env:USERPROFILE) { $candidates += (Join-Path $env:USERPROFILE 'tools') }
    foreach ($c in $candidates) {
        if (-not $c -or -not (Test-Path $c)) { continue }
        if (Test-Path (Join-Path $c 'bin\java.exe')) { return (Resolve-Path $c).Path }
        $child = Get-ChildItem $c -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'bin\java.exe') } |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($child) { return $child.FullName }
    }
    return $null
}

function Get-Gradle {
    $roots = @(
        (Join-Path $env:USERPROFILE '.gradle\wrapper\dists\gradle-8.10.2-all'),
        (Join-Path $env:USERPROFILE 'tools\gradle-8.10.2')
    )
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $bat = Get-ChildItem $r -Recurse -File -Filter 'gradle.bat' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($bat) { return $bat }
    }
    return $null
}

function Get-BuildTools {
    param([string]$Sdk)
    $dirs = Get-ChildItem (Join-Path $Sdk 'build-tools') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($d in $dirs) {
        if (Test-Path (Join-Path $d.FullName 'aapt2.exe')) { return $d.FullName }
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
        if (Test-Path (Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe')) {
            return (Resolve-Path $r).Path
        }
    }
    return $null
}

$sdk = Get-AndroidSdk
if (-not $sdk) { Write-Host '[apk] unavailable: Android SDK not found'; exit 2 }
$jdk = Get-Jdk
if (-not $jdk) { Write-Host '[apk] unavailable: JDK not found'; exit 2 }
$gradle = Get-Gradle
if (-not $gradle) { Write-Host '[apk] unavailable: Gradle 8.10.2 not found'; exit 2 }
$buildTools = Get-BuildTools -Sdk $sdk
if (-not $buildTools) { Write-Host '[apk] unavailable: aapt2 not found'; exit 2 }

if ($Clean) { Remove-Item $outDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $outDir | Out-Null

# Native library first; the APK must contain this exact build.
$nativeScript = Join-Path $PSScriptRoot 'build_app.ps1'
$nativeParams = @{ Variant = $variant }
if ($Clean) { $nativeParams['Clean'] = $true }
if ($GamesActivityAar) { $nativeParams['GamesActivityAar'] = $GamesActivityAar }
if ($DebugTags) { $nativeParams['DebugTags'] = $DebugTags }
if ($CorpusRoot) { $nativeParams['CorpusRoot'] = $CorpusRoot }
if ($PackDynRelocs) {
    $nativeParams['PackDynRelocs'] = $true
    $nativeParams['PackDynRelocsMode'] = $PackDynRelocsMode
}
if ($Icf) {
    $nativeParams['Icf'] = $true
    $nativeParams['IcfMode'] = $IcfMode
}
if ($FnoPlt) { $nativeParams['FnoPlt'] = $true }
if ($KeepDynsym) { $nativeParams['KeepDynsym'] = $true }
if ($NativeTls) { $nativeParams['NativeTls'] = $true }
if ($NoHostHardening) { $nativeParams['NoHostHardening'] = $true }
$ErrorActionPreference = 'Continue'
& $nativeScript @nativeParams 2>&1 | ForEach-Object { Write-Host $_ }
$nativeRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($nativeRc -ne 0) {
    Write-Host "[apk] native build failed ($nativeRc)"
    exit $nativeRc
}

$nativeSo = Join-Path $outDir "$variant\cmake\libwc3gameactivity.so"
if (-not (Test-Path $nativeSo)) {
    $found = Get-ChildItem (Join-Path $outDir "$variant\cmake") `
        -Recurse -Filter 'libwc3gameactivity.so' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($found) { $nativeSo = $found }
}
if (-not (Test-Path $nativeSo)) { Write-Host '[apk] native library missing'; exit 1 }

$jniRoot = Join-Path $outDir 'jniLibs'
$jniDir = Join-Path $jniRoot 'arm64-v8a'
New-Item -ItemType Directory -Force $jniDir | Out-Null
Copy-Item $nativeSo (Join-Path $jniDir 'libwc3gameactivity.so') -Force

# The native host is linked against the NDK's shared C++ runtime.  Android
# does not provide libc++_shared.so as a platform library, so it must ship in
# the APK next to libwc3gameactivity.so.
$ndkRoot = Get-NdkRoot -Sdk $sdk
if (-not $ndkRoot) { Write-Host '[apk] unavailable: NDK not found'; exit 2 }
$libcxx = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so'
if (-not (Test-Path $libcxx)) {
    Write-Host '[apk] unavailable: NDK libc++_shared.so for arm64-v8a not found'
    exit 2
}
Copy-Item $libcxx (Join-Path $jniDir 'libc++_shared.so') -Force

# O-41: AGP's stripReleaseDebugSymbols is a no-op for this prebuilt library in
# the useLegacyPackaging=false layout.  Strip only the release copy and only
# non-PT_LOAD metadata: .symtab/.strtab/.debug_* go away.  O-42 additionally
# localizes every non-JNI definition at link time; .dynsym still exists for the
# Java_* entry points, android_main and JNI_OnLoad.  Crash triage uses the module
# base plus the unstripped .symtab through tools/symbolize_crash.py.
# The debug copy intentionally keeps symbols for the wc3.recomp.debug package.
if ($variant -eq 'release') {
    $jniSo = Join-Path $jniDir 'libwc3gameactivity.so'
    $strip = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
    $readelf = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
    if (-not (Test-Path $strip) -or -not (Test-Path $readelf)) {
        Write-Host '[apk] unavailable: NDK llvm-strip/llvm-readelf not found'
        exit 2
    }
    & $strip --strip-all $jniSo
    if ($LASTEXITCODE -ne 0) { Write-Host '[apk] llvm-strip failed'; exit 1 }
    $sections = (& $readelf -S $jniSo) -join "`n"
    if ($LASTEXITCODE -ne 0) { Write-Host '[apk] llvm-readelf failed'; exit 1 }
    if ($sections -notmatch '\.dynsym' -or $sections -match '\.symtab' -or
        $sections -match '\.debug_') {
        Write-Host '[apk] release strip removed .dynsym or kept .symtab/.debug_*'
        exit 1
    }
    if (-not $KeepDynsym) {
        $dynsyms = (& $readelf --dyn-syms $jniSo) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            Write-Host '[apk] llvm-readelf --dyn-syms failed'
            exit 1
        }
        if ($dynsyms -notmatch 'Java_wc3_recomp_' -or
            $dynsyms -notmatch '\bandroid_main\b' -or
            $dynsyms -notmatch '\bJNI_OnLoad\b') {
            Write-Host '[apk] release JNI/android_main surface missing from .dynsym'
            exit 1
        }
        if ($dynsyms -match 'L_[0-9A-Fa-f]{8}' -or $dynsyms -match '\bwc3_') {
            Write-Host '[apk] release still exports non-JNI dynamic symbols'
            exit 1
        }
        Write-Host '[apk] release .dynsym contains JNI surface only'
    }
    Write-Host "[apk] stripped release .so: $((Get-Item $jniSo).Length) bytes"
}

# A local Maven repository keeps the build offline and independent of a
# network cache descriptor.  games-activity has no POM dependencies, so this
# is the only artifact Gradle needs for the app module.
$localMaven = Join-Path $outDir 'local-maven'
$cache = Join-Path $env:USERPROFILE '.gradle\caches\modules-2\files-2.1\androidx.games\games-activity\4.4.2'
if (-not (Test-Path $cache)) { Write-Host '[apk] unavailable: games-activity cache missing'; exit 2 }
$dest = Join-Path $localMaven 'androidx\games\games-activity\4.4.2'
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $cache -Recurse -File | Where-Object {
    $_.Extension -in '.aar', '.pom', '.jar', '.module'
} | ForEach-Object { Copy-Item $_.FullName (Join-Path $dest $_.Name) -Force }

$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$gradleApp = Join-Path $outDir 'gradle-app'
$gradleCache = Join-Path $outDir 'gradle-cache'
$aapt2 = Join-Path $buildTools 'aapt2.exe'
$gradleTask = 'assemble' + $variant.Substring(0, 1).ToUpper() +
    $variant.Substring(1)
$gradleArgs = @(
    '--offline',
    '--no-daemon',
    '--console=plain',
    '--project-cache-dir', $gradleCache,
    '-p', $projectDir,
    "-Pwc3GameActivityBuildDir=$gradleApp",
    "-Pwc3JniLibs=$jniRoot",
    "-Pwc3LocalMaven=$localMaven",
    "-Pandroid.aapt2FromMavenOverride=$aapt2",
    $gradleTask
)
Write-Host "[apk] gradle  $gradle"
Write-Host "[apk] jdk     $jdk"
Write-Host "[apk] variant $variant"
$ErrorActionPreference = 'Continue'
& $gradle @gradleArgs 2>&1 | ForEach-Object { Write-Host $_ }
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($rc -ne 0) { Write-Host "[apk] gradle failed ($rc)"; exit 1 }

$apkDir = Join-Path $gradleApp "outputs\apk\$variant"
$apk = Join-Path $apkDir "app-$variant.apk"
if (-not (Test-Path $apk)) {
    $apk = Get-ChildItem $apkDir -Filter '*.apk' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $apk -or -not (Test-Path $apk)) { Write-Host '[apk] no APK produced'; exit 1 }
$final = Join-Path $outDir "wc3-$variant.apk"
Copy-Item $apk $final -Force

# O-12: extractNativeLibs=false requires uncompressed libraries at 16 KiB page
# boundaries.  AGP 8.8 emits them that way when useLegacyPackaging is false;
# verify the artifact rather than trusting the build configuration.
$zipalign = Join-Path $buildTools 'zipalign.exe'
if (-not (Test-Path $zipalign)) { Write-Host '[apk] unavailable: zipalign not found'; exit 2 }
& $zipalign -c -P 16 4 $final 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Write-Host '[apk] APK is not 16 KiB page aligned'; exit 1 }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($final)
try {
    $required = @(
        'lib/arm64-v8a/libwc3gameactivity.so',
        'lib/arm64-v8a/libc++_shared.so'
    )
    foreach ($name in $required) {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $name }
        if (-not $entry) { Write-Host "[apk] APK lacks $name"; exit 1 }
        if ($entry.CompressedLength -ne $entry.Length) {
            Write-Host "[apk] $name is compressed; extractNativeLibs=false needs a stored library"
            exit 1
        }
        Write-Host "[apk] lib     $($entry.FullName) ($($entry.Length) bytes, stored)"
    }
    Write-Host "[apk] apk     $final ($((Get-Item $final).Length) bytes)"
} finally {
    $zip.Dispose()
}

if ($Install -or $Launch) {
    $adb = Join-Path $sdk 'platform-tools\adb.exe'
    $deviceArgs = @()
    if ($Serial) { $deviceArgs = @('-s', $Serial) }
    & $adb @deviceArgs install -r $final 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host '[apk] install failed'; exit 1 }
    if ($Launch) {
        $package = if ($variant -eq 'debug') { 'wc3.recomp.debug' } else { 'wc3.recomp' }
        & $adb @deviceArgs shell am start -n "$package/wc3.recomp.Wc3GameActivity" 2>&1 |
            ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { Write-Host '[apk] launch failed'; exit 1 }
    }
}
Write-Host '[apk] built'
exit 0
