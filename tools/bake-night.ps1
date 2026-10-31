<#
.SYNOPSIS
    Bake the interiors' night lightmaps and probes into a build's plugin folder.

.DESCRIPTION
    Each interior the areas' files name (data\areas: a file with `outside` and `level`), and each
    level with a section of its own in data\relight_night.ini, is lit by its own lamps and candles
    alone with the 64-bit relighter (build\bake64\elgarashaders_relight.exe, tools\build-bake.ps1),
    at the size of the game's own lightmaps, and its lightmaps are packed as DXT1
    (elgarashaders_ddspack, the build's) into build\<preset>\daolib\plugins\elgarashaders\night,
    with its probes (<level>.elgp). Nothing of it is kept in the repository: the recipe is,
    and the bake is made from the installed game as the shaders are (the user, 2026-10-30:
    "configs should be part of the repo. The generated stuff should not").

    A level is baked again only where the recipe or either tool is newer than what was made.
    Without the 64-bit relighter it says so and bakes nothing: the plugin then has no night
    lightmaps, and interiors keep the makers' night.

.PARAMETER Preset
    The build whose plugin folder the bake goes into.

.PARAMETER Game
    The game's folder; DAO_GAME_DIR's, or GOG's install.

.PARAMETER Fresh
    Bake every level again.
#>
param(
    [string] $Preset = 'release',
    [string] $Game = $(if ($env:DAO_GAME_DIR) { $env:DAO_GAME_DIR } else { 'C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins' }),
    [switch] $Fresh
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$recipe = Join-Path $repo 'data\relight_night.ini'
$relight = Join-Path $repo 'build\bake64\elgarashaders_relight.exe'
$pack = Join-Path $repo "build\$Preset\elgarashaders_ddspack.exe"
$out = Join-Path $repo "build\$Preset\daolib\plugins\elgarashaders\night"
$work = Join-Path $repo "build\$Preset\night-bake"

if (-not (Test-Path $relight)) {
    Show-Line "bake-night: no $relight (tools\build-bake.ps1 makes it); no night lightmaps baked" -Colour Yellow
    return
}
if (-not (Test-Path $pack)) { throw "bake-night: no $pack; build the preset first" }
if (-not (Test-Path (Join-Path $Game 'packages\core\env'))) { throw "bake-night: no game at $Game" }

# The interiors' levels, from the areas' files (an interior's names its `outside`), and any the
# recipe has a section of its own for: adding an interior is its area's file alone.
$interiors = Get-ChildItem (Join-Path $repo 'data\areas') -Filter '*.ini' | ForEach-Object {
    $text = Get-Content $_.FullName
    if ($text -match '^\s*outside\s*=') {
        ($text | Select-String -Pattern '^\s*level\s*=\s*(\w+)').Matches | Select-Object -First 1 |
            ForEach-Object { $_.Groups[1].Value }
        }
    }
    $sections = Select-String -Path $recipe -Pattern '^\[(\w+)\]\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Where-Object { $_ -ne 'default' }
$levels = @($interiors) + @($sections) | Where-Object { $_ } | Sort-Object -Unique
$inputs = @($recipe, $relight, $pack) | ForEach-Object { (Get-Item $_).LastWriteTimeUtc }
$newest = ($inputs | Measure-Object -Maximum).Maximum
New-Item -ItemType Directory -Force $out | Out-Null

foreach ($level in $levels) {
    $probes = Join-Path $out "$level.elgp"
    if (-not $Fresh -and (Test-Path $probes) -and (Get-Item $probes).LastWriteTimeUtc -gt $newest) {
        continue
    }
    $raw = Join-Path $work $level
    if (Test-Path $raw) { Remove-Item -Recurse -Force $raw }
    New-Item -ItemType Directory -Force $raw | Out-Null
    # The game's own lightmaps are 1024 a side at the most: none is made smaller than the
    # makers' (side=256 halved a 1024 atlas twice, and a floor's piece in it, beside one of a
    # 256 atlas kept whole, caught a candle's light in a strip its neighbour did not:
    # Witch Hunt's library, the user, 2026-10-30).
    $started = Get-Date
    & $relight $Game $level $raw $recipe 'side=1024' | Out-File (Join-Path $work "$level.log")
    if ($LASTEXITCODE -ne 0) { throw "bake-night: $level failed ($LASTEXITCODE); $work\$level.log" }
    Get-ChildItem $out -Filter "$($level)_room*.dds" | Remove-Item
    foreach ($map in Get-ChildItem $raw -Filter "$($level)_room*.dds") {
        & $pack $map.FullName (Join-Path $out $map.Name) 'dxt1'
        if ($LASTEXITCODE -ne 0) { throw "bake-night: $($map.Name) could not be packed" }
    }
    Copy-Item (Join-Path $raw "$level.elgp") $probes
    $made = Get-ChildItem $out -Filter "$($level)*" | Measure-Object -Property Length -Sum
    Show-Line ("bake-night: {0}, {1} files, {2:N1} MB, {3:N0} s" -f $level, $made.Count, ($made.Sum / 1MB),
        ((Get-Date) - $started).TotalSeconds)
}
