<#
.SYNOPSIS
    Build the relighter as a 64-bit program with Embree, into build\bake64.

.DESCRIPTION
    The plugin's own build (tools\build.ps1) is 32-bit, as the game is, and makes a 32-bit
    elgarashaders_relight with it. This builds the same tool for 64 bits from bake\, with
    Embree to cast its rays (issue 5): build\bake64\elgarashaders_relight.exe, which
    tools\relight.ps1 uses where it is there. The first build makes Embree, which takes
    some minutes.

.PARAMETER Fresh
    Remove the build tree first.

.PARAMETER Lint
    After building, run clang-tidy over the tool's three sources (src\RelightTool.cpp,
    Relighter.cpp, ProbeBaker.cpp) with this build's compile database and the
    repository's .clang-tidy; a finding fails the run. The plugin's own lint
    (tools\build.ps1 -Lint) reads the 32-bit build, where the Embree half of
    Relighter.cpp is not compiled: it is linted here or nowhere.

.PARAMETER Sanitize
    Build with MSVC's AddressSanitizer (/fsanitize=address), with symbols, into a tree
    of its own, build\bake64-asan, and put the sanitizer's runtime beside the tool. A
    bake run with that tool stops at the first bad read or write and says where:

        build\bake64-asan\elgarashaders_relight.exe <game folder> lak303d <out folder> data\relight.ini side=64

    It is several times slower; a small side is enough to go through every step.
    Not for -Lint, whose compile database is the plain build's.

.EXAMPLE
    .\tools\build-bake.ps1
    .\tools\build-bake.ps1 -Lint
    .\tools\build-bake.ps1 -Sanitize
#>
[CmdletBinding()]
param(
    [switch] $Fresh,
    [switch] $Lint,
    [switch] $Sanitize
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lint -and $Sanitize) {
    throw 'Lint the plain build: -Lint without -Sanitize.'
}

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

$name = if ($Sanitize) { 'bake64-asan' } else { 'bake64' }
$buildDir = Join-Path $repo "build\$name"
if ($Fresh -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
(Get-Process -Id $PID).PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal

# The sanitizer's build is optimised as the tool is, with symbols, so that what it
# reports has names and lines.
$type = if ($Sanitize) { 'RelWithDebInfo' } else { 'Release' }
$asan = if ($Sanitize) { 'ON' } else { 'OFF' }
Show-Line "`n== configure ($name) ==" -Colour Cyan
# (The compiler's name in quotes: bare, PowerShell hands cmake `cl` and `.exe` as two
# words, and cmake says it is ignoring an extra path.)
cmake -S (Join-Path $repo 'bake') -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$type" '-DCMAKE_CXX_COMPILER=cl.exe' `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows "-DELGARA_SANITIZE=$asan"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

Show-Line "`n== build ($name) ==" -Colour Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

if ($Sanitize) {
    # The tool is linked against the sanitizer's runtime as a DLL, which is the
    # toolset's and on PATH only in a shell made for it: beside the tool, it runs
    # from any.
    $runtime = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
    if (-not (Test-Path $runtime)) {
        throw "No AddressSanitizer runtime at $runtime; add the 'C++ AddressSanitizer' component to Visual Studio."
    }
    Copy-Item $runtime $buildDir
}

if ($Lint) {
    Show-Line "`n== lint ($name) ==" -Colour Cyan
    # LLVM's own release, as tools\build.ps1 and DAOLib's cmake\Quality.cmake have it.
    $llvmBin = if ($env:LLVM_ROOT) { Join-Path $env:LLVM_ROOT 'bin' } else { Join-Path $env:ProgramFiles 'LLVM\bin' }
    $tidy = Join-Path $llvmBin 'clang-tidy.exe'
    if (-not (Test-Path $tidy)) { throw "No clang-tidy at $tidy; winget install LLVM.LLVM, or set LLVM_ROOT." }
    # Findings from our own headers alone, by this checkout's src\ whichever way its
    # slashes lean. The compile database is MSVC's and for 64 bits, which is what
    # clang takes it for; it takes /Zc:preprocessor and /external:anglebrackets
    # without using them, and is told not to say so.
    $headers = [regex]::Escape((Join-Path $repo 'src')).Replace('\\', '[/\\]') + '[/\\]'
    $sources = 'RelightTool.cpp', 'Relighter.cpp', 'ProbeBaker.cpp'
    $results = $sources | ForEach-Object -ThrottleLimit 3 -Parallel {
        $said = & $using:tidy -p $using:buildDir "--header-filter=$using:headers" `
            --extra-arg=-Wno-unused-command-line-argument --quiet (Join-Path $using:repo "src\$_") 2>&1
        [pscustomobject]@{ Source = $_; Exit = $LASTEXITCODE; Said = @($said) }
    }
    $failed = @()
    foreach ($result in $results) {
        foreach ($line in $result.Said) { Show-Line "$line" }
        if ($result.Exit -ne 0) { $failed += $result.Source }
    }
    if ($failed.Count -gt 0) { throw "lint failed: $($failed -join ', ')" }
    Show-Line "clang-tidy: $($sources.Count) sources, no findings" -Colour Green
}
Show-Line "`nOK: $buildDir\elgarashaders_relight.exe" -Colour Green
