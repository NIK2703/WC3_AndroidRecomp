# BP-12: shared ccache discovery for the PowerShell build entry points.
#
# Direct compiler invocations in tools/lift and tools/android go through this
# helper so every build path uses the same cache and the same forward-slash
# path convention required by the MSYS2 ccache binary.

function Get-Wc3Ccache {
    [CmdletBinding()]
    param(
        [string]$Explicit = ''
    )

    $candidates = @()
    if ($Explicit) { $candidates += $Explicit }
    if ($env:CCACHE) { $candidates += $env:CCACHE }

    $command = Get-Command ccache -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }

    $candidates += @(
        'C:\msys64\usr\bin\ccache.exe',
        'C:\msys64\ucrt64\bin\ccache.exe',
        'C:\msys64\mingw64\bin\ccache.exe',
        'C:\msys64\clang64\bin\ccache.exe',
        'C:\ProgramData\chocolatey\bin\ccache.exe',
        'C:\Program Files\ccache\ccache.exe'
    )

    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path.Replace('\', '/')
        }
    }
    return $null
}
