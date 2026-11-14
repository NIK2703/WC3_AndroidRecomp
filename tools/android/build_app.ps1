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
#
# There is deliberately no -Clean switch; delete build\android_app\<variant>
# and build\android\dxvk by hand if a true clean rebuild is required.

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Variant = 'release',
    [string]$GamesActivityAar = '',
    # Product builds require the relifted corpus.  SoftFloat was removed, so
    # the prebuilt legacy corpus is not a linkable product input.
    [string]$CorpusRoot = 'build/corpus-full',
    [switch]$KeepDynsym,
    # O-52: initial-exec is rejected by bionic for a dlopen'ed app library.
    # Only global-dynamic is accepted; the switch remains so old A/B commands
    # fail loudly instead of silently changing TLS models.
    [ValidateSet('global-dynamic')]
    [string]$TlsModel = 'global-dynamic',
    # O-55: opt-out A/B only.  The default keeps the NDK stack canary and
    # Fortify on every non-corpus target.  This is a security control, not a
    # performance optimization.
    [switch]$NoHostHardening,
    # W3L/PvPGN/rubattle compat profile.  The default is rubattle so a plain
    # product build can log in to rubattle.net without an extra flag; off is
    # the explicit exact baseline.
    [ValidateSet('auto', 'rubattle', 'pvpgn', 'off')]
    [string]$W3LCompat = 'rubattle',
    [ValidateRange(30, 255)]
    [int]$W3LLatency = 100
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# BV-5.3: the debug tag list lives in config/debug_tags.txt, not in this
# script.  The file is parsed here so a build only needs the variant; there is
# no command-line tag override.  Tags are checked against the single registry
# in wc3_debug.h so a typo cannot silently disable a subsystem.
function Get-DeclaredDebugTags {
    param([string]$HeaderPath)
    if (-not (Test-Path -LiteralPath $HeaderPath)) {
        throw "debug tag registry not found: $HeaderPath"
    }
    $known = @{}
    $text = [IO.File]::ReadAllText($HeaderPath)
    foreach ($m in [regex]::Matches($text,
                '#define\s+WC3_DEBUG_([A-Za-z0-9_]+)')) {
        $known[$m.Groups[1].Value.ToUpperInvariant()] = $true
    }
    return $known
}

function ConvertTo-DebugTagList {
    param(
        [string]$Text,
        [string]$Source,
        [hashtable]$Known
    )
    $tags = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($Text -split "`r?`n")) {
        $line = $line -replace '#.*$', ''
        foreach ($piece in ($line -split '[;,\s]+')) {
            $tag = $piece.Trim().ToUpperInvariant()
            if (-not $tag) { continue }
            if ($tag.StartsWith('WC3_DEBUG_')) {
                $tag = $tag.Substring('WC3_DEBUG_'.Length)
            }
            if (-not $Known.ContainsKey($tag)) {
                throw "unknown debug tag '$piece' in $Source; " +
                      'declare it in runtime/diag/wc3_debug.h'
            }
            if (-not $tags.Contains($tag)) {
                $tags.Add($tag)
            }
        }
    }
    return $tags
}

function Read-DebugTagsFile {
    param(
        [string]$Path,
        [hashtable]$Known
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "debug tag file not found: $Path"
    }
    $text = [IO.File]::ReadAllText($Path)
    return @(ConvertTo-DebugTagList -Text $text -Source $Path -Known $Known)
}

# Resolve the tag set before the first side effect (SDK checks, AAR
# extraction).  A broken tag file must fail the build before it touches
# output.
if ($Variant -eq 'release') {
    $DebugTags = ''
} else {
    $tagPath = Join-Path $root 'config\debug_tags.txt'
    $knownTags = Get-DeclaredDebugTags `
        -HeaderPath (Join-Path $root 'runtime\diag\wc3_debug.h')
    $tagList = @(Read-DebugTagsFile -Path $tagPath -Known $knownTags)
    $DebugTags = $tagList -join ';'
    Write-Host "[app] debug_tags '$DebugTags' (from $tagPath)"
}

if ($CorpusRoot) {
    $corpusPath = if ([IO.Path]::IsPathRooted($CorpusRoot)) {
        $CorpusRoot
    } else {
        Join-Path $root $CorpusRoot
    }
    if (-not (Test-Path -LiteralPath $corpusPath)) {
        Write-Host "[app] corpus root not found: $corpusPath"
        exit 1
    }
    $CorpusRoot = (Resolve-Path -LiteralPath $corpusPath).Path
}

# The compat profile edits generated C, so it needs the relifted corpus.  An
# explicit rubattle/pvpgn request without -CorpusRoot is a caller error, not a
# silent downgrade to the baseline.
$w3lProfile = 'rubattle'
switch ($W3LCompat) {
    'off' { $w3lCompatValue = 'OFF' }
    'pvpgn' { $w3lCompatValue = 'ON'; $w3lProfile = 'pvpgn' }
    'rubattle' { $w3lCompatValue = 'ON' }
    default {
        $w3lCompatValue = if ($CorpusRoot) { 'ON' } else { 'OFF' }
    }
}
if ($w3lCompatValue -eq 'ON' -and -not $CorpusRoot) {
    Write-Host '[app] -W3LCompat rubattle/pvpgn requires -CorpusRoot'
    exit 1
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

# DXVK is a target product now: both archives are built from
# third_party/dxvk by the target script, not consumed from the legacy tree.
$dxvkScript = Join-Path $PSScriptRoot 'build_dxvk.ps1'
& $dxvkScript -Variant $Variant -Api 26 -Ndk $ndk.Root

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
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item $ga -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $ga | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($GamesActivityAar, $ga)

$debugValue = if ($Variant -eq 'debug') { 'ON' } else { 'OFF' }
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
if (-not $CorpusRoot) {
    throw 'SoftFloat was removed; a corpus-less legacy product build is not supported. Pass -CorpusRoot build/corpus-full.'
}
$configure += "-DWC3_RELIFTED_CORPUS_ROOT=$CorpusRoot"
# Pass the compat mode on every configure: a cached ON from an earlier
# rubattle build must not survive an explicit -W3LCompat off.
$configure += "-DWC3_W3L_COMPAT=$w3lCompatValue"
$configure += "-DWC3_W3L_PROFILE=$w3lProfile"
$configure += "-DWC3_W3L_LATENCY=$W3LLatency"
# Optimization policy: the generated-corpus transformations (P0-1/P0-2/P0-6,
# D1, D2, D3b) are unconditional in CMake/liftcore.  There are no
# per-optimization command-line switches to pass here.
$packDynRelocsValue = 'ON'
$packDynRelocsMode = 'android'
$nativeTlsValue = 'ON'
$thinSkipCopyinValue = 'ON'
$configure += "-DWC3_PACK_DYN_RELOCS=$packDynRelocsValue"
$configure += "-DWC3_PACK_DYN_RELOCS_MODE=$packDynRelocsMode"
$configure += "-DWC3_NATIVE_TLS=$nativeTlsValue"
$configure += "-DWC3_TLS_MODEL=$TlsModel"
$configure += "-DWC3_THIN_SKIP_WRITEONLY_COPYIN=$thinSkipCopyinValue"
$hostHardeningValue = if ($NoHostHardening) { 'OFF' } else { 'ON' }
$configure += "-DWC3_HOST_HARDENING=$hostHardeningValue"
$icfValue = 'ON'
$configure += "-DWC3_ICF=$icfValue"
$configure += "-DWC3_ICF_MODE=safe"
$configure += "-DWC3_ICF_PRINT_SECTIONS=ON"
$fnoPltValue = 'ON'
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
Write-Host "[app] pack_dyn_relocs $($packDynRelocsValue -eq 'ON') ($packDynRelocsMode)"
Write-Host "[app] icf $($icfValue -eq 'ON') (safe)"
Write-Host "[app] fno_plt $($fnoPltValue -eq 'ON')"
Write-Host "[app] native_tls $($nativeTlsValue -eq 'ON') model $TlsModel"
Write-Host "[app] lift_p0_1/2/6 unconditional"
Write-Host "[app] thin_skip_writeonly_copyin $($thinSkipCopyinValue -eq 'ON')"
Write-Host "[app] w3l_compat $w3lCompatValue ($w3lProfile, latency $W3LLatency)"
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

# V10/T2-save: a rebuild is not "the product" unless both known save fixes are
# in the artifact.  The CMake configure step writes the corpus/fix contract;
# these checks read the actual .so instead of trusting the command line.
$fixReport = Join-Path $root 'reports\generated\required_fixes.json'
if (-not (Test-Path -LiteralPath $fixReport)) {
    Write-Host "[app] required-fix report missing: $fixReport"
    exit 1
}
$fixes = Get-Content -LiteralPath $fixReport -Raw | ConvertFrom-Json
if ($null -eq $fixes.fixes.save_list_display -or
    [int]$fixes.fixes.save_list_display.sites -lt 1) {
    Write-Host '[app] required-fix report does not declare the save-list fix'
    exit 1
}
if ($null -eq $fixes.fixes.overlapped_save_io) {
    Write-Host '[app] required-fix report does not declare the overlapped save-I/O fix'
    exit 1
}

$strings = Join-Path $ndk.Root 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strings.exe'
if (-not (Test-Path $strings)) {
    Write-Host '[app] unavailable: NDK llvm-strings not found'
    exit 2
}
$soStrings = (& $strings $so 2>&1) -join "`n"
if ($soStrings -notmatch 'WC3_SAVE_LIST_STRIP_W3Z') {
    Write-Host '[app] save-list display fix marker is absent from the product .so'
    exit 1
}
if ($dynSyms -notmatch '\bpwrite\b' -or $dynSyms -notmatch '\bpread\b') {
    Write-Host '[app] overlapped save I/O is absent from the product .so'
    exit 1
}
Write-Host "[app] fixes save-list=$($fixes.fixes.save_list_display.sites) site(s), overlapped-save-io=present"
Write-Host '[app] built'
exit 0
