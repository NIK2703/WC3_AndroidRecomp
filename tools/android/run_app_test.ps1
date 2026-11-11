# Device smoke test for the diagnostic Android host shell.
#
# This is not a T2 game test: the corpus, dispatcher and HLE are not linked
# yet.  It proves that the APK installs, starts GameActivity, initialises the
# platform and maps the licensed PE images.  The previous foreground activity
# is always restored, including when a check fails.  Exit codes: 0 pass,
# 1 failure, 2 unavailable (SDK/adb/device/PE files).

[CmdletBinding()]
param(
    [string]$Serial = '',
    [string]$PeDir = 'E:\Games\Warcraft III'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outDir = Join-Path $root 'build\android_app'
$apk = Join-Path $outDir 'wc3-debug.apk'
$package = 'com.wc3.recomp'
$activity = "$package/.Wc3GameActivity"

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

function Get-TopActivity {
    param($Adb, [string[]]$DeviceArgs)
    $dump = (& $Adb @DeviceArgs shell dumpsys activity activities 2>&1) -join "`n"
    $m = [regex]::Match($dump, 'topResumedActivity=ActivityRecord\{[^ ]+ [^ ]+ ([^ }]+)')
    if (-not $m.Success) {
        $m = [regex]::Match($dump, 'mResumedActivity: ActivityRecord\{[^ ]+ [^ ]+ ([^ }]+)')
    }
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

$sdk = Get-AndroidSdk
if (-not $sdk) { Write-Host '[app-test] unavailable: Android SDK not found'; exit 2 }
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$deviceArgs = @()
if ($Serial) { $deviceArgs = @('-s', $Serial) }
$devices = (& $adb devices 2>&1) -join "`n"
if ($devices -notmatch '\bdevice\b') {
    Write-Host '[app-test] unavailable: no connected adb device'
    exit 2
}

$war3 = Join-Path $PeDir 'war3.exe'
$game = Join-Path $PeDir 'game.dll'
if (-not (Test-Path $war3) -or -not (Test-Path $game)) {
    Write-Host "[app-test] unavailable: PE files missing under $PeDir"
    exit 2
}

$ErrorActionPreference = 'Continue'
& (Join-Path $PSScriptRoot 'build_apk.ps1') -NativeDebug -Serial $Serial 2>&1 |
    ForEach-Object { Write-Host $_ }
$buildRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildRc -ne 0) {
    Write-Host "[app-test] unavailable: APK build failed ($buildRc)"
    exit 2
}
if (-not (Test-Path $apk)) { Write-Host '[app-test] unavailable: APK missing'; exit 2 }

$previous = Get-TopActivity -Adb $adb -DeviceArgs $deviceArgs
if ($previous -and $previous.StartsWith($package)) { $previous = $null }
Write-Host "[app-test] previous activity: $(if ($previous) { $previous } else { '<home>' })"

$failed = $false
try {
    & $adb @deviceArgs shell rm -rf /data/local/tmp/wc3_pe | Out-Null
    & $adb @deviceArgs shell mkdir -p /data/local/tmp/wc3_pe | Out-Null
    & $adb @deviceArgs push $war3 /data/local/tmp/wc3_pe/war3.exe | Out-Null
    & $adb @deviceArgs push $game /data/local/tmp/wc3_pe/game.dll | Out-Null
    & $adb @deviceArgs install -r $apk | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'adb install failed' }
    & $adb @deviceArgs shell run-as $package mkdir -p files | Out-Null
    & $adb @deviceArgs shell run-as $package cp /data/local/tmp/wc3_pe/war3.exe files/war3.exe | Out-Null
    & $adb @deviceArgs shell run-as $package cp /data/local/tmp/wc3_pe/game.dll files/game.dll | Out-Null

    & $adb @deviceArgs logcat -c | Out-Null
    & $adb @deviceArgs shell am force-stop $package | Out-Null
    # A previous session may leave the device asleep or the notification shade
    # over the activity.  These are system commands, not injected input events.
    & $adb @deviceArgs shell cmd power wakeup 2>&1 | Out-Null
    & $adb @deviceArgs shell wm dismiss-keyguard 2>&1 | Out-Null
    & $adb @deviceArgs shell cmd statusbar collapse 2>&1 | Out-Null
    $start = (& $adb @deviceArgs shell am start -W -n $activity 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "am start failed: $start" }
    Start-Sleep -Seconds 6

    $logs = (& $adb @deviceArgs logcat -d -s wc3host:V AndroidRuntime:E '*:S' 2>&1) -join "`n"
    Write-Host $logs

    $checks = [ordered]@{
        'android_main' = 'android_main pid='
        'war3.exe'     = 'PE war3.exe base=00400000'
        'game.dll'     = 'PE game.dll base=6F000000'
        'surface'      = 'surface acquire'
    }
    foreach ($name in $checks.Keys) {
        if ($logs -notmatch [regex]::Escape($checks[$name])) {
            Write-Host "[app-test] FAIL: missing $name"
            $failed = $true
        }
    }
    if ($logs -match 'FATAL EXCEPTION|UnsatisfiedLinkError|guest memory init failed|platform install failed') {
        Write-Host '[app-test] FAIL: fatal startup error in logcat'
        $failed = $true
    }
    $pidText = (& $adb @deviceArgs shell pidof $package 2>&1) -join ''
    if (-not $pidText.Trim()) {
        Write-Host '[app-test] FAIL: app process is not running'
        $failed = $true
    }

    # The diagnostic panel is part of the product contract while the corpus,
    # HLE and dispatcher are absent: it must not look like a running game.
    $uiPath = Join-Path $outDir 'diagnostic_ui.xml'
    Remove-Item $uiPath -Force -ErrorAction SilentlyContinue
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $adb @deviceArgs shell rm -f /data/local/tmp/wc3_ui.xml 2>&1 | Out-Null
    $dumpOutput = (& $adb @deviceArgs shell uiautomator dump /data/local/tmp/wc3_ui.xml 2>&1) -join "`n"
    & $adb @deviceArgs pull /data/local/tmp/wc3_ui.xml $uiPath 2>&1 | Out-Null
    $ErrorActionPreference = $savedEap
    if (-not (Test-Path $uiPath) -or $dumpOutput -notmatch 'dumped to') {
        Write-Host "[app-test] FAIL: diagnostic UI dump unavailable: $dumpOutput"
        $failed = $true
    } else {
        $ui = Get-Content -Raw $uiPath
        if ($ui -notmatch 'package="com\.wc3\.recomp"') {
            Write-Host '[app-test] FAIL: diagnostic UI dump is not the app hierarchy'
            $failed = $true
        }
        $uiChecks = [ordered]@{
            'diagnostic title' = 'WC3 recomp diagnostic'
            'config state'     = 'config: ready'
            'input state'      = 'input: ready'
            'assets state'     = 'assets:'
            'corpus state'     = 'corpus: missing'
            'HLE state'        = 'HLE: missing'
            'dispatcher state' = 'dispatcher: missing'
            'game state'       = 'game: not started'
        }
        foreach ($name in $uiChecks.Keys) {
            if ($ui -notmatch [regex]::Escape($uiChecks[$name])) {
                Write-Host "[app-test] FAIL: diagnostic UI missing $name"
                $failed = $true
            }
        }
    }
} catch {
    Write-Host "[app-test] FAIL: $($_.Exception.Message)"
    $failed = $true
} finally {
    # `am` writes warnings to stderr when the previous task is already on top;
    # those are not test failures.  Native stderr becomes a terminating error
    # under ErrorActionPreference=Stop in Windows PowerShell, so relax it only
    # for these restore commands.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $adb @deviceArgs shell am force-stop $package 2>&1 | Out-Null
    if ($previous) {
        & $adb @deviceArgs shell am start -n $previous 2>&1 | Out-Null
    } else {
        & $adb @deviceArgs shell am start -a android.intent.action.MAIN -c android.intent.category.HOME 2>&1 | Out-Null
    }
    $ErrorActionPreference = $savedEap
    Write-Host '[app-test] previous activity restored'
}

if ($failed) {
    Write-Host '[app-test] FAILED'
    exit 1
}
Write-Host '[app-test] app shell smoke: OK'
exit 0
