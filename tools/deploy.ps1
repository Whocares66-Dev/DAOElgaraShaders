<#
.SYNOPSIS
    Copy a build's plugin, a folder of its own, into the game; or take it out.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset <preset>`, puts the plugin's folder,
    build\<preset>\daolib\plugins\elgarashaders\ (elgarashaders.dll, its .pdb, graphics.ini
    and the override folder of rebuilt shaders), under bin_ship\daolib\plugins. The game must
    not be running from that folder: it holds those files open. DAOLib's loader
    (version.dll) and its address files are DAOLib's to put there (its own deploy.ps1, or a
    release of it); other plugins in the folder are left as they are.

    A look chosen in play is kept in the installed folder's graphics.chosen, which a deploy
    leaves as it is.

    The levels lit again go with it: for each level of -Relit, the lightmaps, probes,
    reflection cubes and tints
    `.\tools\relight.ps1` left in build\relit\<level>\ are put in the plugin's override
    folder, where they are what the game lights that level by. A level goes into the game
    once it has been looked at in play, so the levels are named and none is taken unasked.

    -Remove deletes the plugin's folder. The light probes baked in play and the log are in
    Documents\BioWare\Dragon Age\daolib\ and stay. A folder that is a link, or has one
    anywhere in it (a junction, a symbolic link), is not deleted: a recursive delete goes
    through one and empties what it points at.

.EXAMPLE
    .\tools\deploy.ps1                 # debug build into the GOG install
    .\tools\deploy.ps1 -Preset release
    .\tools\deploy.ps1 -Preset release -Relit lak303d
    .\tools\deploy.ps1 -Remove
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'debug',
    [switch] $Remove,
    [string[]] $Relit = @(),
    [string] $Game = 'C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $Game 'DAOrigins.exe'))) { throw "No DAOrigins.exe in $Game" }
# The game holds its DLLs open, and only one running from this folder holds this folder's.
# Another has the same process name (another install; a peer's copy), so the running one is
# asked where it runs from. One whose path cannot be read is taken for this folder's.
$exe = (Resolve-Path (Join-Path $Game 'DAOrigins.exe')).Path
foreach ($running in @(Get-Process -Name DAOrigins -ErrorAction SilentlyContinue)) {
    $from = $running.Path
    if (-not $from -or [string]::Equals($from, $exe, [StringComparison]::OrdinalIgnoreCase)) {
        $where = if ($from) { $from } else { 'a path that cannot be read' }
        throw "DAOrigins.exe is running from $where (process $($running.Id)); close it first"
    }
    Show-Line "a game is running from $from (process $($running.Id)); it does not hold this folder's files"
}

$plugins = Join-Path $Game 'daolib\plugins'
$pluginFolder = Join-Path $plugins 'elgarashaders'

function Find-ReparsePoint([string] $Path) {
    # The folder itself or anything in it that is a link: a junction, a symbolic link.
    $top = Get-Item -LiteralPath $Path -Force
    if ($top.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $top.FullName }
    foreach ($item in Get-ChildItem -LiteralPath $Path -Recurse -Force -Attributes ReparsePoint) {
        return $item.FullName
    }
    return $null
}

function Remove-PluginFolder {
    # (ShouldProcess because PSScriptAnalyzer asks it of a function that removes; the
    # script has no -WhatIf, so the answer is always yes.)
    [CmdletBinding(SupportsShouldProcess)]
    param()
    if (-not (Test-Path $pluginFolder)) { return }
    $link = Find-ReparsePoint $pluginFolder
    if ($link) { throw "$link is a link; the plugin's folder is not deleted through one. Remove it by hand." }
    if ($PSCmdlet.ShouldProcess($pluginFolder, 'Remove the folder')) {
        Remove-Item -LiteralPath $pluginFolder -Recurse -Force
    }
}

if ($Remove) {
    Remove-PluginFolder
    Show-Line "removed $pluginFolder" -Colour Green
    return
}

$built = Join-Path $repo "build\$Preset\daolib\plugins\elgarashaders"
if (-not (Test-Path (Join-Path $built 'elgarashaders.dll'))) {
    throw "No elgarashaders.dll in $built; run .\tools\build.ps1 -Preset $Preset first"
}
if (-not (Test-Path (Join-Path $Game 'version.dll'))) {
    Show-Line "no version.dll beside DAOrigins.exe: DAOLib's loader is not installed, and nothing loads the plugin" -Colour Yellow
}
# The look chosen in play is the player's, kept across a deploy.
$chosen = Join-Path $pluginFolder 'graphics.chosen'
$kept = if (Test-Path $chosen) { Get-Content -LiteralPath $chosen -Raw } else { $null }
Remove-PluginFolder
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item -LiteralPath $built -Destination $pluginFolder -Recurse
if ($null -ne $kept) { Set-Content -LiteralPath $chosen -Value $kept -NoNewline }
foreach ($level in $Relit) {
    $lit = Join-Path $repo "build\relit\$level"
    $files = @(Get-ChildItem -LiteralPath $lit -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.dds', '.elgp', '.lamps', '.rml', '.mmh' })
    if ($files.Count -eq 0) { throw "Nothing of $level in $lit; run .\tools\relight.ps1 -Level $level first" }
    $files | Copy-Item -Destination (Join-Path $pluginFolder 'override')
    Show-Line "$level lit again: $($files.Count) files"
}
Show-Line "deployed $built to $pluginFolder" -Colour Green
