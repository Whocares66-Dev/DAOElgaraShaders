# Which release of DAOLib the submodule is at, dot-sourced by the scripts that say so
# or insist on one (package.ps1, release.ps1), as DAOLib's address_release.ps1 is by its:
#   . (Join-Path $PSScriptRoot 'daolib_release.ps1')
# DAOLib has releases of its own, a tag v<version> each, and its zip is the loader and
# the address files a game has. A release of the mod holds the submodule at one, so that
# the plugin asks nothing of a game that the DAOLib release lacks (dev/RELEASING.md);
# between releases it is at whatever commit the work needs.

# The version of the release the submodule at $Path is checked out at, or nothing when
# its commit is no release's.
function Get-DAOLibRelease([string] $Path) {
    $tag = @(git -C $Path tag --points-at HEAD --list 'v*') | Select-Object -First 1
    if ($tag) { $tag -replace '^v' }
}

# The plugin API the DAOLib at $Path is of: a loader loads the plugins of its own and
# refuses the rest (src/dao/Plugin.h).
function Get-DAOLibPluginApi([string] $Path) {
    $header = Get-Content -Raw (Join-Path $Path 'src\dao\Plugin.h')
    if ($header -notmatch 'kPluginApiVersion\s*=\s*(\d+)\s*;') { throw "No kPluginApiVersion in $Path\src\dao\Plugin.h" }
    $Matches[1]
}
