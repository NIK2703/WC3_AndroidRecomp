# Persistent warning collector for canonical builds.
#
# Dot-source this file from a build script, call
# Initialize-Wc3BuildWarningLog once, feed captured command output to
# Add-Wc3BuildWarnings, and call Complete-Wc3BuildWarningLog from the
# outermost build script.  The log is append-only: an incremental build does
# not erase warnings from an earlier build.  Each warning carries the build
# id, phase, source location, warning code and a short context block.  A JSONL
# companion is written for later filtering.

$script:Wc3BuildWarningUtf8 = New-Object System.Text.UTF8Encoding($false)

function Write-Wc3BuildWarningText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    [IO.File]::AppendAllText($Path, $Text + [Environment]::NewLine,
        $script:Wc3BuildWarningUtf8)
}

function Initialize-Wc3BuildWarningLog {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Kind,
        [string]$Variant = ''
    )
    if ($global:Wc3BuildWarningState -and $global:Wc3BuildWarningState.Active) {
        return $global:Wc3BuildWarningState
    }
    $dir = Join-Path $Root 'build\logs'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $warningPath = Join-Path $dir 'build_warnings.log'
    $jsonlPath = Join-Path $dir 'build_warnings.jsonl'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $safeKind = ($Kind -replace '[^A-Za-z0-9_.-]', '_')
    $safeVariant = ($Variant -replace '[^A-Za-z0-9_.-]', '_')
    $buildId = "$stamp-$safeKind-$safeVariant-$PID"
    $state = [pscustomobject]@{
        Active = $true
        Root = $Root
        Kind = $Kind
        Variant = $Variant
        BuildId = $buildId
        StartedUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        WarningPath = $warningPath
        JsonlPath = $jsonlPath
        Count = 0
    }
    if (-not (Test-Path -LiteralPath $warningPath)) {
        $header = @'
# WC3 build warnings
# Appended by tools/build_warnings.ps1.  Each build starts with a `=== build ...` line.
# Only warnings emitted by this build are appended; an incremental build does not
# re-emit warnings from translation units that were already up to date.
'@
        Write-Wc3BuildWarningText -Path $warningPath -Text $header
    }
    $buildHeader = "`n=== build {0} started={1} kind={2} variant={3} host={4} ===" -f `
        $buildId, $state.StartedUtc, $Kind, $Variant, $env:COMPUTERNAME
    Write-Wc3BuildWarningText -Path $warningPath -Text $buildHeader
    $global:Wc3BuildWarningState = $state
    return $state
}

function Test-Wc3WarningLine {
    param([string]$Line)
    if (-not $Line) { return $false }
    $t = $Line.Trim()
    if (-not $t) { return $false }
    if ($t -match '^\d+\s+warnings?\s+generated\.?$') { return $false }
    if ($t -match '^warning:\s*$') { return $false }
    if ($t -match '(?i)^CMake Warning') { return $true }
    if ($t -match '(?i)^WARNING:') { return $true }
    if ($t -match '(?i)\bWarning:\s*$') { return $true }
    if ($t -match '(?i)^warning:') { return $true }
    if ($t -match '(?i)^w:\s+') { return $true }
    if ($t -match '(?i)^warn:\s+') { return $true }
    if ($t -match '(?i)^.+?:\d+(?::\d+)?:\s*warning:') { return $true }
    if ($t -match '(?i)^(ld|ld\.lld|clang|clang\+\+|gcc|g\+\+|cc1|cc1plus|link|ar|llvm-ar|llvm-ld|ninja):\s*warning:') { return $true }
    if ($t -match '(?i)^.*\bwarning\s+[A-Z]+\d+:') { return $true }
    return $false
}

function Get-Wc3WarningRecord {
    param(
        [string]$Line,
        [string]$Phase,
        $State,
        $Context
    )
    $t = $Line.Trim()
    $file = $null
    $lineNo = $null
    $colNo = $null
    $code = $null
    $message = $t
    if ($t -match '^(?<file>.+?):(?<line>\d+)(?::(?<col>\d+(?:-\d+)?))?:\s*[Ww]arning:\s*(?<msg>.*)$') {
        $file = $Matches.file
        $lineNo = [int]$Matches.line
        $colNo = $Matches.col
        $message = $Matches.msg.Trim()
    } elseif ($t -match '^(?i:CMake Warning)(?: \([^)]*\))?\s+at\s+(?<file>.+?):(?<line>\d+).*$') {
        $file = $Matches.file
        $lineNo = [int]$Matches.line
        $message = $t
    } elseif ($t -match '^(?<file>.+?):(?<line>\d+)(?::(?<col>\d+(?:-\d+)?))?\s+[Ww]arning:\s*$') {
        $file = $Matches.file
        $lineNo = [int]$Matches.line
        $colNo = $Matches.col
        $message = $t
    } elseif ($t -match '^(?i:WARNING):\s*(?<msg>.*)$') {
        $message = $Matches.msg.Trim()
    } elseif ($t -match '^w:\s+(?<file>file:///.+?):(?<line>\d+):(?<col>\d+)\s+(?<msg>.*)$') {
        $file = $Matches.file
        $lineNo = [int]$Matches.line
        $colNo = $Matches.col
        $message = $Matches.msg.Trim()
    }
    if ($message -match '\[(?<code>-W[^\]]+)\]') {
        $code = $Matches.code
    }
    [ordered]@{
        build_id = $State.BuildId
        timestamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        kind = $State.Kind
        variant = $State.Variant
        phase = $Phase
        file = $file
        line = $lineNo
        column = $colNo
        code = $code
        message = $message
        context = @($Context)
    }
}

function Add-Wc3BuildWarnings {
    param(
        [AllowNull()][object]$Lines,
        [Parameter(Mandatory = $true)][string]$Phase
    )
    $state = $global:Wc3BuildWarningState
    if (-not $state -or -not $state.Active) { return }
    $items = @($Lines)
    if ($items.Count -eq 0) { return }
    for ($i = 0; $i -lt $items.Count; $i++) {
        $raw = $items[$i]
        $line = if ($raw -is [System.Management.Automation.ErrorRecord]) {
            $raw.ToString()
        } else {
            [string]$raw
        }
        if (-not (Test-Wc3WarningLine -Line $line)) { continue }
        $context = New-Object System.Collections.Generic.List[string]
        $context.Add($line.TrimEnd())
        for ($j = $i + 1; $j -lt $items.Count -and $j -le $i + 5; $j++) {
            $nextRaw = $items[$j]
            $next = if ($nextRaw -is [System.Management.Automation.ErrorRecord]) {
                $nextRaw.ToString()
            } else {
                [string]$nextRaw
            }
            $nextTrim = $next.Trim()
            if (-not $nextTrim) { break }
            if (Test-Wc3WarningLine -Line $next) { break }
            if ($nextTrim -match '^\d+\s+warnings?\s+generated\.?$') { break }
            if ($nextTrim -match '^\[\d+/\d+\]') { break }
            $context.Add($next.TrimEnd())
        }
        $record = Get-Wc3WarningRecord -Line $line -Phase $Phase -State $state -Context $context
        $block = New-Object System.Text.StringBuilder
        [void]$block.AppendFormat("[{0}] {1} | {2}", $state.BuildId, $Phase, $line.TrimEnd())
        [void]$block.AppendLine()
        for ($k = 1; $k -lt $context.Count; $k++) {
            [void]$block.Append('    ')
            [void]$block.AppendLine($context[$k])
        }
        Write-Wc3BuildWarningText -Path $state.WarningPath -Text $block.ToString()
        $json = $record | ConvertTo-Json -Compress -Depth 4
        [IO.File]::AppendAllText($state.JsonlPath,
            $json + [Environment]::NewLine, $script:Wc3BuildWarningUtf8)
        $state.Count = $state.Count + 1
    }
}

function Complete-Wc3BuildWarningLog {
    param(
        [ValidateSet('ok', 'failed')][string]$Status = 'ok',
        [int]$ExitCode = 0
    )
    $state = $global:Wc3BuildWarningState
    if (-not $state -or -not $state.Active) { return }
    $finished = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $footer = "=== build {0} finished={1} status={2} exit={3} warnings={4} ===" -f `
        $state.BuildId, $finished, $Status, $ExitCode, $state.Count
    Write-Wc3BuildWarningText -Path $state.WarningPath -Text $footer
    Write-Host ("[warnings] {0} warning(s) appended to {1}" -f $state.Count, $state.WarningPath)
    $state.Active = $false
}
