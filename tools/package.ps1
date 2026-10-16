<#
.SYNOPSIS
    Make the release zip the mod is installed from.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset release`, writes dist\DAOElgaraShaders-<version>.zip,
    laid out as the game's install folder, so it is extracted there (beside bin_ship) as
    DAOLib's own zip is. It holds the plugin's folder and nothing else:

        bin_ship\daolib\plugins\elgarashaders\elgarashaders.dll
        bin_ship\daolib\plugins\elgarashaders\elgarashaders.pdb   so a crash log names the mod's frames
        bin_ship\daolib\plugins\elgarashaders\materials.answers   what each of the game's materials is given
        bin_ship\daolib\plugins\elgarashaders\materials.names     the names' tables a mod's are classed by
        bin_ship\daolib\plugins\elgarashaders\override\           the game's shaders rebuilt, and the metal maps
        bin_ship\daolib\plugins\elgarashaders\README.md, LICENSE.txt, SMAA-LICENSE.txt, VERSION.txt

    No loader and no address files: those are DAOLib's zip, which a game has first. Not
    graphics.chosen: the options are the player's, kept across an update. DAOTurnBased's
    tools\package.ps1, copied 2026-10-16, with this plugin's files.

    The override folder is as the build made it (the shaders, SMAA's two lookup textures,
    the metal maps), and this refuses one with no shaders. A session's learned maps are in
    the player's profile, never there. VERSION.txt is what the README cannot say: the release,
    and the DAOLib it was built on, which is the tag extern\DAOLib is at, where a release
    holds it (dev/RELEASING.md).

    The version is the project's (CMakeLists.txt). Nothing is copied into the game.

.EXAMPLE
    .\tools\package.ps1                 # from build\release
    .\tools\package.ps1 -Preset debug   # a debug build's, for a trial
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'release'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
. (Join-Path $PSScriptRoot 'daolib_release.ps1')
$repo = Split-Path -Parent $PSScriptRoot

$built = Join-Path $repo "build\$Preset\daolib\plugins\elgarashaders"
$builtPlugin = Join-Path $built 'elgarashaders.dll'
if (-not (Test-Path $builtPlugin)) { throw "No $builtPlugin; run .\tools\build.ps1 -Preset $Preset first" }
$builtPdb = Join-Path $built 'elgarashaders.pdb'
if (-not (Test-Path $builtPdb)) { throw "No $builtPdb; a crash log names none of the mod's frames without it" }
$tables = 'materials.answers', 'materials.names'
foreach ($table in $tables) {
    if (-not (Test-Path (Join-Path $built $table))) { throw "No $table in $built; run .\tools\build.ps1 -Preset $Preset again" }
}

# The override folder: the game's shaders as build_shaders.py rebuilt them, and the
# metal maps the materials' table made (elgarashaders_materials objects).
$override = Join-Path $built 'override'
$overrideFiles = @(Get-ChildItem $override -File -ErrorAction SilentlyContinue)
$shaders = @($overrideFiles | Where-Object { $_.Extension -ne '.dds' })
$maps = @($overrideFiles | Where-Object { $_.Name -like '*_m.dds' })
if (-not $shaders) { throw "No shaders in $override; run .\tools\build.ps1 -Preset $Preset again" }

$cmake = Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')
if ($cmake -notmatch 'project\((\w+) VERSION (\d+\.\d+\.\d+)') { throw 'No version in CMakeLists.txt' }
$version = $Matches[2]
$name = "$($Matches[1])-$version"

# The loader and the address files a game has are a DAOLib release's, so VERSION.txt
# says which this was built on. At no tag it is a commit's, which a trial may be and a
# release is not.
$daolib = Join-Path $repo 'extern\DAOLib'
$daolibVersion = Get-DAOLibRelease $daolib
$pluginApi = Get-DAOLibPluginApi $daolib
if ($daolibVersion) {
    $builtOn = "DAOLib $daolibVersion"
}
else {
    $builtOn = "DAOLib at its commit $(git -C $daolib rev-parse --short HEAD), between releases"
    Show-Warn 'extern\DAOLib is at no release''s tag; a release of the mod holds it at one'
}

$out = Join-Path $repo 'dist'
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugin = Join-Path $stage 'bin_ship\daolib\plugins\elgarashaders'
New-Item -ItemType Directory -Force (Join-Path $plugin 'override') | Out-Null

# By name, not the folder as built: graphics.chosen is a player's, and a debug build
# leaves the linker's .ilk there.
Copy-Item -Force $builtPlugin, $builtPdb $plugin
foreach ($table in $tables) { Copy-Item -Force (Join-Path $built $table) $plugin }
foreach ($file in $overrideFiles) { Copy-Item -Force $file.FullName (Join-Path $plugin 'override') }
Copy-Item -Force (Join-Path $repo 'README.md') $plugin
Copy-Item -Force (Join-Path $repo 'LICENSE') (Join-Path $plugin 'LICENSE.txt')
Copy-Item -Force (Join-Path $repo 'shaders\smaa\LICENSE.txt') (Join-Path $plugin 'SMAA-LICENSE.txt')

$versionText = @"
DAOElgaraShaders $version
https://github.com/Whocares66-Dev/DAOElgaraShaders/releases
Guide: https://whocares66-dev.github.io/DAOElgaraShaders/

Built on $builtOn, plugin API $pluginApi
(https://github.com/Whocares66-Dev/DAOLib/releases). The game needs that release of
DAOLib or a later one of the same plugin API: its loader, and its address files. A loader
of another plugin API does not load this plugin, and the logs in
Documents\BioWare\Dragon Age\daolib (loader.log, elgarashaders.log) say so.
"@
Set-Content -Path (Join-Path $plugin 'VERSION.txt') -Value $versionText -Encoding utf8

# What is given out does not say who built it. The linker writes the folder of its
# temporary files into a PDB, which tools\build.ps1 keeps in the build tree for that
# reason (as DAOLib #55); a plugin linked any other way is caught here, and so is any
# other file of the zip that names this user's folder or this user.
$profileFolder = [Environment]::GetFolderPath('UserProfile')
$userName = Split-Path -Leaf $profileFolder
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $bytes = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($file.FullName))
    foreach ($word in $profileFolder, "Users\$userName", "Users/$userName") {
        foreach ($written in $word, ($word.ToCharArray() -join "`0")) {
            if ($bytes.IndexOf($written, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                Remove-Item -Recurse -Force $stage
                throw "$($file.Name) holds the path of this user's folder; build with .\tools\build.ps1 -Preset $Preset and nothing else"
            }
        }
    }
}

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Show-Info "built on $builtOn, plugin API $pluginApi; $($shaders.Count) shaders and $($maps.Count) maps in override"
Show-Ok "package -> $zip"
