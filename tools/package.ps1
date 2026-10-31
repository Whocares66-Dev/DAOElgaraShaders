<#
.SYNOPSIS
    Make the release zip the mod is installed from.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset release`, writes dist\DAOElgaraShaders-<version>.zip,
    laid out as the game's install folder, so it is extracted there (beside bin_ship) as
    DAOLib's own zip is. It holds the plugin's folder as the build made it, whole, and
    nothing else, with README.md, LICENSE.txt, SMAA-LICENSE.txt and VERSION.txt beside:

        elgarashaders.dll, elgarashaders.pdb   the plugin, and its symbols for a crash log
        materials.answers, materials.names     the materials' table's tool's
        override\                              the game's shaders rebuilt, and the metal maps
        every other file                        a file of data\ the build copied, at its place there

    The whole folder, not files by name: 0.4.0's zip named its folders one by one and left
    out clouds\, and Vigil's Keep's overcast day with it (#61). So a file the build puts in
    the folder is given out with nothing here changed, and a file the build did not make is
    refused, not given: a file of data\ is given where data\ still has it, and one it has not
    (a branch's, left in the build tree: daynight.ini) stops the zip until it is deleted;
    and each of data\'s folders of .ini files must be there whole. Never given: what is the
    player's or a developer's (graphics.chosen, outdoors.tune, the timers' and dumps' files)
    and the build's own bookkeeping (override\materials.made).

    No loader and no address files: those are DAOLib's zip, which a game has first.
    DAOTurnBased's tools\package.ps1, copied 2026-10-16, with this plugin's files.

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
# What the build makes in the folder from no file of data\ of the same name.
$products = 'elgarashaders.dll', 'elgarashaders.pdb', 'materials.answers', 'materials.names'
foreach ($product in $products) {
    if (-not (Test-Path (Join-Path $built $product))) { throw "No $product in $built; run .\tools\build.ps1 -Preset $Preset again" }
}
# What is never given out: the player's options, a developer's files (outdoors.tune's
# tuning; the frame and GPU timers' gpu-timing, level-shaders and shadow-cost; the shadow
# maps' dump's), the build's bookkeeping, and a debug link's leavings.
$notGiven = 'graphics.chosen', 'outdoors.tune', 'gpu-timing', 'level-shaders', 'shadow-cost', 'dump-shadow-maps',
'materials.made'
$notGivenKinds = '.ilk', '.exp', '.lib', '.log'

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

# The folder's files, each given or not: a file of data\ only where data\ has it.
$data = Join-Path $repo 'data'
$given = @()
$stale = @()
foreach ($file in Get-ChildItem $built -Recurse -File) {
    $relative = $file.FullName.Substring($built.Length + 1)
    if ($notGiven -contains $file.Name -or $notGivenKinds -contains $file.Extension) { continue }
    if ($products -contains $relative -or $relative.StartsWith('override\') -or $relative.StartsWith('night\')) {
        $given += $relative
        continue
    }
    if (Test-Path -PathType Leaf (Join-Path $data $relative)) { $given += $relative; continue }
    $stale += $relative
}
# The interiors' nights (tools\bake-night.ps1): every interior data\areas names has its
# probes and lightmaps, or it would be left as its makers lit it in the release.
foreach ($area in Get-ChildItem (Join-Path $data 'areas') -Filter '*.ini') {
    $text = Get-Content $area.FullName
    if (-not ($text -match '^\s*outside\s*=')) { continue }
    $level = ($text | Select-String -Pattern '^\s*level\s*=\s*(\w+)').Matches | Select-Object -First 1 |
        ForEach-Object { $_.Groups[1].Value }
    if ($level -and -not (Test-Path (Join-Path $built "night\$level.elgp"))) {
        throw "No night of $level ($($area.Name)) in $built\night; build with the 64-bit relighter (tools\build-bake.ps1)"
    }
}
if ($stale) {
    throw "In $built, made by no step of this build and no file of data\: $($stale -join ', '). Delete them (a branch's leavings) or add their source to data\ and the build"
}
# Each of data\'s folders the build copies is there whole: 0.4.0's zip had no clouds\.
foreach ($folder in Get-ChildItem $data -Directory) {
    $sources = @(Get-ChildItem $folder.FullName -File -Filter '*.ini')
    $copied = @($given | Where-Object { $_.StartsWith("$($folder.Name)\") })
    if ($sources.Count -ne $copied.Count) {
        throw "data\$($folder.Name) has $($sources.Count) .ini files and $built $($copied.Count); run .\tools\build.ps1 -Preset $Preset again"
    }
}

$out = Join-Path $repo 'dist'
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugin = Join-Path $stage 'bin_ship\daolib\plugins\elgarashaders'
foreach ($relative in $given) {
    $into = Join-Path $plugin $relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $into) | Out-Null
    Copy-Item -Force (Join-Path $built $relative) $into
}
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
$folders = ($given | Where-Object { $_.Contains('\') } | ForEach-Object { $_.Split('\')[0] } | Group-Object |
        ForEach-Object { "$($_.Name)\ $($_.Count)" }) -join ', '
Show-Info "built on $builtOn, plugin API $pluginApi; $($given.Count) files ($($shaders.Count) shaders, $($maps.Count) metal maps); $folders"
Show-Ok "package -> $zip"
