# Build the compiled lift toolchain owned by this tree.
#
# Every step that touches the corpus is a C program (BP-6.4, ABI-11.3).  The
# sources in tools/lift/csrc are byte-pinned copies of the upstream toolbox by
# config/build_upstream.json; tools/lift/csrc_local holds this tree's own C
# tools (corpus identity, lift invariants).  PowerShell here only orchestrates
# gcc and copies the vendor runtime next to the executables.  Products go to
# build/lift-tools, never into the source tree (R2/R3).
#
#   powershell -ExecutionPolicy Bypass -File tools/lift/build_tools.ps1
#   powershell -ExecutionPolicy Bypass -File tools/lift/build_tools.ps1 -Force -Smoke
#
# -Smoke additionally decodes x86 through capstone, merges the fixture
# addresses, hashes/verifies a deterministic corpus input set and runs the
# lift invariant checker over a clean and a deliberately corrupted fixture
# (V5.4).  A broken DLL load or a detector that stopped firing fails the build.
param(
    [switch]$Force,
    [switch]$Smoke,
    [switch]$PosixCheck,
    [string]$Gcc,
    [string]$Ccache
)
$ErrorActionPreference = 'Stop'

$toolRoot = $PSScriptRoot
$root     = (Resolve-Path (Join-Path $toolRoot '..\..')).Path
. (Join-Path $root 'tools\ccache.ps1')
$csrc      = Join-Path $toolRoot 'csrc'
$csrcLocal = Join-Path $toolRoot 'csrc_local'
$posixShim = Join-Path $toolRoot 'posix_shim'
$inc       = Join-Path $root 'third_party\capstone\include'
$bin       = Join-Path $root 'build\lift-tools'
$dll       = Join-Path $root 'third_party\capstone\bin\capstone.dll'
$fixtures  = Join-Path $root 'tests\fixtures\lift_tools'
$badFix    = Join-Path $root 'tests\fixtures\lift_tools_bad'
$reportDir = Join-Path $root 'reports\generated'

if (-not (Test-Path $reportDir)) { New-Item -ItemType Directory -Force -Path $reportDir | Out-Null }

if (-not $Gcc) {
    $Gcc = 'C:\msys64\ucrt64\bin\gcc.exe'
    if (-not (Test-Path $Gcc)) {
        $cmd = Get-Command gcc -ErrorAction SilentlyContinue
        if (-not $cmd) { throw 'gcc not found (install MSYS2 UCRT64 or pass -Gcc)' }
        $Gcc = $cmd.Source
    }
}
if (-not (Test-Path $dll)) { throw "vendor runtime missing: $dll" }
if (-not (Test-Path $bin)) { New-Item -ItemType Directory -Force -Path $bin | Out-Null }

$ccache = Get-Wc3Ccache -Explicit $Ccache
if (-not $ccache) {
    Write-Warning 'ccache not found; lift tools will build without a compiler cache'
}
$compilerExe = if ($ccache) { $ccache } else { $Gcc }
$compilerPrefix = if ($ccache) { @($Gcc.Replace('\', '/')) } else { @() }

$common = @('-O2', '-Wall', '-Wextra', '-std=c11', '-static')
$core   = @('liftcore.c', 'liftutil.c', 'liftemit.c', 'liftflags.c', 'csglue.c')
$closure2Src = @((Join-Path $csrcLocal 'closure2.c'), (Join-Path $csrc 'lift32.c')) +
    @($core | ForEach-Object { Join-Path $csrc $_ })

# out -> sources, plus an optional define and an optional source directory.
# closure.exe reuses lift32.c's lf_lift_files(), so its main must be compiled
# out.  The two csrc_local tools are owned by this tree, not by the upstream
# pin in config/build_upstream.json.
$targets = @(
    @{ out = 'triage.exe';   src = @('triage.c') },
    @{ out = 'probe.exe';    src = @('probe.c') },
    @{ out = 'expand.exe';   src = @('expand.c') },
    @{ out = 'manifest.exe'; src = @('manifest.c') },
    @{ out = 'lint.exe';     src = @('lint.c') },
    @{ out = 'map.exe';      src = @('map.c') },
    @{ out = 'addr.exe';     src = @('addr.c') },
    @{ out = 'scan.exe';     src = @('scan.c') },
    @{ out = 'ledger.exe';   src = @('ledger.c') },
    @{ out = 'stack.exe';    src = @('stack.c') },
    @{ out = 'watch.exe';    src = @('watch.c') },
    @{ out = 'refs.exe';     src = @('refs.c') },
    @{ out = 'entry.exe';    src = @('entry.c') },
    @{ out = 'hle.exe';      src = @('hle.c') },
    @{ out = 'bmp2png.exe';  src = @('bmp2png.c') },
    @{ out = 'bmpinfo.exe';  src = @('bmpinfo.c') },
    @{ out = 'lift.exe';     src = @('lift.c') },
    @{ out = 'known.exe';    src = @('known.c') },
    @{ out = 'lift32.exe';   src = @('lift32.c') + $core },
    @{ out = 'seed.exe';     src = @('seed.c') + $core },
    @{ out = 'closure.exe';  src = @('closure.c', 'lift32.c') + $core; def = 'WC3_NO_LIFT_MAIN' },
    @{ out = 'closure2.exe'; srcfull = $closure2Src; def = 'WC3_NO_LIFT_MAIN' },
    @{ out = 'cstest.exe';   src = @('cstest.c', 'csglue.c') },
    @{ out = 'corpus.exe';   src = @('corpus.c', 'sha256.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'liftpass.exe'; src = @('liftpass.c', 'sha256.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'atomiccheck.exe'; src = @('atomiccheck.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'devirtualize.exe'; src = @('devirtualize.c'); dir = $csrcLocal },
    @{ out = 'gen_boot_dispatch.exe'; src = @('gen_boot_dispatch.c'); dir = $csrcLocal },
    @{ out = 'save_list_display.exe'; src = @('save_list_display.c', 'sha256.c'); dir = $csrcLocal },
    @{ out = 'dead_flags.exe'; src = @('dead_flags.c', 'sha256.c'); dir = $csrcLocal },
    @{ out = 'return_address_elide.exe'; src = @('return_address_elide.c', 'sha256.c'); dir = $csrcLocal },
    @{ out = 'w3l_compat.exe'; src = @('w3l_compat.c', 'sha256.c'); dir = $csrcLocal },
    @{ out = 'ccache_wrapper.exe'; src = @('ccache_wrapper.c'); dir = $csrcLocal },
    @{ out = 'dxvk_isolate_archive_elf.exe'; src = @('dxvk_isolate_archive_elf.c'); dir = $csrcLocal },
    @{ out = 'flag_read_audit.exe'; src = @('flag_read_audit.c'); dir = $csrcLocal },
    @{ out = 'audit_tls.exe'; src = @('audit_tls.c', 'sha256.c'); dir = $csrcLocal },
    @{ out = 'dxvk_android_probe.exe'; src = @('dxvk_android_probe.c'); dir = $csrcLocal },
    @{ out = 'closure_pipeline.exe'; src = @('closure_pipeline.c', 'sha256.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'thunkseeds.exe'; srcfull = @((Join-Path $csrcLocal 'thunkseeds.c'), (Join-Path $csrc 'csglue.c')) },
    @{ out = 'corpusinput.exe'; src = @('corpusinput.c', 'sha256.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'corpusclosure.exe'; src = @('corpusclosure.c', 'sha256.c', 'dirwalk.c'); dir = $csrcLocal },
    @{ out = 'splitcorpus.exe'; src = @('splitcorpus.c', 'dirwalk.c'); dir = $csrcLocal }
)

$scriptTime = [IO.File]::GetLastWriteTimeUtc($PSCommandPath)
foreach ($t in $targets) {
    $srcdir = if ($t.dir) { $t.dir } else { $csrc }
    $srcs = if ($t.srcfull) { @($t.srcfull) } else { @($t.src | ForEach-Object { Join-Path $srcdir $_ }) }
    foreach ($s in $srcs) { if (-not (Test-Path $s)) { throw "missing source: $s" } }
    $out  = Join-Path $bin $t.out
    $newest = $scriptTime
    foreach ($s in $srcs) {
        $stamp = [IO.File]::GetLastWriteTimeUtc($s)
        if ($stamp -gt $newest) { $newest = $stamp }
    }
    $outTime = if (Test-Path $out) { [IO.File]::GetLastWriteTimeUtc($out) } else { [datetime]::MinValue }
    if ((-not $Force) -and ($outTime -ge $newest) -and (Test-Path (Join-Path $bin 'capstone.dll'))) {
        Write-Host ("up to date: {0}" -f $t.out)
        continue
    }
    $flags = @($common)
    if ($t.def) { $flags += "-D$($t.def)" }
    $includeArgs = @('-I', $srcdir, '-I', $csrcLocal, '-I', $csrc, '-I', $inc)
    Write-Host ("gcc -> {0}" -f $t.out)

    # Compile separately from linking.  ccache cannot cache a compiler call
    # that also links, so a one-shot `gcc sources -o exe` would always be
    # reported as "called for linking" (BP-12).
    $objDir = Join-Path $bin 'obj'
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $objects = @()
    $index = 0
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    foreach ($s in $srcs) {
        $objName = '{0}_{1}_{2}.o' -f `
            [IO.Path]::GetFileNameWithoutExtension($t.out), $index, `
            [IO.Path]::GetFileNameWithoutExtension($s)
        $obj = Join-Path $objDir $objName
        $compileArgs = @($compilerPrefix) + @($flags) + @($includeArgs) +
            @('-c', $s, '-o', $obj)
        $buildOut = & $compilerExe @compileArgs 2>&1
        $rc = $LASTEXITCODE
        if ($rc -ne 0) {
            $ErrorActionPreference = $prev
            $buildOut | ForEach-Object { Write-Host $_ }
            throw ("build failed: {0} ({1})" -f $t.out, $s)
        }
        $objects += $obj
        $index++
    }
    $linkArgs = @($compilerPrefix) + @($flags) + @($includeArgs) +
        @($objects) + @('-o', $out)
    $buildOut = & $compilerExe @linkArgs 2>&1
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($rc -ne 0) {
        $buildOut | ForEach-Object { Write-Host $_ }
        throw ("build failed: " + $t.out)
    }
}

Copy-Item -LiteralPath $dll -Destination (Join-Path $bin 'capstone.dll') -Force

if ($Smoke) {
    $cstest = Join-Path $bin 'cstest.exe'
    $decoded = & $cstest 2>&1
    if ($LASTEXITCODE -ne 0) { $decoded | ForEach-Object { Write-Host $_ }; throw 'cstest failed: capstone.dll did not load' }
    if (-not ($decoded -match 'mov\s+eax')) { throw 'cstest produced no x86 decode' }
    Write-Host 'smoke: capstone decode OK'

    $outList = Join-Path $bin 'smoke_known.h'
    $merge = & (Join-Path $bin 'lift.exe') known $outList $fixtures 2>&1
    if ($LASTEXITCODE -ne 0) { $merge | ForEach-Object { Write-Host $_ }; throw 'lift known failed' }
    $body = Get-Content -LiteralPath $outList -Raw
    if ($body -notmatch 'X\(00401000\)' -or $body -notmatch 'X\(6F00A120\)') {
        throw 'lift known did not merge the fixture addresses'
    }
    Write-Host ("smoke: address merge OK ({0})" -f ($merge -join ' '))

    # corpus.exe: identity over a deterministic input set, with a negative
    # verify so a silently broken hash cannot look like a pass (BP-6, G-05).
    $inputs   = Join-Path $fixtures 'corpus_inputs.txt'
    $hashOut  = Join-Path $bin 'smoke_corpus.sha256'
    $vaOut    = Join-Path $bin 'smoke_va.txt'
    $corpus   = Join-Path $bin 'corpus.exe'
    $hash = & $corpus hash --inputs $inputs --root $root --out $hashOut `
        --json (Join-Path $reportDir 'corpus_smoke.json') 2>&1
    if ($LASTEXITCODE -ne 0) { $hash | ForEach-Object { Write-Host $_ }; throw 'corpus hash failed' }
    $verify = & $corpus verify --inputs $inputs --root $root --expected $hashOut 2>&1
    if ($LASTEXITCODE -ne 0) { $verify | ForEach-Object { Write-Host $_ }; throw 'corpus verify failed on unchanged inputs' }
    $neg = & $corpus verify --inputs $inputs --root $root --expected $hashOut --generator other 2>&1
    if ($LASTEXITCODE -eq 0) { throw 'corpus verify accepted a changed generator' }
    $list = & $corpus list --dir $fixtures --out $vaOut 2>&1
    if ($LASTEXITCODE -ne 0) { $list | ForEach-Object { Write-Host $_ }; throw 'corpus list failed' }
    $vas = Get-Content -LiteralPath $vaOut -Raw
    foreach ($va in @('00401000', '00401030', '00401040', '6F00A120')) {
        if ($vas -notmatch $va) { throw "corpus list missed $va" }
    }
    Write-Host ("smoke: corpus hash/list OK ({0})" -f (($hash -join ' ').Trim()))

    # splitcorpus.exe: generated C is cut at function boundaries, never in the
    # middle of a function, and every chunk gets the cpu.h include.
    $splitDir = Join-Path $bin 'smoke_split'
    if (Test-Path $splitDir) { Remove-Item -Recurse -Force $splitDir }
    $split = & (Join-Path $bin 'splitcorpus.exe') (Join-Path $fixtures 'war3_smoke.c') $splitDir smoke 1 0 2>&1
    if ($LASTEXITCODE -ne 0) { $split | ForEach-Object { Write-Host $_ }; throw 'splitcorpus failed' }
    $chunks = @(Get-ChildItem -LiteralPath $splitDir -Filter '*.c')
    if ($chunks.Count -ne 2) { throw "splitcorpus wrote $($chunks.Count) chunks, expected 2" }
    foreach ($chunk in $chunks) {
        $body = Get-Content -LiteralPath $chunk.FullName -Raw
        if ($body -notmatch '#include "cpu\.h"') { throw "splitcorpus chunk missing cpu.h: $($chunk.Name)" }
        if (([regex]::Matches($body, '(?m)^void L_')).Count -ne 1) {
            throw "splitcorpus chunk does not contain exactly one function: $($chunk.Name)"
        }
    }
    Write-Host 'smoke: corpus split OK (2 chunks)'

    # liftpass.exe: a clean catalogue passes and a deliberately corrupted one
    # fails with all five detectors firing (V5.4).  Without the negative half
    # a green report would be evidence about the checker, not about the corpus.
    $listH    = Join-Path $fixtures 'lifted_smoke.h'
    $lp       = Join-Path $bin 'liftpass.exe'
    $goodJson = Join-Path $reportDir 'liftpass_smoke.json'
    $badJson  = Join-Path $reportDir 'liftpass_bad.json'
    $good = & $lp check $fixtures --list $listH --json $goodJson --self-test 2>&1
    if ($LASTEXITCODE -ne 0) { $good | ForEach-Object { Write-Host $_ }; throw 'liftpass rejected the clean fixture' }
    $goodBody = Get-Content -LiteralPath $goodJson -Raw
    if ($goodBody -notmatch '"verdict": "pass"' -or $goodBody -notmatch '"self_test": "pass"') {
        throw 'liftpass clean-fixture report is not a pass'
    }
    # The RECOMP_TODO-only body must be accepted (explicit abort(), not a
    # fall-through).  A regression here would turn real unsupported guest
    # instructions into fake "no_terminal" violations on the whole corpus.
    if ($goodBody -notmatch '00401040' -or $goodBody -notmatch '"violations": \[\s*\]') {
        throw 'liftpass clean-fixture report lost the RECOMP_TODO terminal case'
    }
    $bad = & $lp check $badFix --list $listH --json $badJson --self-test 2>&1
    if ($LASTEXITCODE -eq 0) { throw 'liftpass accepted the corrupted fixture' }
    $badBody = Get-Content -LiteralPath $badJson -Raw
    foreach ($kind in @('unknown_target', 'return_in_void', 'hle_devirtualized',
                        'static_tls_header', 'duplicate_definition')) {
        if ($badBody -notmatch $kind) { throw "liftpass detector $kind did not fire" }
    }
    Write-Host 'smoke: liftpass V5.4 detectors OK (5/5 on corrupted fixture)'

    # The closure driver itself is compiled C.  Its asset-free self-test
    # proves it still detects a corrupted corpus before it is used on a PE.
    $cp = Join-Path $bin 'closure_pipeline.exe'
    $cpOut = & $cp --self-test --root $root 2>&1
    if ($LASTEXITCODE -ne 0) { $cpOut | ForEach-Object { Write-Host $_ }; throw 'closure_pipeline self-test failed' }
    if (-not ($cpOut -match 'self-test: pass')) { throw 'closure_pipeline self-test did not report a pass' }
    Write-Host 'smoke: closure driver self-test OK (5/5 detectors)'

    # atomiccheck.exe: a clean xchg/lock-inc fixture passes, the old plain
    # lowering fails.  This is the negative half of the V5.9 atomic gate; the
    # CMake configure check runs the same detector over the real corpus.
    $atomicGood = Join-Path $bin 'smoke_atomic_good.c'
    $atomicBad  = Join-Path $bin 'smoke_atomic_bad.c'
    $goodText = @'
void L_00401000(CPU *c)
{
    { uint32_t _v = c->eax;                                      /* 00401000: xchg dword ptr [ebx], eax */
      uint32_t _old = atomic_xchg32((c->ebx), _v);               /* 00401000: xchg dword ptr [ebx], eax */
      c->eax = (uint32_t)(_old); }                               /* 00401000: xchg dword ptr [ebx], eax */
    { uint32_t _old = atomic_xadd32((c->ecx), 1u); flags_incs(c, _old, 4); } /* 00401010: lock inc dword ptr [ecx] */
    { uint32_t _t = c->esp; c->esp = (uint32_t)(c->eax); c->eax = (uint32_t)(_t); } /* 00401020: xchg esp, eax */
}
'@
    $badText = @'
void L_00401000(CPU *c)
{
    { uint32_t _t = rd32((c->ebx)); wr32((c->ebx), c->eax); c->eax = _t; } /* 00401000: xchg dword ptr [ebx], eax */
    { uint32_t _old = rd32((c->ecx)); wr32((c->ecx), _old + 1); }          /* 00401010: lock inc dword ptr [ecx] */
}
'@
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($atomicGood, $goodText, $utf8)
    [IO.File]::WriteAllText($atomicBad, $badText, $utf8)
    $ac = Join-Path $bin 'atomiccheck.exe'
    $acGoodJson = Join-Path $reportDir 'atomiccheck_smoke.json'
    $acBadJson  = Join-Path $reportDir 'atomiccheck_bad.json'
    # Deliberately failing native tools write to stderr; with the script-wide
    # Stop preference PowerShell 5.1 turns that into NativeCommandError before
    # the exit code can be inspected.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $acGood = & $ac --file $atomicGood --report $acGoodJson --require-any 2>&1
    $acGoodRc = $LASTEXITCODE
    $acBad = & $ac --file $atomicBad --report $acBadJson --require-any 2>&1
    $acBadRc = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($acGoodRc -ne 0) { $acGood | ForEach-Object { Write-Host $_ }; throw 'atomiccheck rejected the clean fixture' }
    $acGoodBody = Get-Content -LiteralPath $acGoodJson -Raw
    if ($acGoodBody -notmatch '"verdict": "pass"') { throw 'atomiccheck clean-fixture report is not a pass' }
    if ($acBadRc -eq 0) { throw 'atomiccheck accepted plain xchg/lock-inc lowering' }
    $acBadBody = Get-Content -LiteralPath $acBadJson -Raw
    if ($acBadBody -notmatch 'non_atomic_lowering' -or
        $acBadBody -notmatch '00401000' -or $acBadBody -notmatch '00401010') {
        throw 'atomiccheck did not report both plain xchg and lock-inc sites'
    }
    Write-Host 'smoke: atomic lowering gate OK (clean pass, plain xchg/lock inc fail)'

    # The compiled LiftPasses are self-checking.  Their Python counterparts
    # remain the reference implementation until the full-corpus equivalence
    # reports are archived, but the build uses these C tools.
    $dv = Join-Path $bin 'devirtualize.exe'
    $dvOut = & $dv --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($dvOut -match 'self-test.*OK')) {
        $dvOut | ForEach-Object { Write-Host $_ }
        throw 'devirtualize self-test failed'
    }
    $bg = Join-Path $bin 'gen_boot_dispatch.exe'
    $bgOut = & $bg --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($bgOut -match 'self-test: OK')) {
        $bgOut | ForEach-Object { Write-Host $_ }
        throw 'gen_boot_dispatch self-test failed'
    }
    $sl = Join-Path $bin 'save_list_display.exe'
    $slOut = & $sl --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($slOut -match 'self-test: ok')) {
        $slOut | ForEach-Object { Write-Host $_ }
        throw 'save_list_display self-test failed'
    }
    $df = Join-Path $bin 'dead_flags.exe'
    $dfOut = & $df --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($dfOut -match 'self-test: OK')) {
        $dfOut | ForEach-Object { Write-Host $_ }
        throw 'dead_flags self-test failed'
    }
    $ra = Join-Path $bin 'return_address_elide.exe'
    $raOut = & $ra --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($raOut -match 'self-test: OK')) {
        $raOut | ForEach-Object { Write-Host $_ }
        throw 'return_address_elide self-test failed'
    }
    $wc = Join-Path $bin 'w3l_compat.exe'
    $wcOut = & $wc --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($wcOut -match 'self-test: ok')) {
        $wcOut | ForEach-Object { Write-Host $_ }
        throw 'w3l_compat self-test failed'
    }
    $cw = Join-Path $bin 'ccache_wrapper.exe'
    $cwSelf = & $cw --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($cwSelf -match 'self-test: OK')) {
        $cwSelf | ForEach-Object { Write-Host $_ }
        throw 'ccache_wrapper self-test failed'
    }
    if ($ccache) {
        $cwOut = & $cw $ccache $Gcc --version 2>&1
        if ($LASTEXITCODE -ne 0 -or -not ($cwOut -match 'gcc|clang')) {
            $cwOut | ForEach-Object { Write-Host $_ }
            throw 'ccache_wrapper smoke failed'
        }
    }
    $dx = Join-Path $bin 'dxvk_isolate_archive_elf.exe'
    $dxOut = & $dx --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($dxOut -match 'self-test: OK')) {
        $dxOut | ForEach-Object { Write-Host $_ }
        throw 'dxvk_isolate_archive_elf self-test failed'
    }
    $fr = Join-Path $bin 'flag_read_audit.exe'
    $frOut = & $fr --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($frOut -match 'self-test: OK')) {
        $frOut | ForEach-Object { Write-Host $_ }
        throw 'flag_read_audit self-test failed'
    }
    $tls = Join-Path $bin 'audit_tls.exe'
    $tlsOut = & $tls --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($tlsOut -match 'self-test: OK')) {
        $tlsOut | ForEach-Object { Write-Host $_ }
        throw 'audit_tls self-test failed'
    }
    $probe = Join-Path $bin 'dxvk_android_probe.exe'
    $probeOut = & $probe --self-test 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($probeOut -match 'self-test: OK')) {
        $probeOut | ForEach-Object { Write-Host $_ }
        throw 'dxvk_android_probe self-test failed'
    }
    Write-Host 'smoke: compiled LiftPass/generator self-tests OK'
}

# BP-8.3: the pinned upstream sources are byte-frozen, so POSIX portability
# is provided by an include-path shim instead of editing them.  The shim is
# NEVER added to the normal Windows build; this mode compiles every source
# with -fsyntax-only through the POSIX branch and reports warnings honestly.
if ($PosixCheck) {
    if (-not (Test-Path (Join-Path $posixShim 'windows.h'))) {
        throw "posix shim missing: $posixShim"
    }
    $files = @(Get-ChildItem -LiteralPath $csrc -Filter '*.c') +
             @(Get-ChildItem -LiteralPath $csrcLocal -Filter '*.c')
    $bad = 0
    $warn = 0
    foreach ($f in $files) {
        # gcc writes pre-existing warnings to stderr; with ErrorActionPreference
        # = 'Stop' (set at the top) those would become terminating errors.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $syntaxArgs = @($compilerPrefix) + @(
            '-fsyntax-only', '-std=c11', '-Wall', '-Wextra',
            '-DWC3_FORCE_POSIX', '-I', $posixShim, '-I', $csrcLocal,
            '-I', $csrc, '-I', $inc, $f.FullName)
        $out = & $compilerExe @syntaxArgs 2>&1
        $rc = $LASTEXITCODE
        $ErrorActionPreference = $prev
        if ($rc -ne 0) {
            $bad++
            Write-Host ("posix: FAIL {0}" -f $f.Name)
            $out | Select-Object -First 8 | ForEach-Object { Write-Host $_ }
        } elseif ($out) { $warn++ }
    }
    Write-Host ("posix check: {0}/{1} files, {2} error(s), {3} warning(s)" -f `
        ($files.Count - $bad), $files.Count, $bad, $warn)
    if ($bad -gt 0) { throw "posix syntax check failed for $bad file(s)" }
}

# Machine-readable build identity.  BP-6.2 wants size/hash evidence; this is
# the toolchain half of it, not a corpus manifest.
$manifest = @{
    schema    = 1
    generated = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    compiler  = (& $Gcc --version | Select-Object -First 1)
    tools     = @()
}
foreach ($t in $targets) {
    $p = Join-Path $bin $t.out
    $hash = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.tools += @{ name = $t.out; bytes = (Get-Item -LiteralPath $p).Length; sha256 = $hash }
}
# PowerShell 5.1 -Encoding UTF8 emits a BOM, which json.load() rejects.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $reportDir 'lift_tools.json'),
    ($manifest | ConvertTo-Json -Depth 6), $utf8NoBom)
Write-Host ("built {0} tools into {1}" -f $targets.Count, $bin)
