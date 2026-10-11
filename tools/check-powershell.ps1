<#
.SYNOPSIS
    Format or lint every PowerShell script in the repository with PSScriptAnalyzer, on its default rules.

.DESCRIPTION
    The build's format, format-check and tidy targets call this (cmake/Quality.cmake),
    and it runs the same by hand. The scripts are what git tracks, a new one from the
    moment it is added (git add): a script lying in the tree that is nobody's to commit
    is not this repository's to rewrite or to fail on. It took those too until
    2026-10-09, "so one in a new folder is not missed": a gate's -Format rewrote six
    untracked scripts of the user's in dev\, and its lint failed on them.

    -Mode Format       rewrite each script with Invoke-Formatter
    -Mode FormatCheck  fail if any script would be rewritten
    -Mode Lint         Invoke-ScriptAnalyzer; fail on any finding

    -Stamp names a file that holds the scripts' hashes as last linted clean. While
    they are unchanged, Lint returns before the module loads, which is most of its
    cost -- the same reason clang-tidy keeps a stamp per file.

.EXAMPLE
    .\tools\check-powershell.ps1 -Mode Lint
    .\tools\check-powershell.ps1 -Mode Format
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Format', 'FormatCheck', 'Lint')] [string] $Mode,
    [string] $Stamp
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot

$files = @(git -C $repo ls-files --cached -- '*.ps1' '*.psm1' '*.psd1' |
        ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path $_ })
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed; is this a git checkout?' }

if ($Mode -eq 'Lint') {
    $hashes = ($files | ForEach-Object { "$_ $((Get-FileHash $_).Hash)" }) -join "`n"
    if ($Stamp -and (Test-Path $Stamp) -and (Get-Content -Raw $Stamp) -ceq $hashes) { return }

    $findings = @($files | ForEach-Object { Invoke-ScriptAnalyzer -Path $_ })
    foreach ($f in $findings) {
        Show-Bad ('{0}:{1}: {2} [{3}]' -f $f.ScriptPath, $f.Line, $f.Message, $f.RuleName)
    }
    if ($findings.Count) { exit 1 }
    if ($Stamp) { Set-Content -NoNewline -Path $Stamp -Value $hashes }
    Show-Ok "PSScriptAnalyzer: $($files.Count) scripts, no findings"
    return
}

$unformatted = 0
foreach ($file in $files) {
    $text = [IO.File]::ReadAllText($file)
    $formatted = Invoke-Formatter -ScriptDefinition $text
    if ($formatted -ceq $text) { continue }
    $unformatted++
    if ($Mode -eq 'Format') {
        [IO.File]::WriteAllText($file, $formatted)
        Show-Info "formatted $file"
    }
    else {
        Show-Bad "would reformat $file"
    }
}
if ($Mode -eq 'FormatCheck' -and $unformatted) { exit 1 }
