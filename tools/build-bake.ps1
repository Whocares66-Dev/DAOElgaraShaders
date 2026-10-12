<#
.SYNOPSIS
    Build the relighter as a 64-bit program with Embree, into build\bake64.

.DESCRIPTION
    The plugin's own build (tools\build.ps1) is 32-bit, as the game is, and makes a 32-bit
    elgarashaders_relight with it. This builds the same tool for 64 bits from bake\, with
    Embree to cast its rays (issue 5): build\bake64\elgarashaders_relight.exe, which
    tools\relight.ps1 uses where it is there. The first build makes Embree, which takes
    some minutes.

.EXAMPLE
    .\tools\build-bake.ps1
#>
[CmdletBinding()]
param(
    [switch] $Fresh
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot

# The toolset as tools\build.ps1 finds it, for 64 bits.
$vcvars = $null
$roots = @()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) { $roots += & $vswhere -all -prerelease -products * -property installationPath }
foreach ($root in ${env:ProgramFiles}, ${env:ProgramFiles(x86)}) {
    foreach ($edition in 'Enterprise', 'Professional', 'Community', 'BuildTools') {
        $roots += (Join-Path $root "Microsoft Visual Studio\2022\$edition")
    }
}
foreach ($root in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
    $candidate = Join-Path $root 'VC\Auxiliary\Build\vcvarsall.bat'
    if (Test-Path $candidate) { $vcvars = $candidate; break }
}
if (-not $vcvars) { throw 'No Visual Studio 2022 C++ toolset found (no vcvarsall.bat).' }
$userVcpkgRoot = $env:VCPKG_ROOT
$output = & ${env:ComSpec} /s /c "`"$vcvars`" amd64 >nul 2>&1 && set"
foreach ($line in $output) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}
if ($userVcpkgRoot) { $env:VCPKG_ROOT = $userVcpkgRoot }
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set.' }

$buildDir = Join-Path $repo 'build\bake64'
if ($Fresh -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
(Get-Process -Id $PID).PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal

Show-Line "`n== configure (bake64) ==" -Colour Cyan
cmake -S (Join-Path $repo 'bake') -B $buildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl.exe `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

Show-Line "`n== build (bake64) ==" -Colour Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Show-Line "`nOK: $buildDir\elgarashaders_relight.exe" -Colour Green
