# Build both vendored DXVK trees for Android arm64.
#
# The source is in third_party/dxvk/{1.10.3,3.1.1}; the only Win32 surface DXVK
# sees is third_party/dxvk/shim.  This is a target build, not the old tree's
# probe: it writes the two static archives that Wc3LegacyBoot.cmake links into
# libwc3gameactivity.so.  Every compiler call goes through ccache (BP-12).
#
# Exit codes: 0 built, 1 failure, 2 unavailable (SDK/NDK/ccache missing).
#
#   powershell -File tools\android\build_dxvk.ps1 -Variant debug
#   powershell -File tools\android\build_dxvk.ps1 -Variant release -Full
#
# There is deliberately no -Clean switch.  -Full recompiles every object but
# never deletes the shared cache; delete build\android\dxvk by hand for a true
# clean.
#
# DXVK objects are profile-independent and live in a shared cache.  Only the
# Win32 shim object and the isolated archives are per-variant, because the shim
# carries the WC3_DEBUG_GRAPHICS and WC3_DEBUG_DXVK_DEBUG branches.

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Variant = 'release',
    [int]$Api = 26,
    [int]$Jobs = 12,
    [switch]$Full,
    [string]$Ndk = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'tools\ccache.ps1')

# Native tools write progress to stderr.  Windows PowerShell turns that into a
# NativeCommandError under ErrorActionPreference Stop even when the tool exits
# 0, so run each native step with Continue and check its real exit code.
$script:LastNativeExit = 0
function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $FilePath @Arguments 2>&1 | Out-Host
    $script:LastNativeExit = $LASTEXITCODE
    $ErrorActionPreference = $prev
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
    param([string]$Explicit)
    $roots = @()
    if ($Explicit) { $roots += $Explicit }
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
                Root     = (Resolve-Path $r).Path
                Clang    = (Resolve-Path $clang).Path
                ClangXX  = (Resolve-Path (Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe')).Path
                Sysroot  = (Resolve-Path (Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\sysroot')).Path
                Bin      = (Resolve-Path (Join-Path $r 'toolchains\llvm\prebuilt\windows-x86_64\bin')).Path
            }
        }
    }
    return $null
}

$sdk = Get-AndroidSdk
if (-not $sdk) {
    Write-Host '[dxvk] unavailable: Android SDK not found'
    throw 'DXVK build unavailable (SDK/NDK/ccache)'
}
$ndkInfo = Get-AndroidNdk -Explicit $Ndk
if (-not $ndkInfo) {
    Write-Host '[dxvk] unavailable: Android NDK with clang.exe not found'
    throw 'DXVK build unavailable (SDK/NDK/ccache)'
}
$ccache = Get-Wc3Ccache
if (-not $ccache) {
    Write-Host '[dxvk] unavailable: ccache not found (BP-12 requires it)'
    throw 'DXVK build unavailable (SDK/NDK/ccache)'
}

$dxvkRoot = Join-Path $root 'third_party\dxvk'
$dxvk1 = Join-Path $dxvkRoot '1.10.3'
$dxvk3 = Join-Path $dxvkRoot '3.1.1'
$shim = Join-Path $dxvkRoot 'shim'
$directx = Join-Path $dxvk3 'include\native\directx'
foreach ($p in @($dxvk1, $dxvk3, $shim, (Join-Path $dxvk1 'build-win64-ucrt\compile_commands.json'),
        (Join-Path $dxvk3 'build-win64-ucrt\compile_commands.json'))) {
    if (-not (Test-Path $p)) { throw "vendored DXVK input missing: $p" }
}
$patch = Join-Path $dxvkRoot 'patches\dxvk-3.1.1-wc3-d3d9.patch'
if (-not (Test-Path $patch)) { throw "DXVK compatibility patch missing: $patch" }
$featureSource = Join-Path $dxvk3 'src\dxvk\dxvk_device_info.cpp'
if (-not (Select-String -LiteralPath $featureSource -Pattern 'storageBuffer8BitAccess, false' -Quiet)) {
    throw "DXVK 3.1.1 source does not contain the WC3 storageBuffer8BitAccess patch"
}
$cmdlistSource = Join-Path $dxvk3 'src\dxvk\dxvk_cmdlist.h'
if (-not (Select-String -LiteralPath $featureSource -Pattern 'khrMaintenance6, maintenance6, false' -Quiet) -or
    -not (Select-String -LiteralPath $cmdlistSource -Pattern 'vkCmdBindDescriptorSets2KHR != nullptr' -Quiet) -or
    -not (Select-String -LiteralPath $cmdlistSource -Pattern 'vkCmdPushConstants2KHR != nullptr' -Quiet)) {
    throw "DXVK 3.1.1 source does not contain the WC3 maintenance6 fallback"
}
$optionsSource = Join-Path $dxvk3 'src\dxvk\dxvk_options.cpp'
$optionsHeader = Join-Path $dxvk3 'src\dxvk\dxvk_options.h'
$deviceHeader = Join-Path $dxvk3 'src\dxvk\dxvk_device_info.h'
$graphicsSource = Join-Path $dxvk3 'src\dxvk\dxvk_graphics.cpp'
$contextSource = Join-Path $dxvk3 'src\dxvk\dxvk_context.cpp'
$shaderSource = Join-Path $dxvk3 'src\dxvk\dxvk_shader.cpp'
$shaderHeader = Join-Path $dxvk3 'src\dxvk\dxvk_shader.h'
$fixedFunctionSource = Join-Path $dxvk3 'src\d3d9\d3d9_fixed_function.cpp'
if (-not (Select-String -LiteralPath $optionsSource -Pattern 'WC3_DXVK_VERSION' -Quiet) -or
    -not (Select-String -LiteralPath $optionsSource -Pattern 'ignoreMissingExtensions' -Quiet) -or
    -not (Select-String -LiteralPath $optionsHeader -Pattern 'ignoreMissingExtensions' -Quiet) -or
    -not (Select-String -LiteralPath $featureSource -Pattern 'Ignoring missing required extension feature' -Quiet) -or
    -not (Select-String -LiteralPath $featureSource -Pattern 'm_ignoreMissingExtensions' -Quiet) -or
    -not (Select-String -LiteralPath $deviceHeader -Pattern 'm_ignoreMissingExtensions' -Quiet) -or
    -not (Select-String -LiteralPath $cmdlistSource -Pattern 'vkCmdBindIndexBuffer2KHR != nullptr' -Quiet) -or
    -not (Select-String -LiteralPath $cmdlistSource -Pattern 'vkCmdSetDepthClipEnableEXT != nullptr' -Quiet) -or
    -not (Select-String -LiteralPath $graphicsSource -Pattern 'extDepthClipEnable.depthClipEnable' -Quiet) -or
    -not (Select-String -LiteralPath $contextSource -Pattern 'khrLoadStoreOpNone' -Quiet) -or
    -not (Select-String -LiteralPath $optionsSource -Pattern 'useShaderModules' -Quiet) -or
    -not (Select-String -LiteralPath $optionsHeader -Pattern 'useShaderModules' -Quiet) -or
    -not (Select-String -LiteralPath $shaderSource -Pattern 'useShaderModules' -Quiet) -or
    -not (Select-String -LiteralPath $shaderSource -Pattern 'vkCreateShaderModule' -Quiet) -or
    -not (Select-String -LiteralPath $shaderHeader -Pattern 'm_modules' -Quiet)) {
    throw "DXVK 3.1.1 source does not contain the WC3 forced-compatibility fallbacks"
}
# Adreno 0.786 accepts SPIR-V 1.3 and rejects the SPIR-V 1.6 fixed-function
# shaders at pipeline creation.  The C++ static_asserts are the actual guard;
# this marker fails the script early if the source pin is reverted.
if (-not (Select-String -LiteralPath $fixedFunctionSource -Pattern 'must be SPIR-V 1.3' -Quiet)) {
    throw "DXVK 3.1.1 fixed-function shaders are not pinned to SPIR-V 1.3"
}
# Check the actual headers consumed by the compiler, not only the source marker.
$fixedFunctionHeaderDir = Join-Path $dxvk3 'build-win64-ucrt\src\d3d9\libd3d9.a.p'
foreach ($fixedFunctionName in @('vert', 'frag', 'frag_sample')) {
    $fixedFunctionHeader = Join-Path $fixedFunctionHeaderDir "d3d9_fixed_function_$fixedFunctionName.h"
    if (-not (Test-Path $fixedFunctionHeader)) {
        throw "DXVK 3.1.1 fixed-function shader header missing: $fixedFunctionHeader"
    }
    $fixedFunctionHeaderText = Get-Content -LiteralPath $fixedFunctionHeader -Raw
    if ($fixedFunctionHeaderText -notmatch '0x07230203,\s*0x00010300') {
        throw "DXVK 3.1.1 fixed-function shader $fixedFunctionName is not SPIR-V 1.3"
    }
    if ($fixedFunctionHeaderText -notmatch 'SPIRV_VERSION 0x00010300u') {
        throw "DXVK 3.1.1 fixed-function shader $fixedFunctionName is missing the WC3 SPIR-V version macro"
    }
}
$lowmemPatch = Join-Path $dxvkRoot 'patches\dxvk-3.1.1-lowmem-placed.patch'
if (-not (Test-Path $lowmemPatch)) { throw "DXVK low-memory placed patch missing: $lowmemPatch" }
$lowmemSource = Join-Path $dxvk3 'src\dxvk\dxvk_memory.cpp'
if (-not (Select-String -LiteralPath $lowmemSource -Pattern 'wc3_lowmem.h' -Quiet) -or
    -not (Select-String -LiteralPath $lowmemSource -Pattern 'm_usePlacedMemory' -Quiet) -or
    -not (Select-String -LiteralPath $featureSource -Pattern 'extMapMemoryPlaced' -Quiet)) {
    throw "DXVK 3.1.1 source does not contain the WC3 low-memory placed-mapping patch"
}
$lowmemHeader = Join-Path $dxvkRoot 'shim\wc3_lowmem.h'
if (-not (Test-Path $lowmemHeader)) { throw "DXVK low-memory shim header missing: $lowmemHeader" }

# DXVK 1.10.3 bundles Vulkan headers that predate the extension; the fallback
# header and the same source markers must survive a vendor refresh too.
$lowmem1Patch = Join-Path $dxvkRoot 'patches\dxvk-1.10.3-lowmem-placed.patch'
if (-not (Test-Path $lowmem1Patch)) { throw "DXVK 1.10.3 low-memory placed patch missing: $lowmem1Patch" }
$lowmem1VkHeader = Join-Path $dxvkRoot 'shim\wc3_vulkan_lowmem.h'
if (-not (Test-Path $lowmem1VkHeader)) { throw "DXVK low-memory Vulkan header missing: $lowmem1VkHeader" }
$lowmem1Memory = Join-Path $dxvk1 'src\dxvk\dxvk_memory.cpp'
$lowmem1Adapter = Join-Path $dxvk1 'src\dxvk\dxvk_adapter.cpp'
$lowmem1Options = Join-Path $dxvk1 'src\dxvk\dxvk_options.cpp'
if (-not (Select-String -LiteralPath $lowmem1Memory -Pattern 'm_usePlacedMemory' -Quiet) -or
    -not (Select-String -LiteralPath $lowmem1Memory -Pattern 'wc3_lowmem_acquire' -Quiet) -or
    -not (Select-String -LiteralPath $lowmem1Adapter -Pattern 'extMapMemoryPlaced' -Quiet) -or
    -not (Select-String -LiteralPath $lowmem1Adapter -Pattern 'khrMapMemory2' -Quiet) -or
    -not (Select-String -LiteralPath $lowmem1Options -Pattern 'lowMemoryPlaced' -Quiet)) {
    throw "DXVK 1.10.3 source does not contain the WC3 low-memory placed-mapping patch"
}

# Device profile 2026-11-11: the Android build defines _WIN32 for the DXVK
# shim, so DXVK's Windows timer branch (1 ms granularity, 4 ms + duration/6
# busy-wait) was compiled into the target.  At 60 FPS the presenter thread
# spun ~6.8 ms per frame and 66.85% of its CPU was clock_gettime.  Both trees
# must keep the Android absolute-clock_nanosleep path; fail early if a vendor
# refresh drops it.
$sleepHeader3 = Join-Path $dxvk3 'src\util\util_sleep.h'
$sleepSource3 = Join-Path $dxvk3 'src\util\util_sleep.cpp'
$fpsLimiter1 = Join-Path $dxvk1 'src\util\util_fps_limiter.cpp'
if (-not (Select-String -LiteralPath $sleepHeader3 -SimpleMatch -Pattern 'defined(_WIN32) && !defined(__ANDROID__)' -Quiet) -or
    -not (Select-String -LiteralPath $sleepSource3 -SimpleMatch -Pattern 'clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME' -Quiet) -or
    -not (Select-String -LiteralPath $fpsLimiter1 -SimpleMatch -Pattern 'clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME' -Quiet)) {
    throw "DXVK source does not contain the Android no-busy-wait pacing patch"
}

# Android's WSI reports VK_SUBOPTIMAL_KHR from vkQueuePresentKHR whenever the
# swap chain is not pre-rotated.  DXVK 3.1.1 treated every non-success result
# as dirty and recreated the whole swap chain per frame; 1.10.3 already keeps
# it.  Fail early if a vendor refresh restores the 3.1.1 behaviour.
$presenter3 = Join-Path $dxvk3 'src\dxvk\dxvk_presenter.cpp'
if (-not (Select-String -LiteralPath $presenter3 -SimpleMatch -Pattern 'if (m_acquireStatus < 0 || !m_swapchain)' -Quiet) -or
    -not (Select-String -LiteralPath $presenter3 -SimpleMatch -Pattern 'Presenter: VK_SUBOPTIMAL_KHR (surface is not pre-rotated), keeping swapchain' -Quiet)) {
    throw "DXVK 3.1.1 source does not contain the Android suboptimal-present patch"
}

$buildRoot = Join-Path $root 'build\android\dxvk'
$obj1 = Join-Path $buildRoot 'obj\1.10.3'
$obj3 = Join-Path $buildRoot 'obj\3.1.1'
$outDir = Join-Path $buildRoot $Variant
foreach ($d in @($obj1, $obj3, $outDir, (Join-Path $buildRoot 'tmp'))) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# --stale-only compares only the .cpp timestamp against its object.  The
# low-memory patch adds fields to DxvkDeviceFeatures, so a header change must
# rebuild every translation unit, not just the edited one.
function Test-DxvkSourcesNewerThanObjects {
    param([string[]]$SourceRoots, [string]$ObjDir)
    $newestObj = Get-ChildItem -LiteralPath $ObjDir -Recurse -File -Filter '*.o' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $newestObj) { return $true }
    $newestSrc = Get-ChildItem -LiteralPath $SourceRoots -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.h', '.hpp', '.inl', '.c', '.cpp' } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    return [bool]($newestSrc -and ($newestSrc.LastWriteTimeUtc -gt $newestObj.LastWriteTimeUtc))
}

$forceFull = [bool]$Full
if (-not $forceFull) {
    $stale1 = Test-DxvkSourcesNewerThanObjects -SourceRoots @((Join-Path $dxvk1 'src'), $shim) -ObjDir $obj1
    $stale3 = Test-DxvkSourcesNewerThanObjects -SourceRoots @((Join-Path $dxvk3 'src'), $shim) -ObjDir $obj3
    if ($stale1 -or $stale3) {
        Write-Host '[dxvk] source or header newer than cached objects; forcing full rebuild'
        $forceFull = $true
    }
}

$debug = ($Variant -eq 'debug')
$graphicsDebug = $false
$dxvkDebug = $false
if ($debug) {
    $tagPath = Join-Path $root 'config\debug_tags.txt'
    if (Test-Path $tagPath) {
        # Strip comments per line.  Joining the whole file first would let the
        # first '#' swallow every tag after it.
        $tags = @(Get-Content -LiteralPath $tagPath | ForEach-Object {
            ($_ -replace '#.*', '').Trim()
        } | Where-Object { $_ }) -join ';'
        $graphicsDebug = [bool]($tags -match '(^|[\s;,])GRAPHICS([\s;,]|$)')
        $dxvkDebug = [bool]($tags -match '(^|[\s;,])DXVK_DEBUG([\s;,]|$)')
    }
}
$debugFlags = @()
if ($debug) { $debugFlags += '-DWC3_DEBUG=1' }
if ($graphicsDebug) { $debugFlags += '-DWC3_DEBUG_GRAPHICS=1' }
if ($dxvkDebug) { $debugFlags += '-DWC3_DEBUG_DXVK_DEBUG=1' }

Write-Host "[dxvk] variant $Variant api $Api -> $outDir"
Write-Host "[dxvk] ndk $($ndkInfo.Root)"
Write-Host "[dxvk] ccache $ccache"

$probe = Join-Path $root 'build\lift-tools\dxvk_android_probe.exe'
if (-not (Test-Path $probe)) {
    throw "dxvk_android_probe.exe missing; run tools/lift/build_tools.ps1"
}
$probeArgs = @('--dxvk', $dxvk1, '--ndk', $ndkInfo.Root,
    '--api', $Api, '--objdir', $obj1, '--codegen', '--jobs', $Jobs,
    '--ccache', $ccache)
if (-not $forceFull) { $probeArgs += '--stale-only' }
Write-Host '[dxvk] compiling 1.10.3 objects'
Invoke-Native $probe $probeArgs
if ($script:LastNativeExit -ne 0) { throw "DXVK 1.10.3 object build failed: $script:LastNativeExit" }

$probeArgs3 = @('--dxvk', $dxvk3, '--ndk', $ndkInfo.Root,
    '--api', $Api, '--objdir', $obj3, '--codegen', '--jobs', $Jobs,
    '--ccache', $ccache)
if (-not $forceFull) { $probeArgs3 += '--stale-only' }
Write-Host '[dxvk] compiling 3.1.1 objects'
Invoke-Native $probe $probeArgs3
if ($script:LastNativeExit -ne 0) { throw "DXVK 3.1.1 object build failed: $script:LastNativeExit" }

$objects1 = @(Get-ChildItem $obj1 -File -Filter '*.o' | ForEach-Object { $_.FullName })
$objects3 = @(Get-ChildItem $obj3 -File -Filter '*.o' | ForEach-Object { $_.FullName })
if ($objects1.Count -lt 100) { throw "only $($objects1.Count) DXVK 1.10.3 objects; run with -Full" }
if ($objects3.Count -lt 190) { throw "only $($objects3.Count) DXVK 3.1.1 objects; run with -Full" }

# ---- Win32 surface -------------------------------------------------------
$win32Src = Join-Path $shim 'dxvk_win32.cpp'
$win32Obj = Join-Path $outDir 'dxvk_win32.o'
$commonFlags = @(
    "--target=aarch64-linux-android$Api",
    "--sysroot=$($ndkInfo.Sysroot)",
    '-std=c++17', '-O0', '-fPIC', '-fshort-wchar', '-fdeclspec',
    '-DNOMINMAX', '-D_WIN32_WINNT=0xa00',
    '-I', $shim, '-I', $directx
) + $debugFlags
$wrapper = Join-Path $root 'build\lift-tools\ccache_wrapper.exe'
if (-not (Test-Path $wrapper)) {
    throw "ccache_wrapper.exe missing; run tools/lift/build_tools.ps1"
}
$needsShim = $forceFull -or -not (Test-Path $win32Obj) -or
    ((Get-Item $win32Src).LastWriteTimeUtc -gt (Get-Item $win32Obj).LastWriteTimeUtc) -or
    ((Get-Item $lowmemHeader).LastWriteTimeUtc -gt (Get-Item $win32Obj).LastWriteTimeUtc)
if ($needsShim) {
    Write-Host "[dxvk] compiling dxvk_win32.cpp ($Variant)"
    Invoke-Native $wrapper (@($ccache, $ndkInfo.ClangXX) + $commonFlags + @('-c', $win32Src, '-o', $win32Obj))
    if ($script:LastNativeExit -ne 0) { throw "dxvk_win32.cpp failed: $script:LastNativeExit" }
}

# ---- static archives and symbol isolation --------------------------------
$archive1 = Join-Path $outDir 'libdxvk1.a'
$archive3 = Join-Path $outDir 'libdxvk3.a'
$iso1 = Join-Path $outDir 'libdxvk1_iso.a'
$iso3 = Join-Path $outDir 'libdxvk3_iso.a'
foreach ($a in @($archive1, $archive3, $iso1, $iso3)) {
    if (Test-Path $a) { Remove-Item $a -Force }
}
$ar = Join-Path $ndkInfo.Bin 'llvm-ar.exe'
Write-Host "[dxvk] archiving $($objects1.Count) 1.10.3 objects -> $archive1"
Invoke-Native $ar (@('rcs', $archive1) + $objects1 + @($win32Obj))
if ($script:LastNativeExit -ne 0) { throw 'llvm-ar failed for DXVK 1.10.3' }
Write-Host "[dxvk] archiving $($objects3.Count) 3.1.1 objects -> $archive3"
Invoke-Native $ar (@('rcs', $archive3) + $objects3)
if ($script:LastNativeExit -ne 0) { throw 'llvm-ar failed for DXVK 3.1.1' }

$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$env:TEMP = Join-Path $buildRoot 'tmp'
$env:TMP = $env:TEMP
try {
    $env:AR = Join-Path $ndkInfo.Bin 'llvm-ar.exe'
    $env:NM = Join-Path $ndkInfo.Bin 'llvm-nm.exe'
    $env:OBJCOPY = Join-Path $ndkInfo.Bin 'llvm-objcopy.exe'
    $isolate = Join-Path $root 'build\lift-tools\dxvk_isolate_archive_elf.exe'
    if (-not (Test-Path $isolate)) {
        throw "dxvk_isolate_archive_elf.exe missing; run tools/lift/build_tools.ps1"
    }
    Write-Host "[dxvk] isolating 1.10.3 symbols -> $iso1"
    Invoke-Native $isolate @('wc3_dxvk1_', $iso1, $archive1)
    if ($script:LastNativeExit -ne 0) { throw "DXVK 1.10.3 symbol isolation failed: $script:LastNativeExit" }
    Write-Host "[dxvk] isolating 3.1.1 symbols -> $iso3"
    Invoke-Native $isolate @('wc3_dxvk3_', $iso3, $archive3)
    if ($script:LastNativeExit -ne 0) { throw "DXVK 3.1.1 symbol isolation failed: $script:LastNativeExit" }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}

foreach ($a in @($iso1, $iso3)) {
    if (-not (Test-Path $a)) { throw "DXVK archive was not produced: $a" }
    Write-Host "[dxvk] $(Split-Path -Leaf $a) $((Get-Item $a).Length) bytes"
}
Write-Host "[dxvk] built ($Variant)"
return
