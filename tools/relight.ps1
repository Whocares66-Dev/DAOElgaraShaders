<#
.SYNOPSIS
    Light again the levels data\relight.ini names, into build\relit\<level>\.

.DESCRIPTION
    Runs the build's elgarashaders_relight for each level that has a section in
    data\relight.ini (or for those named), with that file's values. The lightmaps are
    written to build\relit\<level>\, a DDS each of its atlas's own name, with the level's
    probes, and with what tools\tint_level.py makes for the level's param, model and
    texture lines (rooms, copies of models and textures recoloured); `tools\deploy.ps1 -Relit <level>` puts them in the plugin's override folder, where
    they are what the game lights the level by. A level takes half a minute to a minute.

.EXAMPLE
    .\tools\relight.ps1 -Preset release
    .\tools\relight.ps1 -Preset release -Level lak303d
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'release',
    [string[]] $Level = @(),
    [string] $Game = 'C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot
# The 64-bit tool with Embree where it has been built (tools\build-bake.ps1), else the
# 32-bit one the plugin's build makes.
$tool = Join-Path $repo 'build\bake64\elgarashaders_relight.exe'
if (-not (Test-Path $tool)) { $tool = Join-Path $repo "build\$Preset\elgarashaders_relight.exe" }
$file = Join-Path $repo 'data\relight.ini'
if (-not (Test-Path $tool)) { throw "No $tool; run .\tools\build.ps1 -Preset $Preset first" }

if ($Level.Count -eq 0) {
    $Level = @(Select-String -LiteralPath $file -Pattern '^\[(.+)\]\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } |
            Where-Object { $_ -ne 'default' })
}
foreach ($one in $Level) {
    $out = Join-Path $repo "build\relit\$one"
    # Into a folder beside it first: a level half written is no level.
    $new = "$out.new"
    if (Test-Path -LiteralPath $new) { Get-ChildItem -LiteralPath $new -File | Remove-Item -Confirm:$false }
    & $tool $Game $one $new $file 2>&1 | ForEach-Object { Show-Line ("$_" -replace '^\[[^\]]*\] \[[^\]]*\] ', '') }
    if ($LASTEXITCODE -ne 0) { throw "$one was not lit again ($LASTEXITCODE)" }
    # The level's tints, which are tint_level.py's: its section's param, model and texture lines.
    $tints = @()
    $section = ''
    foreach ($line in Get-Content -LiteralPath $file) {
        $line = ($line -replace ';.*$', '').Trim()
        if ($line -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
        if ($section -eq $one -and $line -match '^(param|model|texture)\s*=\s*(.+)$') { $tints += "$($Matches[1])=$($Matches[2])" }
    }
    if ($tints.Count -gt 0) {
        & python (Join-Path $PSScriptRoot 'tint_level.py') $Game $one $new @tints 2>&1 | ForEach-Object { Show-Line "$_" }
        if ($LASTEXITCODE -ne 0) { throw "$one was not tinted ($LASTEXITCODE)" }
    }
    # No lightmap of the level may be left the game's own: it would be read as ours are written.
    & python (Join-Path $PSScriptRoot 'check_relit.py') $Game $one $new 2>&1 | ForEach-Object { Show-Line "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$one is not lit again whole; nothing of it was taken (the files are in $new)" }
    New-Item -ItemType Directory -Force $out | Out-Null
    Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.dds', '.elgp', '.rml', '.mmh' } | Remove-Item -Confirm:$false
    Get-ChildItem -LiteralPath $new -File | Move-Item -Destination $out
    Remove-Item -LiteralPath $new -Confirm:$false
    Show-Line "$one lit again: $out" -Colour Green
}
