# Build and, with -Run, execute the arm64 platform boundary test on a device.
#
# Exit codes: 0 pass, 1 fail, 2 unavailable (no NDK or no device).  The script
# never reports unavailable as pass.

[CmdletBinding()]
param(
    [switch]$Run,
    [string]$Serial = '',
    [string]$PeDir = '',
    [string]$Ccache = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'tools\ccache.ps1')
$ccache = Get-Wc3Ccache -Explicit $Ccache
if (-not $ccache) {
    Write-Warning 'ccache not found; platform test will build without a compiler cache'
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
    Write-Host '[platform] unavailable: Android SDK not found'
    exit 2
}
$ndk = Get-AndroidNdk
if (-not $ndk) {
    Write-Host '[platform] unavailable: Android NDK with clang.exe not found'
    exit 2
}

$outDir = Join-Path $root 'build\android_platform'
New-Item -ItemType Directory -Force $outDir | Out-Null
$binary = Join-Path $outDir 'wc3_platform_test'

$commonArgs = @(
    '--target=aarch64-linux-android26',
    '-std=c11',
    '-O2',
    '-fPIE',
    '-Wall',
    '-Wextra',
    '-I' + (Join-Path $root 'runtime\cpu'),
    '-I' + (Join-Path $root 'runtime\guest'),
    '-I' + (Join-Path $root 'runtime\platform'),
    '-I' + (Join-Path $root 'runtime\platform\android'),
    '-I' + (Join-Path $root 'third_party\softfloat\source\include')
)
$sources = @(
    (Join-Path $root 'tests\platform_android_test.c'),
    (Join-Path $root 'runtime\platform\android\platform_android.c'),
    (Join-Path $root 'runtime\platform\platform.c'),
    (Join-Path $root 'runtime\guest\guest_memory.c'),
    (Join-Path $root 'runtime\guest\pe.c')
)
$linkArgs = @('-pie', '-landroid', '-llog', '-laaudio')
$objDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null
function Invoke-Wc3Compiler {
    param([string[]]$Arguments)
    if ($ccache) {
        & $ccache $ndk.Clang.Replace('\', '/') @Arguments 2>&1 |
            ForEach-Object { Write-Host $_ }
    } else {
        & $ndk.Clang @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    }
}
Write-Host "[platform] ndk   $($ndk.Root)"
Write-Host "[platform] build $binary"
$ErrorActionPreference = 'Continue'
$objects = @()
foreach ($src in $sources) {
    $obj = Join-Path $objDir `
        ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
    Invoke-Wc3Compiler ($commonArgs + @('-c', $src, '-o', $obj))
    if ($LASTEXITCODE -ne 0) {
        $ErrorActionPreference = 'Stop'
        Write-Host "[platform] build failed ($LASTEXITCODE)"
        exit 1
    }
    $objects += $obj
}
# ccache cannot cache a compile+link invocation, so link the objects
# separately; the compile stage above remains cacheable (BP-12).
Invoke-Wc3Compiler ($commonArgs + $objects + $linkArgs + @('-o', $binary))
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($rc -ne 0) {
    Write-Host "[platform] build failed ($rc)"
    exit 1
}
if (-not $Run) {
    Write-Host '[platform] built'
    exit 0
}

$adb = Join-Path $sdk 'platform-tools\adb.exe'
$deviceArgs = @()
if ($Serial) { $deviceArgs = @('-s', $Serial) }
$ErrorActionPreference = 'Continue'
$devices = (& $adb @deviceArgs devices 2>&1 | Out-String)
$ErrorActionPreference = 'Stop'
if ($devices -notmatch '(?m)^\S+\s+device\s*$') {
    Write-Host '[platform] unavailable: no adb device'
    exit 2
}

$remote = '/data/local/tmp/wc3_platform_test'
$ErrorActionPreference = 'Continue'
& $adb @deviceArgs push $binary $remote 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host '[platform] push failed'
    exit 1
}
& $adb @deviceArgs shell chmod 755 $remote 2>&1 | ForEach-Object { Write-Host $_ }

$peArgs = @()
if ($PeDir) {
    $war3 = Join-Path $PeDir 'war3.exe'
    $game = Join-Path $PeDir 'game.dll'
    if (-not (Test-Path $war3) -or -not (Test-Path $game)) {
        Write-Host "[platform] unavailable: PE files not found in $PeDir"
        exit 2
    }
    $remotePe = '/data/local/tmp/wc3_pe'
    & $adb @deviceArgs shell mkdir -p $remotePe 2>&1 | ForEach-Object { Write-Host $_ }
    & $adb @deviceArgs push $war3 "$remotePe/war3.exe" 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host '[platform] PE push failed'; exit 1 }
    & $adb @deviceArgs push $game "$remotePe/game.dll" 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host '[platform] PE push failed'; exit 1 }
    $peArgs = @("$remotePe/war3.exe", "$remotePe/game.dll")
}
$output = (& $adb @deviceArgs shell $remote @peArgs 2>&1 | Out-String)
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Write-Host $output.TrimEnd()
if ($rc -ne 0 -or $output -notmatch 'platform android: OK') {
    Write-Host "[platform] device test failed (exit $rc)"
    exit 1
}
Write-Host '[platform] device test passed'
exit 0
