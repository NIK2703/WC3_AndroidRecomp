# Build + install the full nfsu2 audio port on a connected device.
#
# ADR-032/033/034 / AUDIO_ARCHITECTURE.md A-14/A-15/A-16.  A-16 removed the
# v2 opt-out: the stale wave2_v2_off file from the rev-6 rollback is IGNORED
# by wc3_hle_winmm_waveout_install().  The new architecture is the product
# path in every variant as soon as a freshly built APK is installed.  The only
# rollback is the explicit opt-in file wave2_legacy_on.
#
# Usage (PowerShell, from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools\enable_pull_audio.ps1 `
#       -SdkDir E:\Android\Sdk -Serial <adb-serial>
#
#   # APK already built/installed, only clear the legacy opt-in:
#   powershell -ExecutionPolicy Bypass -File tools\enable_pull_audio.ps1 -SkipBuild
#
# Rollback (instant, no rebuild):
#   adb shell touch <DataDir>/wave2_legacy_on
param(
    [string]$SdkDir = $env:ANDROID_HOME,
    [string]$Serial = "",
    [string]$DataDir = "/sdcard/Android/data/wc3.recomp/files",
    [string]$Package = "wc3.recomp",
    [switch]$SkipBuild,
    [switch]$Rollback
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$adb = if ($SdkDir) { Join-Path $SdkDir "platform-tools\adb.exe" } else { "adb" }
$adbArgs = @()
if ($Serial) { $adbArgs += @("-s", $Serial) }

function Invoke-Adb([string[]]$Arguments) {
    & $adb @adbArgs @Arguments
    if ($LASTEXITCODE -ne 0) { throw "adb $($Arguments -join ' ') failed" }
}

if (-not (Get-Command $adb -ErrorAction SilentlyContinue)) {
    throw "adb not found. Set -SdkDir or add platform-tools to PATH."
}
Invoke-Adb @("devices")
Write-Host "Package: $Package  DataDir: $DataDir"

if ($Rollback) {
    Invoke-Adb @("shell", "mkdir -p $DataDir")
    Invoke-Adb @("shell", "touch $DataDir/wave2_legacy_on")
    Write-Host "ROLLBACK ACTIVE: as-built legacy WINMM will be used on next launch."
    return
}

if (-not $SkipBuild) {
    $gradle = Join-Path $root "apps\android_gameactivity\gradlew.bat"
    if (-not (Test-Path $gradle)) { $gradle = "gradle" }
    Push-Location (Join-Path $root "apps\android_gameactivity")
    try {
        if ($gradle -eq "gradle") {
            & gradle assembleRelease
        } else {
            & $gradle assembleRelease
        }
        if ($LASTEXITCODE -ne 0) { throw "assembleRelease failed" }
    } finally { Pop-Location }

    $apk = Join-Path $root "apps\android_gameactivity\app\build\outputs\apk\release\app-release.apk"
    if (-not (Test-Path $apk)) {
        $found = Get-ChildItem -Path (Join-Path $root "apps\android_gameactivity") `
            -Filter "*.apk" -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "release" } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $found) { throw "release APK not found under apps/android_gameactivity" }
        $apk = $found.FullName
    }
    Write-Host "Installing $apk"
    Invoke-Adb @("install", "-r", $apk)
}

# A-16: no opt-out exists.  Clear the legacy opt-in and the stale rev-6 file
# so the app state cannot be mistaken for a rollback on the next launch.
Invoke-Adb @("shell", "mkdir -p $DataDir")
Invoke-Adb @("shell", "rm -f $DataDir/wave2_legacy_on")
Invoke-Adb @("shell", "rm -f $DataDir/wave2_v2_off")
Invoke-Adb @("shell", "rm -f $DataDir/wave2_v2_on")
Invoke-Adb @("shell", "am", "force-stop", $Package)

Write-Host "FULL NFSU2 AUDIO PORT ACTIVE (default product path)."
Write-Host "Verify in the next debug run log (WC3_DEBUG_TAGS=AUDIO):"
Write-Host "  [wave2] open ... platform=Wc3AudioApi callback_thread=guest"
Write-Host "  AAudio opened fmt=0 ... rate=<device> guest=<guest>"
Write-Host "  [wave2] pull xruns=... consumed=... stale=..."
Write-Host "Then run: python tools\audio_pcm_audit.py --gate --sync-log <run.txt> --miles-state <run.txt> --pull-log <run.txt> <wave2_dump.pcm>"
Write-Host "Rollback any time with: powershell -File tools\enable_pull_audio.ps1 -Rollback -SkipBuild"
