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
#
# All native variants automatically use the IDEAPAD distcc worker when it is
# reachable and keep the normal local path otherwise.  Debug-info commands
# still compile locally by construction.  The optimized variants also allow
# PGO/LTO commands; a final release_optimized build copies its merged profile
# to IDEAPAD before compiling.  See tools/dist/README.md.  Set WC3_DIST_AUTO=0
# for a forced local build.

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug', 'release_optimized', 'release_optimized_pgo')]
    [string]$Variant = 'release',
    [string]$GamesActivityAar = '',
    # release_optimized is release plus ThinLTO and a two-stage PGO profile.
    # release_optimized_pgo is the on-device training build: profile generation
    # only, with no LTO.  The final release_optimized build consumes the merged
    # profile and applies ThinLTO.
    [ValidateSet('off', 'generate', 'use')]
    [string]$Pgo = 'off',
    [string]$PgoProfile = 'build/pgo/wc3.profdata',
    [string]$PgoRawDir = '/data/data/wc3.recomp/files/pgo',
    # Instrumentation kind.  release_optimized defaults to IRPGO; 'instr' is
    # retained only for reproducing the historical front-end profile.
    [ValidateSet('instr', 'ir')]
    [string]$PgoKind = 'instr',
    # release_optimized-only code generation profile.  These parameters are
    # rejected for release/debug so the canonical product stays unchanged.
    [ValidateSet('O2', 'O3')]
    [string]$OptLevel = 'O2',
    [ValidateSet('', 'O2', 'O3')]
    [string]$CorpusOptLevel = '',
    [string]$Mtune = '',
    [switch]$OmitFramePointer,
    [switch]$SplitMachineFunctions,
    [ValidateSet('none', 'hfsort', 'cdsort')]
    [string]$CallGraphSort = 'none',
    [ValidateSet('O2', 'O3')]
    [string]$LtoOptLevel = 'O2',
    [switch]$LtoWholeProgramVisibility,
    [switch]$NoThinLtoCache,
    [switch]$EmitRelocs,
    [switch]$Polly,
    [switch]$HiddenVisibility,
    [switch]$NoSemanticInterposition,
    [string]$SymbolOrderFile = '',
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
    [int]$W3LLatency = 100,
    # 24-unit selection / adaptive 8x3 panel (gameplay/UI, not HLE).  auto
    # follows the product profile: on with a W3L product corpus, off for the
    # exact baseline.  The headless DUT never receives this profile.
    [ValidateSet('auto', 'on', 'off')]
    [string]$Selection24 = 'auto'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'tools\build_warnings.ps1')
# Release and debug builds automatically use the IDEAPAD distcc worker when it
# is reachable.  A failed probe is a normal local build; see tools/dist/README.md.
. (Join-Path $root 'tools\dist\distributed_env.ps1')

function ConvertTo-CMakeBool {
    param([bool]$Value)
    if ($Value) { return 'ON' }
    return 'OFF'
}
$isDebug = $Variant -eq 'debug'
$isOptimized = $Variant -in @('release_optimized', 'release_optimized_pgo')
$isProfileBuild = $Variant -eq 'release_optimized_pgo'
if (-not $isOptimized -and $Pgo -ne 'off') {
    throw "-Pgo is only valid with -Variant release_optimized or release_optimized_pgo"
}
if ($isOptimized -and $Pgo -eq 'off') {
    $Pgo = if ($isProfileBuild) { 'generate' } else { 'use' }
}
if ($isProfileBuild -and $Pgo -ne 'generate') {
    throw "-Variant release_optimized_pgo requires -Pgo generate"
}
if (-not $isProfileBuild -and $isOptimized -and $Pgo -eq 'generate') {
    throw "profile generation uses -Variant release_optimized_pgo, not release_optimized"
}

$isFinalOptimized = $Variant -eq 'release_optimized'
$experimentalParams = @(
    'PgoKind', 'OptLevel', 'CorpusOptLevel', 'Mtune', 'OmitFramePointer',
    'SplitMachineFunctions', 'CallGraphSort', 'LtoOptLevel',
    'LtoWholeProgramVisibility', 'NoThinLtoCache', 'EmitRelocs', 'Polly',
    'HiddenVisibility', 'NoSemanticInterposition', 'SymbolOrderFile'
)
if (-not $isOptimized) {
    foreach ($name in $experimentalParams) {
        if ($PSBoundParameters.ContainsKey($name)) {
            throw "-$name is only valid with -Variant release_optimized or " +
                  'release_optimized_pgo; release/debug must keep their ' +
                  'historical code generation'
        }
    }
}
if ($Mtune -and $Mtune -notmatch '^[A-Za-z0-9+_.-]+$') {
    throw "-Mtune contains unsupported characters: '$Mtune'"
}

# Variant defaults.  release_optimized is the measured O3/IRPGO profile; the
# training build keeps the same base code-generation and instrumentation
# flags so its counters map to the final build, but it skips LTO and
# machine-function splitting.
$effectivePgoKind = if ($PSBoundParameters.ContainsKey('PgoKind')) {
    $PgoKind
} elseif ($isOptimized) {
    'ir'
} else {
    'instr'
}
$effectiveOptLevel = if ($PSBoundParameters.ContainsKey('OptLevel')) {
    $OptLevel
} elseif ($isOptimized) {
    'O3'
} else {
    'O2'
}
$effectiveCorpusOptLevel = if ($PSBoundParameters.ContainsKey('CorpusOptLevel')) {
    $CorpusOptLevel
} else {
    $effectiveOptLevel
}
if (-not $effectiveCorpusOptLevel) {
    $effectiveCorpusOptLevel = $effectiveOptLevel
}
$effectiveOmitFramePointer = if ($PSBoundParameters.ContainsKey('OmitFramePointer')) {
    [bool]$OmitFramePointer
} elseif ($isOptimized) {
    $true
} else {
    $false
}
$effectiveSplitMachineFunctions =
    if ($PSBoundParameters.ContainsKey('SplitMachineFunctions')) {
        [bool]$SplitMachineFunctions
    } elseif ($isFinalOptimized) {
        $true
    } else {
        $false
    }
$effectiveCallGraphSort = if ($PSBoundParameters.ContainsKey('CallGraphSort')) {
    $CallGraphSort
} elseif ($isFinalOptimized) {
    'cdsort'
} else {
    'none'
}
$effectiveLtoOptLevel = if ($PSBoundParameters.ContainsKey('LtoOptLevel')) {
    $LtoOptLevel
} elseif ($isFinalOptimized) {
    'O3'
} else {
    'O2'
}
$effectiveLtoWholeProgramVisibility =
    if ($PSBoundParameters.ContainsKey('LtoWholeProgramVisibility')) {
        [bool]$LtoWholeProgramVisibility
    } elseif ($isFinalOptimized) {
        $true
    } else {
        $false
    }
$effectiveThinLtoCache = if ($PSBoundParameters.ContainsKey('NoThinLtoCache')) {
    -not [bool]$NoThinLtoCache
} elseif ($isFinalOptimized) {
    $true
} else {
    $false
}
$effectiveEmitRelocs = if ($PSBoundParameters.ContainsKey('EmitRelocs')) {
    [bool]$EmitRelocs
} else {
    $false
}
$effectivePolly = if ($PSBoundParameters.ContainsKey('Polly')) {
    [bool]$Polly
} elseif ($isOptimized) {
    # Polly is part of the experimental release_optimized profile by default.
    # It remains off for release/debug and can be disabled explicitly with
    # -Polly:$false when isolating a suspected FP/codegen regression.
    $true
} else {
    $false
}
$effectiveHiddenVisibility = if ($PSBoundParameters.ContainsKey('HiddenVisibility')) {
    [bool]$HiddenVisibility
} elseif ($isOptimized) {
    $true
} else {
    $false
}
$effectiveNoSemanticInterposition =
    if ($PSBoundParameters.ContainsKey('NoSemanticInterposition')) {
        [bool]$NoSemanticInterposition
    } elseif ($isOptimized) {
        $true
    } else {
        $false
    }
$effectiveSymbolOrderFile = if ($PSBoundParameters.ContainsKey('SymbolOrderFile')) {
    $SymbolOrderFile
} else {
    ''
}

if ($isProfileBuild -and $effectiveSplitMachineFunctions) {
    throw '-SplitMachineFunctions requires profile use; the training build ' +
          'has no profile for the split decision'
}
if (-not $isFinalOptimized -and $effectiveCallGraphSort -ne 'none') {
    throw '-CallGraphSort requires -Variant release_optimized with ThinLTO'
}
if (-not $isFinalOptimized -and $effectiveLtoWholeProgramVisibility) {
    throw '-LtoWholeProgramVisibility requires release_optimized ThinLTO'
}
if (-not $isFinalOptimized -and $effectiveThinLtoCache) {
    throw 'ThinLTO cache requires release_optimized ThinLTO'
}
if ($effectiveSymbolOrderFile) {
    $orderPath = if ([IO.Path]::IsPathRooted($effectiveSymbolOrderFile)) {
        $effectiveSymbolOrderFile
    } else {
        Join-Path $root $effectiveSymbolOrderFile
    }
    if (-not (Test-Path -LiteralPath $orderPath)) {
        throw "symbol ordering file not found: $orderPath"
    }
    $effectiveSymbolOrderFile =
        ((Resolve-Path -LiteralPath $orderPath).Path) -replace '\\', '/'
}

# O-42 is part of the release ABI check; -KeepDynsym is the explicit triage
# exception.  Whole-program visibility is only valid when that boundary holds.
$hideDynsym = (-not $isDebug) -and (-not $KeepDynsym)
if ($effectiveLtoWholeProgramVisibility -and -not $hideDynsym) {
    throw '-LtoWholeProgramVisibility requires the default hidden dynamic ' +
          'symbol surface; do not combine it with -KeepDynsym'
}

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
if (-not $isDebug) {
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

# The selection profile edits generated C too, so an explicit on without a
# corpus is a caller error.  auto keeps the exact baseline and any headless
# configure at 12 units.
$selection24Value = switch ($Selection24) {
    'on' { 'ON' }
    'off' { 'OFF' }
    default {
        if ($CorpusRoot -and $w3lCompatValue -eq 'ON') { 'ON' } else { 'OFF' }
    }
}
if ($selection24Value -eq 'ON' -and -not $CorpusRoot) {
    Write-Host '[app] -Selection24 on requires -CorpusRoot'
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
            $profdata = Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-profdata.exe'
            return [pscustomobject]@{
                Root     = (Resolve-Path $r).Path
                Clang    = (Resolve-Path $clang).Path
                Profdata = if (Test-Path $profdata) {
                    (Resolve-Path $profdata).Path
                } else {
                    $null
                }
            }
        }
    }
    return $null
}

function Get-PgoProfileKind {
    param(
        [string]$Path,
        [string]$Profdata
    )
    if (-not $Profdata -or -not (Test-Path -LiteralPath $Profdata)) {
        throw 'llvm-profdata.exe not found in the selected NDK'
    }
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
    throw "cannot determine PGO instrumentation kind for $Path; " +
          'expected llvm-profdata to report IR or Front-end'
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

# One trampoline call: when IDEAPAD is reachable, the helper starts the tunnel
# and re-runs this same script with WC3_DIST_ACTIVE=1.  The original body below
# then executes unchanged in the child scope; no try/finally wraps it.  If the
# peer is unavailable or WC3_DIST_AUTO=0, this returns and the body runs locally.
# The profile is copied before the child starts so every profile-use compile
# sees the same absolute path locally and on IDEAPAD.
$distPgoProfile = ''
if ($isOptimized -and $Pgo -eq 'use' -and $PgoProfile) {
    $profileCandidate = if ([IO.Path]::IsPathRooted($PgoProfile)) {
        $PgoProfile
    } else {
        Join-Path $root $PgoProfile
    }
    if (Test-Path -LiteralPath $profileCandidate -PathType Leaf) {
        $distPgoProfile = (Resolve-Path -LiteralPath $profileCandidate).Path
    }
}
Invoke-Wc3AutoDistributedBuild -ScriptPath $PSCommandPath `
    -Parameters $PSBoundParameters -Root $root -Variant $Variant `
    -PgoProfile $distPgoProfile

# DXVK is a target product now: both archives are built from
# third_party/dxvk by the target script, not consumed from the legacy tree.
$dxvkScript = Join-Path $PSScriptRoot 'build_dxvk.ps1'
$dxvkVariant = if ($isDebug) { 'debug' } else { 'release' }
& $dxvkScript -Variant $dxvkVariant -Api 26 -Ndk $ndk.Root

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

$warningState = Initialize-Wc3BuildWarningLog -Root $root -Kind 'native' -Variant $Variant

$debugValue = if ($isDebug) { 'ON' } else { 'OFF' }
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
$configure += "-DWC3_SELECTION_24=$selection24Value"
# release_optimized is the release product plus ThinLTO and PGO.  The training
# build deliberately skips LTO; it exists only to collect counters.  The final
# build uses both flags and the same release product inputs.
$ltoValue = if ($Variant -eq 'release_optimized') { 'THIN' } else { 'OFF' }
$configure += "-DWC3_LTO=$ltoValue"
if ($isOptimized) {
    if ($Pgo -eq 'use') {
        $profilePath = if ([IO.Path]::IsPathRooted($PgoProfile)) {
            $PgoProfile
        } else {
            Join-Path $root $PgoProfile
        }
        if (-not (Test-Path -LiteralPath $profilePath)) {
            throw "PGO profile not found: $profilePath (run the training build first)"
        }
        $PgoProfile = ((Resolve-Path -LiteralPath $profilePath).Path) -replace '\\', '/'
        # clang's -fprofile-use silently consumes a profile of the wrong kind
        # and the resulting binary has no useful profile data.  Check the
        # actual instrumentation level instead of trusting the file extension
        # or an earlier build's sidecar.
        $actualPgoKind = Get-PgoProfileKind -Path $PgoProfile `
            -Profdata $ndk.Profdata
        if ($actualPgoKind -ne $effectivePgoKind) {
            throw "PGO profile kind '$actualPgoKind' does not match requested " +
                  "'$effectivePgoKind' ($PgoProfile). Rebuild the training "
                  "profile with -PgoKind $effectivePgoKind."
        }
        $configure += '-DWC3_PGO_MODE=use'
        $configure += "-DWC3_PGO_PROFILE=$PgoProfile"
    } else {
        if (-not $PgoRawDir) {
            throw '-PgoRawDir is required for profile generation'
        }
        $configure += '-DWC3_PGO_MODE=generate'
        $configure += "-DWC3_PGO_RAW_DIR=$PgoRawDir"
    }
} else {
    $configure += '-DWC3_PGO_MODE=off'
}
$configure += "-DWC3_PGO_KIND=$effectivePgoKind"
# The release/debug profile is explicit and conservative; only
# release_optimized changes the code-generation knobs.  Passing every value
# also prevents a stale CMake cache from a previous optimized configure from
# leaking into a canonical build.
$configure += "-DWC3_OPT_LEVEL=$effectiveOptLevel"
$configure += "-DWC3_CORPUS_OPT_LEVEL=$effectiveCorpusOptLevel"
$configure += "-DWC3_MTUNE=$Mtune"
$configure += "-DWC3_OMIT_FRAME_POINTER=$(ConvertTo-CMakeBool $effectiveOmitFramePointer)"
$configure += "-DWC3_SPLIT_MACHINE_FUNCTIONS=$(ConvertTo-CMakeBool $effectiveSplitMachineFunctions)"
$configure += "-DWC3_CALL_GRAPH_SORT=$effectiveCallGraphSort"
$configure += "-DWC3_LTO_OPT_LEVEL=$effectiveLtoOptLevel"
$configure += "-DWC3_LTO_WHOLE_PROGRAM_VISIBILITY=$(ConvertTo-CMakeBool $effectiveLtoWholeProgramVisibility)"
$configure += "-DWC3_THINLTO_CACHE=$(ConvertTo-CMakeBool $effectiveThinLtoCache)"
$configure += "-DWC3_EMIT_RELOCS=$(ConvertTo-CMakeBool $effectiveEmitRelocs)"
$configure += "-DWC3_POLLY=$(ConvertTo-CMakeBool $effectivePolly)"
$configure += "-DWC3_HIDDEN_VISIBILITY=$(ConvertTo-CMakeBool $effectiveHiddenVisibility)"
$configure += "-DWC3_NO_SEMANTIC_INTERPOSITION=$(ConvertTo-CMakeBool $effectiveNoSemanticInterposition)"
$configure += "-DWC3_SYMBOL_ORDER_FILE=$effectiveSymbolOrderFile"
# Optimization policy: the generated-corpus transformations (P0-1/P0-2/P0-6,
# D1, D2, D3b, D4) are unconditional in CMake/liftcore.  There are no
# per-optimization command-line switches to pass here.  -fno-plt is not passed
# for either variant.
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
# O-42: release ships only the JNI surface.  Debug keeps all dynamic symbols so
# on-device crash logs retain L_<va> names; -KeepDynsym is the explicit escape
# hatch for a release triage build.  $hideDynsym is resolved before the
# optimize flags so whole-program visibility can reject -KeepDynsym early.
if ($hideDynsym) {
    $configure += '-DWC3_HIDE_DYNSYM=ON'
} else {
    $configure += '-DWC3_HIDE_DYNSYM=OFF'
}
Write-Host "[app] ndk   $($ndk.Root)"
Write-Host "[app] pack_dyn_relocs $($packDynRelocsValue -eq 'ON') ($packDynRelocsMode)"
Write-Host "[app] icf $($icfValue -eq 'ON') (safe)"
Write-Host "[app] native_tls $($nativeTlsValue -eq 'ON') model $TlsModel"
Write-Host "[app] lift_p0_1/2/6 unconditional"
Write-Host "[app] thin_skip_writeonly_copyin $($thinSkipCopyinValue -eq 'ON')"
Write-Host "[app] w3l_compat $w3lCompatValue ($w3lProfile, latency $W3LLatency)"
Write-Host "[app] selection24 $selection24Value"
Write-Host "[app] host_hardening $($hostHardeningValue -eq 'ON')"
Write-Host "[app] hide_dynsym $hideDynsym"
Write-Host "[app] lto   $ltoValue"
Write-Host "[app] pgo   $Pgo kind $effectivePgoKind"
Write-Host "[app] opt   $effectiveOptLevel corpus $effectiveCorpusOptLevel mtune '$Mtune'"
Write-Host "[app] frame_pointer $(-not $effectiveOmitFramePointer) split_mf $effectiveSplitMachineFunctions"
Write-Host "[app] layout cg_sort $effectiveCallGraphSort lto_opt $effectiveLtoOptLevel wpv $effectiveLtoWholeProgramVisibility thinlto_cache $effectiveThinLtoCache"
Write-Host "[app] codegen hidden_visibility $effectiveHiddenVisibility no_semantic_interposition $effectiveNoSemanticInterposition polly $effectivePolly emit_relocs $effectiveEmitRelocs"
if ($effectiveSymbolOrderFile) {
    Write-Host "[app] symbol_order $effectiveSymbolOrderFile"
}
if ($isOptimized) {
    if ($Pgo -eq 'use') {
        Write-Host "[app] pgo_profile $PgoProfile"
    } else {
        Write-Host "[app] pgo_raw_dir $PgoRawDir"
    }
}
Write-Host "[app] aar   $GamesActivityAar"
Write-Host "[app] debug $isDebug"
$ErrorActionPreference = 'Continue'
$configureOut = @(& $cmake @configure 2>&1)
$configureOut | ForEach-Object { Write-Host $_ }
Add-Wc3BuildWarnings -Lines $configureOut -Phase 'cmake-configure'
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    $buildOut = @(& $cmake --build (Join-Path $out 'cmake') --config Release 2>&1)
    $buildOut | ForEach-Object { Write-Host $_ }
    Add-Wc3BuildWarnings -Lines $buildOut -Phase 'cmake-build'
    $rc = $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
if ($rc -ne 0) {
    Complete-Wc3BuildWarningLog -Status failed -ExitCode $rc
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

# A linked helper library is not evidence that the generated corpus actually
# calls it.  An incremental build once reused pre-selection24 leaf_call
# objects, so the .so contained wc3_sel24_* but every call site still had the
# original 12-unit code.  The corpus archive is the artifact that must carry
# the undefined references; check it before trusting the product marker.
if ($selection24Value -eq 'ON') {
    $nm = Join-Path $ndk.Root 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
    $corpusArchive = Join-Path $out 'cmake\libwc3_corpus_relifted.a'
    if (-not (Test-Path -LiteralPath $corpusArchive)) {
        Write-Host "[app] selection24 corpus archive missing: $corpusArchive"
        exit 1
    }
    $corpusSyms = (& $nm -u $corpusArchive 2>&1) -join "`n"
    foreach ($sym in @(
        'wc3_sel24_init_group',
        'wc3_sel24_note_selection',
        'wc3_sel24_select',
        'wc3_sel24_hide_buttons',
        'wc3_sel24_fill',
        'wc3_sel24_cap')) {
        if ($corpusSyms -notmatch ('\b' + [regex]::Escape($sym) + '\b')) {
            Write-Host "[app] selection24 call site is absent from the corpus archive: $sym"
            exit 1
        }
    }
    Write-Host '[app] selection24 corpus calls present'
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
if ($selection24Value -eq 'ON' -and
    $soStrings -notmatch 'WC3_SELECTION_24') {
    Write-Host '[app] selection24 marker is absent from the product .so'
    exit 1
}
# The cursor switch is a runtime launch setting, but it can only work when the
# product link kept the --wrap member and resolved the generated corpus call
# through it.  Check the marker and the wrapper symbol in the actual .so.
if ($soStrings -notmatch 'WC3_HIDE_CURSOR') {
    Write-Host '[app] cursor-hide build marker is absent from the product .so'
    exit 1
}
$nm = Join-Path $ndk.Root 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
$soSymbols = (& $nm $so 2>&1) -join "`n"
if ($soSymbols -notmatch '\b__wrap_L_6F1A47B0\b' -or
    $soSymbols -notmatch '\b__wrap_L_6F342220\b') {
    Write-Host '[app] cursor-hide wrapper is absent from the product .so'
    exit 1
}
if ($dynSyms -notmatch '\bpwrite\b' -or $dynSyms -notmatch '\bpread\b') {
    Write-Host '[app] overlapped save I/O is absent from the product .so'
    exit 1
}
Write-Host "[app] fixes save-list=$($fixes.fixes.save_list_display.sites) site(s), overlapped-save-io=present, selection24=$($selection24Value -eq 'ON'), cursor-hide=present"
Complete-Wc3BuildWarningLog -Status ok -ExitCode 0
Write-Host '[app] built'
exit 0
