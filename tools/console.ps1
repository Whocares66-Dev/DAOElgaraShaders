# What the scripts under tools\ print, dot-sourced by each:
#   . (Join-Path $PSScriptRoot 'console.ps1')
# Their printed lines are their user interface -- progress, colours, the next
# command to paste -- and PSScriptAnalyzer's rule against Write-Host allows it
# inside a function whose verb is Show, which is where every call lives.

function Show-Line([string]$Text = '', [string]$Colour) {
    if ($Colour) { Write-Host $Text -ForegroundColor $Colour } else { Write-Host $Text }
}
function Show-Head([string]$Text) { Show-Line; Show-Line $Text -Colour Cyan; Show-Line ('-' * $Text.Length) -Colour DarkGray }
function Show-Ok([string]$Text) { Show-Line "  OK    $Text" -Colour Green }
function Show-Warn([string]$Text) { Show-Line "  WARN  $Text" -Colour Yellow }
function Show-Bad([string]$Text) { Show-Line "  FAIL  $Text" -Colour Red }
function Show-Info([string]$Text) { Show-Line "        $Text" -Colour Gray }
