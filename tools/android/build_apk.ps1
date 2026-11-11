# Build the canonical Android APK from this tree.
#
# Native code is built by build_app.ps1/CMake; Gradle only packages the Java
# activity and the already-built libwc3gameactivity.so.  No script from the old
# working tree is executed.  Exit codes: 0 built, 1 failure, 2 unavailable.
#
# There is deliberately no -Clean switch: building must never delete the
# native or DXVK output as a side effect.  A true clean rebuild is a manual
# action (delete build\android_app and build\android\dxvk by hand).

[CmdletBinding()]
param(
    [switch]$Release,
    [switch]$NativeDebug,
    [switch]$Install,
    [switch]$Launch,
    [switch]$KeepDynsym,
    # O-52: initial-exec is rejected by bionic for a dlopen'ed app library.
    # Only global-dynamic is accepted.
    [ValidateSet('global-dynamic')]
    [string]$TlsModel = 'global-dynamic',
    # O-55: security A/B only; this is not a performance optimization.
    [switch]$NoHostHardening,
    # W3L/PvPGN/rubattle compat profile.  The default is rubattle so a plain
    # APK build can log in to rubattle.net without an extra flag; off is the
    # explicit exact baseline.
    [ValidateSet('auto', 'rubattle', 'pvpgn', 'off')]
    [string]$W3LCompat = 'rubattle',
    [ValidateRange(30, 255)]
    [int]$W3LLatency = 100,
    [string]$Serial = '',
    [string]$GamesActivityAar = '',
    # Product builds use the relifted corpus by default.  Pass -CorpusRoot ''
    # together with -W3LCompat off for the legacy exact-baseline build.
    [string]$CorpusRoot = 'build/corpus-full'
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

New-Item -ItemType Directory -Force $outDir | Out-Null

# Native library first; the APK must contain this exact build.
$nativeScript = Join-Path $PSScriptRoot 'build_app.ps1'
$nativeParams = @{ Variant = $variant }
if ($GamesActivityAar) { $nativeParams['GamesActivityAar'] = $GamesActivityAar }
# Always forward the value, including an explicit empty string: the empty
# value is the legacy exact-baseline escape hatch and must override the
# build_app default.
$nativeParams['CorpusRoot'] = $CorpusRoot
if ($KeepDynsym) { $nativeParams['KeepDynsym'] = $true }
$nativeParams['TlsModel'] = $TlsModel
if ($NoHostHardening) { $nativeParams['NoHostHardening'] = $true }
# Forward the compat choice on every APK build; build_app resolves auto
# against CorpusRoot and passes ON/OFF explicitly to CMake.
$nativeParams['W3LCompat'] = $W3LCompat
$nativeParams['W3LLatency'] = $W3LLatency
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

$jniRoot = Join-Path $outDir "$variant\jniLibs"
$jniDir = Join-Path $jniRoot 'arm64-v8a'
New-Item -ItemType Directory -Force $jniDir | Out-Null
$jniSo = Join-Path $jniDir 'libwc3gameactivity.so'
Copy-Item $nativeSo $jniSo -Force

# ADR-036/DRV-20: libadrenotools hooks stay pinned transitional binaries until
# the upstream sources are vendored.  Copy them next to the product .so and
# verify the exact bytes instead of trusting a stale manual jniLibs copy.
$adrenotoolsDir = $env:WC3_ADRENOTOOLS_DIR
if (-not $adrenotoolsDir) {
    $upstream = Get-Content (Join-Path $root 'config\build_upstream.json') -Raw |
        ConvertFrom-Json
    $adrenotoolsDir = Join-Path $upstream.upstream.path 'build/android-shared/adrenotools'
}
$hookHashes = [ordered]@{
    'libmain_hook.so' = 'be1cbb5774d76d1a06be0e2f67d4c11d681f9af9cb8959d5e2a6c0fa285bc103'
    'libhook_impl.so' = 'f95475bd98254c9172c9efa29ced69dc3404631fff96fbb589b4c919e6e9d855'
}
foreach ($hook in $hookHashes.Keys) {
    $source = Join-Path $adrenotoolsDir $hook
    if (-not (Test-Path $source)) {
        Write-Host "[apk] unavailable: $hook not found in $adrenotoolsDir"
        exit 2
    }
    $actual = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $hookHashes[$hook]) {
        Write-Host "[apk] $hook sha256 $actual != pinned $($hookHashes[$hook])"
        exit 1
    }
    Copy-Item -LiteralPath $source (Join-Path $jniDir $hook) -Force
}
Write-Host '[apk] hooks   libmain_hook.so/libhook_impl.so pinned'

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

$readelf = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
if (-not (Test-Path $readelf)) {
    Write-Host '[apk] unavailable: NDK llvm-readelf not found'
    exit 2
}

# O-41: AGP's stripReleaseDebugSymbols is a no-op for this prebuilt library in
# the useLegacyPackaging=false layout.  Strip only the release copy and only
# non-PT_LOAD metadata: .symtab/.strtab/.debug_* go away.  O-42 additionally
# localizes every non-JNI definition at link time; .dynsym still exists for the
# Java_* entry points, android_main and JNI_OnLoad.  Crash triage uses the module
# base plus the unstripped .symtab through tools/symbolize_crash.py.
# The debug copy intentionally keeps symbols for the wc3.recomp.debug package.
if ($variant -eq 'release') {
    $strip = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
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

# V10/T2-save: the packaged copy, after release stripping, must still carry the
# save-list marker and the overlapped-I/O libc calls.  build_app.ps1 checks the
# unstripped artifact; this check covers what Gradle will actually package.
$fixReport = Join-Path $root 'reports\generated\required_fixes.json'
if (-not (Test-Path -LiteralPath $fixReport)) {
    Write-Host "[apk] required-fix report missing: $fixReport"
    exit 1
}
$fixes = Get-Content -LiteralPath $fixReport -Raw | ConvertFrom-Json
if ($null -eq $fixes.fixes.save_list_display -or
    [int]$fixes.fixes.save_list_display.sites -lt 1 -or
    $null -eq $fixes.fixes.overlapped_save_io) {
    Write-Host '[apk] required-fix report does not declare both save fixes'
    exit 1
}
$strings = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strings.exe'
if (-not (Test-Path $strings)) {
    Write-Host '[apk] unavailable: NDK llvm-strings not found'
    exit 2
}
$packagedStrings = (& $strings $jniSo 2>&1) -join "`n"
if ($packagedStrings -notmatch 'WC3_SAVE_LIST_STRIP_W3Z') {
    Write-Host '[apk] save-list display fix marker is absent from the packaged .so'
    exit 1
}
$packagedDynsyms = (& $readelf --dyn-syms $jniSo 2>&1) -join "`n"
if ($packagedDynsyms -notmatch '\bpwrite\b' -or
    $packagedDynsyms -notmatch '\bpread\b') {
    Write-Host '[apk] overlapped save I/O is absent from the packaged .so'
    exit 1
}
Write-Host "[apk] fixes save-list=$($fixes.fixes.save_list_display.sites) site(s), overlapped-save-io=present"

# ADR-036/E7.2: DXVK 1.10.3's undefined vkGetInstanceProcAddr is satisfied by
# dxvk_entry.cpp's hidden shim.  A DT_NEEDED libvulkan.so would load the system
# loader before the custom-driver hook and defeat ICD selection.
$dynamic = (& $readelf -d $jniSo) -join "`n"
if ($LASTEXITCODE -ne 0) { Write-Host '[apk] llvm-readelf -d failed'; exit 1 }
if ($dynamic -match '\(NEEDED\).*\[libvulkan\.so') {
    Write-Host '[apk] product .so has DT_NEEDED libvulkan.so; ADR-036 requires the hidden shim'
    exit 1
}
Write-Host '[apk] product .so has no DT_NEEDED libvulkan.so'

# The APK must contain the library produced by this native build.  Gradle can
# silently pick a stale src/main/jniLibs copy when the per-buildType source
# directory is misconfigured.  Pin size and bytes before packaging, then
# verify the APK entry instead of trusting the source-set wiring.
$jniSoInfo = Get-Item -LiteralPath $jniSo
$jniSoHash = (Get-FileHash -LiteralPath $jniSo -Algorithm SHA256).Hash
if ($jniSoInfo.Length -le 0) { Write-Host '[apk] native library is empty'; exit 1 }

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
# BV-3.2: jniLibs belong to the Gradle buildType source set, so use the
# per-buildType property and keep the native directories separate.
$jniProperty = if ($variant -eq 'debug') {
    "-Pwc3JniLibsDebug=$jniRoot"
} else {
    "-Pwc3JniLibsRelease=$jniRoot"
}
# Always repackage from a clean Gradle build directory.  AGP/zipflinger
# incremental packaging keeps old entry offsets when the 153 MiB native
# library changes; repeated builds then leave unreferenced holes inside the
# APK (observed: 774 MiB file for 171 MiB of live entries).  Cleaning only the
# Gradle output keeps the native CMake build and ccache intact.
$gradleArgs = @(
    '--offline',
    '--no-daemon',
    '--console=plain',
    '--project-cache-dir', $gradleCache,
    '-p', $projectDir,
    "-Pwc3GameActivityBuildDir=$gradleApp",
    $jniProperty,
    "-Pwc3LocalMaven=$localMaven",
    "-Pandroid.aapt2FromMavenOverride=$aapt2",
    ':app:clean',
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
        'lib/arm64-v8a/libc++_shared.so',
        # libadrenotools hooks are read from the APK and extracted to filesDir
        # on first use; they must be present and stored (O-12 stays closed).
        'lib/arm64-v8a/libmain_hook.so',
        'lib/arm64-v8a/libhook_impl.so'
    )
    foreach ($name in $required) {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $name }
        if (-not $entry) { Write-Host "[apk] APK lacks $name"; exit 1 }
        if ($entry.CompressedLength -ne $entry.Length) {
            Write-Host "[apk] $name is compressed; extractNativeLibs=false needs a stored library"
            exit 1
        }
        Write-Host "[apk] lib     $($entry.FullName) ($($entry.Length) bytes, stored)"
        if ($name -eq 'lib/arm64-v8a/libwc3gameactivity.so') {
            if ($entry.Length -ne $jniSoInfo.Length) {
                Write-Host "[apk] APK native library size $($entry.Length) != $variant $($jniSoInfo.Length)"
                exit 1
            }
            $entryStream = $entry.Open()
            try {
                $sha = [System.Security.Cryptography.SHA256]::Create()
                try {
                    $entryHash = [System.BitConverter]::ToString(
                        $sha.ComputeHash($entryStream)
                    ).Replace('-', '')
                } finally {
                    $sha.Dispose()
                }
            } finally {
                $entryStream.Dispose()
            }
            if ($entryHash -ne $jniSoHash) {
                Write-Host "[apk] APK native library does not match $variant"
                exit 1
            }
            Write-Host "[apk] native  packaged .so matches $variant"
        }
    }
    # A clean package has only local/central-directory headers and the APK
    # signing block outside the entry payload.  Reject the 600+ MiB holes that
    # incremental zipflinger packaging used to leave behind.
    $liveBytes = [int64](($zip.Entries |
        Measure-Object -Property CompressedLength -Sum).Sum)
    $containerBytes = (Get-Item $final).Length
    $overhead = $containerBytes - $liveBytes
    Write-Host "[apk] zip     live=$liveBytes container=$containerBytes overhead=$overhead"
    if ($overhead -gt 8MB) {
        Write-Host "[apk] APK contains $overhead bytes of unreferenced padding; stale incremental package rejected"
        exit 1
    }
    Write-Host "[apk] apk     $final ($containerBytes bytes)"
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
