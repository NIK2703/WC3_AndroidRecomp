# Build the canonical-tree boot probe.
#
# This links the lifted war3.exe + Storm.dll corpus from the legacy build tree
# with the new tree's CPU header, a generated dispatcher and the imported HLE
# source.  It is the first executable game path in this tree; it stops at the
# launcher's LoadLibraryA("game.dll") until the game.dll corpus round is added.
#
#   powershell -File tools\build_boot_probe.ps1
#
# The legacy tree is an input corpus, not a build script: this script compiles
# every C source itself and only consumes already-generated chunk objects.
[CmdletBinding()]
param(
    [string]$LegacyRoot = 'E:\projects\warcraft3_recomp',
    [string]$NdkRoot = 'E:\Android\Sdk\ndk\29.0.14206865',
    [string]$OutDir = '',
    [int]$Api = 26,
    [switch]$War3Only,
    [switch]$D3D9,
    [switch]$VerboseCommands
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $root 'build\boot' }
$objDir = Join-Path $OutDir 'obj'
$genDir = Join-Path $OutDir 'generated'
New-Item -ItemType Directory -Force -Path $objDir, $genDir | Out-Null

$clang = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$sysroot = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\sysroot'
$readelf = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
if (-not (Test-Path $clang)) { throw "clang not found: $clang" }
if (-not (Test-Path $sysroot)) { throw "sysroot not found: $sysroot" }

$dispatchList = Join-Path $genDir 'boot_dispatch_list.h'
$prefixArgs = if ($War3Only) {
    @('--prefix', '004', '--prefix', '350')
} else {
    @('--prefix', '004', '--prefix', '350', '--prefix', '351',
      '--prefix', '352', '--prefix', '353', '--prefix', '600', '--prefix', '6F')
}
python (Join-Path $PSScriptRoot 'gen_boot_dispatch.py') `
    --list (Join-Path $LegacyRoot 'build\android\boot_probe\probe_list.h') `
    --out $dispatchList @prefixArgs
if ($LASTEXITCODE -ne 0) { throw 'dispatch list generation failed' }

$common = @(
    "--target=aarch64-linux-android$Api",
    "--sysroot=$sysroot",
    '-std=c11',
    '-O2',
    '-fPIE',
    '-ffunction-sections',
    '-fdata-sections',
    '-fno-strict-aliasing',
    '-fwrapv',
    '-ffp-contract=off',
    '-DSOFTFLOAT_FAST_INT64',
    '-DLITTLEENDIAN',
    '-DTHREAD_LOCAL=_Thread_local',
    '-DWC3_DEBUG=0',
    '-DNDEBUG',
    '-I', (Join-Path $root 'runtime\cpu'),
    '-I', (Join-Path $root 'runtime\legacy'),
    '-I', (Join-Path $root 'third_party\softfloat\source\include'),
    '-I', (Join-Path $root 'third_party\softfloat\source\8086-SSE'),
    '-I', $genDir
)

$sources = @(
    (Join-Path $root 'runtime\boot\dispatch_boot.c'),
    (Join-Path $root 'runtime\legacy\boot_probe.c'),
    (Join-Path $root 'runtime\legacy\hle_android.c'),
    (Join-Path $root 'runtime\legacy\pe32.c'),
    (Join-Path $root 'runtime\legacy\guest_mem.c')
)
$objects = @()
foreach ($src in $sources) {
    $name = [IO.Path]::GetFileNameWithoutExtension($src)
    $obj = Join-Path $objDir "$name.o"
    Write-Host "[boot] compile $([IO.Path]::GetFileName($src))"
    if ($VerboseCommands) { Write-Host ($clang + ' ' + ($common -join ' ') + ' -c ' + $src + ' -o ' + $obj) }
    & $clang @common '-c' $src '-o' $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    $objects += $obj
}

$corpusDirs = if ($War3Only) {
    @('war3', 'war3_closure', 'storm_a35')
} else {
    @('war3', 'war3_closure', 'storm_a35', 'game', 'game_extra', 'game_vt',
      'game_vtc', 'ijl', 'mss_a35', 'mssfast_a35', 'mp3dec_a35')
}
foreach ($dir in $corpusDirs) {
    $path = Join-Path $LegacyRoot "build\android\chunks\$dir"
    if (-not (Test-Path $path)) { throw "missing corpus directory: $path" }
    $chunks = Get-ChildItem $path -File -Filter '*.o' | Sort-Object Name
    if (-not $chunks) { throw "empty corpus directory: $path" }
    $objects += $chunks.FullName
}
$extras = if ($War3Only) {
    @('war3_27_extra2.o', 'storm_27_extra2_a35.o')
} else {
    @('war3_27_extra2.o', 'storm_27_extra2_a35.o')
}
foreach ($extra in $extras) {
    $path = Join-Path $LegacyRoot "build\android\boot_probe\$extra"
    if (-not (Test-Path $path)) { throw "missing corpus object: $path" }
    $objects += $path
}

if ($D3D9) {
    foreach ($name in @('hle_d3d9.o', 'hle_d3d9_dxvk.o')) {
        $path = Join-Path $LegacyRoot "build\android\boot_probe\$name"
        if (-not (Test-Path $path)) { throw "missing D3D9 object: $path" }
        $objects += $path
    }
    # dxvk_entry.cpp is rebuilt here (not linked from the legacy tree) so the
    # build does not inherit a stale WC3_ADRENOTOOLS object.  The custom-driver
    # path is opt-in and needs libadrenotools; the system Vulkan loader does
    # not.
    $dxvkSrc = Join-Path $LegacyRoot 'runtime\android\dxvk_entry.cpp'
    $dxvkObj = Join-Path $objDir 'dxvk_entry.o'
    $clangxx = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
    $cxxCommon = @(
        "--target=aarch64-linux-android$Api",
        "--sysroot=$sysroot",
        '-std=c++17',
        '-O2',
        '-fPIC',
        '-fshort-wchar',
        '-fdeclspec',
        '-DNOMINMAX',
        '-D_WIN32_WINNT=0xa00',
        '-ffunction-sections',
        '-fdata-sections',
        '-I', (Join-Path $LegacyRoot 'runtime\android\dxvk_shim'),
        '-I', (Join-Path $LegacyRoot 'third_party\dxvk-src\include\native\directx'),
        '-I', (Join-Path $LegacyRoot 'runtime\host'),
        '-I', (Join-Path $LegacyRoot 'build27'),
        '-I', (Join-Path $LegacyRoot 'runtime\android'),
        '-I', (Join-Path $root 'runtime\cpu'),
        '-I', (Join-Path $root 'runtime\legacy'),
        '-I', (Join-Path $root 'third_party\softfloat\source\include'),
        '-I', (Join-Path $root 'third_party\softfloat\source\8086-SSE')
    )
    Write-Host '[boot] compile dxvk_entry.cpp'
    & $clangxx @cxxCommon '-c' $dxvkSrc '-o' $dxvkObj
    if ($LASTEXITCODE -ne 0) { throw 'compile failed: dxvk_entry.cpp' }
    $objects += $dxvkObj
}

$out = Join-Path $OutDir 'wc3_boot_probe'
$rsp = Join-Path $OutDir 'link.rsp'
function RspPath([string]$p) { '"' + $p.Replace('\', '/') + '"' }
$rspLines = @(
    "--target=aarch64-linux-android$Api",
    "--sysroot=$($sysroot.Replace('\', '/'))",
    '-fPIE',
    '-pie',
    '-Wl,--gc-sections',
    '-Wl,-z,max-page-size=16384',
    '-Wl,--exclude-libs,ALL',
    '-o', (RspPath $out)
)
foreach ($obj in $objects) { $rspLines += (RspPath $obj) }
$rspLines += (RspPath (Join-Path $LegacyRoot 'build\android\libsoftfloat.a'))
if ($D3D9) {
    $rspLines += @('-Wl,--whole-archive')
    $rspLines += (RspPath (Join-Path $LegacyRoot 'build\android\libdxvk1_iso.a'))
    $rspLines += (RspPath (Join-Path $LegacyRoot 'build\android\libdxvk3_iso.a'))
    $rspLines += @('-Wl,--no-whole-archive')
    $rspLines += @('-lvulkan', '-llog', '-landroid', '-laaudio', '-ldl', '-lm')
} else {
    $rspLines += @('-llog', '-landroid', '-laaudio', '-ldl', '-lm')
}
Set-Content -Path $rsp -Value $rspLines -Encoding Ascii
Write-Host "[boot] link $out ($($objects.Count) corpus/source objects, D3D9=$D3D9)"
if ($D3D9) {
    $clangxx = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
    & $clangxx "@$rsp" '-nostdlib++' '-Wl,-Bstatic' '-lc++' '-lc++abi' '-Wl,-Bdynamic'
} else {
    & $clang "@$rsp"
}
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

$size = (Get-Item $out).Length
Write-Host ("[boot] built {0} ({1:N1} MiB)" -f $out, ($size / 1MB))
if (Test-Path $readelf) {
    & $readelf -h $out | Select-String 'Class:|Machine:|Type:'
}
