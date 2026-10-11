<#
.SYNOPSIS
    Build DAOElgaraShaders from any shell -- no "x86 Native Tools Command Prompt" needed.

.DESCRIPTION
    DAOLib's build script, for this repository: it finds Visual Studio, imports the
    32-bit target environment (vcvarsall.bat amd64_x86) into this session, and runs
    CMake with a preset. DAOLib comes from the folder DAOLIB_DIR names (the
    environment's, or CMake's), else extern\DAOLib (a submodule; run
    `git submodule update --init --recursive` after cloning), else ..\DAOLib.

.PARAMETER Preset
    debug       elgarashaders.dll, its shaders, the baker's command and the tests;
                build\debug\daolib\plugins\elgarashaders is then the plugin's folder,
                for tools\deploy.ps1. The first run compiles spdlog and Detours
                through vcpkg.
    release     same, optimized, with its symbols: what the game plays.
    debug-analyze
                same, our own targets under MSVC's static analyser (/analyze).
                Its own tree, so the debug tree's compile database, which
                clang-tidy reads, never holds /analyze. Not for -Lint.

.PARAMETER Test
    After building, run the tests (ctest): the Python tools' own.

.PARAMETER Format
    After building, rewrite every source in place with its formatter: the
    `format` target (DAOLib's cmake/Quality.cmake, over this tree).

.PARAMETER Lint
    After building (and testing, with -Test), run the linters: clang-tidy over
    every C++ file and PSScriptAnalyzer over the PowerShell. The `tidy` target;
    a finding fails the run.

.PARAMETER Fresh
    Remove the preset's build tree first.

.PARAMETER Jobs
    How many compilations to run at once. The default leaves two of the
    machine's cores to everything else.

.EXAMPLE
    .\tools\build.ps1 -Preset debug -Test
    .\tools\build.ps1 -Preset debug -Test -Format -Lint
    .\tools\build.ps1 -Preset release
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'debug-analyze')]
    [string] $Preset = 'debug',
    [switch] $Test,
    [switch] $Format,
    [switch] $Lint,
    [switch] $Fresh,
    [int] $Jobs = 0
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
if ($Lint -and $Preset -eq 'debug-analyze') {
    throw 'clang-tidy cannot read a compile database holding /analyze; lint with -Preset debug.'
}
$hostTarget = 'amd64_x86'
$repo = Split-Path -Parent $PSScriptRoot

function Get-VcVarsAllPath {
    # Do NOT locate the toolset with `vswhere -latest -requires ...`: a VS install
    # flagged isComplete=False is invisible to it while cl.exe sits on disk. Probe
    # the filesystem and let the presence of vcvarsall.bat be the answer.
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -all -prerelease -products * -property installationPath
    }
    foreach ($root in ${env:ProgramFiles}, ${env:ProgramFiles(x86)}) {
        foreach ($ed in 'Enterprise', 'Professional', 'Community', 'BuildTools') {
            $candidates += (Join-Path $root "Microsoft Visual Studio\2022\$ed")
        }
    }
    foreach ($c in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $vcvars = Join-Path $c 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $vcvars) { return $vcvars }
    }
    return $null
}

function Import-DeveloperEnvironment {
    $vcvars = Get-VcVarsAllPath
    if (-not $vcvars) {
        throw @'
No Visual Studio 2022 C++ toolset found (no vcvarsall.bat anywhere).

Install the C++ workload, with Visual Studio CLOSED:

  & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
      --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" `
      --add Microsoft.VisualStudio.Workload.NativeDesktop `
      --includeRecommended --quiet --norestart
'@
    }
    Show-Line "Importing developer environment from $vcvars ($hostTarget)" -Colour DarkGray
    # `set` after vcvarsall dumps the whole environment; replay it into this session.
    $output = & ${env:ComSpec} /s /c "`"$vcvars`" $hostTarget >nul 2>&1 && set"
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
}

# vcvarsall sets VCPKG_ROOT to the vcpkg bundled inside Visual Studio, which is
# NOT the one this project uses. Preserve whatever was set beforehand.
$userVcpkgRoot = $env:VCPKG_ROOT
Import-DeveloperEnvironment
if ($userVcpkgRoot) {
    if ($env:VCPKG_ROOT -ne $userVcpkgRoot) {
        Show-Line "  restoring VCPKG_ROOT=$userVcpkgRoot (vcvarsall had reset it to $env:VCPKG_ROOT)" -Colour DarkGray
    }
    $env:VCPKG_ROOT = $userVcpkgRoot
}

# LLVM's own release first on PATH, not the older one Visual Studio bundles.
$llvmBin = if ($env:LLVM_ROOT) { Join-Path $env:LLVM_ROOT 'bin' } else { Join-Path $env:ProgramFiles 'LLVM\bin' }
if (Test-Path (Join-Path $llvmBin 'clang-tidy.exe')) {
    $env:PATH = "$llvmBin;$env:PATH"
}

foreach ($tool in 'cl', 'cmake', 'ninja') {
    $found = Get-Command $tool -ErrorAction SilentlyContinue
    if (-not $found) { throw "'$tool' is still not on PATH after importing vcvarsall." }
    Show-Line ("  {0,-6} {1}" -f $tool, $found.Source) -Colour DarkGray
}
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set; every preset needs vcpkg.' }
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    throw "vcpkg.exe missing. Run: & '$env:VCPKG_ROOT\bootstrap-vcpkg.bat'"
}

$buildDir = Join-Path $repo "build\$Preset"
if ($Fresh -and (Test-Path $buildDir)) {
    Show-Line "Removing $buildDir" -Colour Yellow
    Remove-Item -Recurse -Force $buildDir
}

$cores = [Environment]::ProcessorCount
$parallel = if ($Jobs -gt 0) { $Jobs } else { [Math]::Max(1, $cores - 2) }
$env:CMAKE_BUILD_PARALLEL_LEVEL = $parallel
(Get-Process -Id $PID).PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
Show-Line "  jobs   $parallel of $cores cores, below normal priority"

Push-Location $repo
try {
    Show-Line "`n== configure ($Preset) ==" -Colour Cyan
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

    Show-Line "`n== build ($Preset) ==" -Colour Cyan
    # The linker writes the path of its temporary files into a PDB, and the
    # user's own is under their name (C:\Users\<name>\AppData\Local\Temp);
    # a release's PDB is given to whoever installs it (as DAOLib #55). So a
    # build's is in its tree, for the build alone.
    $userTemp = $env:TEMP, $env:TMP
    $env:TEMP = $env:TMP = New-Item -ItemType Directory -Force (Join-Path $buildDir 'tmp')
    try { cmake --build --preset $Preset --parallel $parallel }
    finally { $env:TEMP, $env:TMP = $userTemp }
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    if ($Test) {
        Show-Line "`n== test ($Preset) ==" -Colour Cyan
        ctest --preset $Preset --parallel $parallel
        if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
    }

    if ($Format) {
        Show-Line "`n== format ==" -Colour Cyan
        cmake --build --preset $Preset --target format
        if ($LASTEXITCODE -ne 0) { throw "format failed ($LASTEXITCODE)" }
    }

    if ($Lint) {
        Show-Line "`n== lint ($Preset) ==" -Colour Cyan
        cmake --build --preset $Preset --parallel $parallel --target tidy
        if ($LASTEXITCODE -ne 0) { throw "lint failed ($LASTEXITCODE)" }
    }
    Show-Line "`nOK" -Colour Green
}
finally {
    Pop-Location
}
