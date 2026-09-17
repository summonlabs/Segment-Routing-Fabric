# Segment Routing Fabric 1.0.0 - full local verification chain.
# Copyright 2026 Summon Software Labs.
#
# Runs the complete verification the release was closed on. No step uses a test
# timeout: every suite runs plainly and is allowed to complete naturally.
#
#   pwsh -File scripts/verify.ps1 [-SkipAnalyze] [-SkipDebug] [-SkipSanitize]
#
[CmdletBinding()]
param(
    [switch] $SkipAnalyze,
    [switch] $SkipDebug,
    [switch] $SkipSanitize
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$script:Failures = @()
$script:Step = 0

function Invoke-Step {
    param([string] $Name, [scriptblock] $Body)
    $script:Step++
    Write-Host ''
    Write-Host ('[{0}] {1}' -f $script:Step, $Name) -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $Body
    if ($LASTEXITCODE -ne 0) {
        $script:Failures += ('{0} (exit {1})' -f $Name, $LASTEXITCODE)
        Write-Host ('    FAILED: {0}' -f $Name) -ForegroundColor Red
    } else {
        Write-Host ('    ok: {0}' -f $Name) -ForegroundColor Green
    }
}

function Invoke-CMake {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Arguments)
    & cmake @Arguments
}

# Locate the Visual Studio C++ toolset. The Visual Studio CMake generator finds the
# compiler by itself; the MSVC bin directory is only added to PATH so that the
# AddressSanitizer runtime is resolvable when the instrumented binaries run.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = $null
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsInstall) {
    throw 'Visual Studio with the C++ toolset was not found.'
}
$toolsetRoot = Join-Path $vsInstall 'VC\Tools\MSVC'
$toolset = Get-ChildItem $toolsetRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$msvcBin = Join-Path $toolset.FullName 'bin\Hostx64\x64'
$env:PATH = $msvcBin + ';' + ${env:PATH}
Write-Host ('toolchain: {0}' -f $msvcBin)

$generator = 'Visual Studio 17 2022'

function Invoke-Configure {
    param([string] $BuildDirectory, [string[]] $Extra)
    $arguments = @('-S', '.', '-B', $BuildDirectory, '-G', $generator, '-A', 'x64',
                   '-DSRF_BUILD_TESTS=ON', '-DSRF_BUILD_EXAMPLES=ON',
                   '-DSRF_BUILD_BENCH=ON', '-DSRF_BUILD_TOOLS=ON')
    if ($Extra) {
        $arguments += $Extra
    }
    & cmake @arguments
}

Invoke-Step 'configure and build Release' {
    Invoke-Configure -BuildDirectory 'build'
    if ($LASTEXITCODE -ne 0) { return }
    Invoke-CMake --build build --config Release
}

Invoke-Step 'every test suite (Release)' {
    & ctest --test-dir build -C Release --output-on-failure
}

if (-not $SkipDebug) {
    Invoke-Step 'configure and build Debug' {
        Invoke-Configure -BuildDirectory 'build-debug'
        if ($LASTEXITCODE -ne 0) { return }
        Invoke-CMake --build build-debug --config Debug
    }
    Invoke-Step 'every test suite (Debug)' {
        & ctest --test-dir build-debug -C Debug --output-on-failure
    }
}

if (-not $SkipAnalyze) {
    Invoke-Step 'MSVC static analysis with warnings as errors' {
        Invoke-Configure -BuildDirectory 'build-analyze' -Extra @('-DSRF_ANALYZE=ON')
        if ($LASTEXITCODE -ne 0) { return }
        Invoke-CMake --build build-analyze --config Release
    }
}

if (-not $SkipSanitize) {
    Invoke-Step 'configure and build AddressSanitizer' {
        Invoke-Configure -BuildDirectory 'build-asan' -Extra @('-DSRF_SANITIZE=ON')
        if ($LASTEXITCODE -ne 0) { return }
        Invoke-CMake --build build-asan --config Release
    }
    Invoke-Step 'every test suite (AddressSanitizer)' {
        & ctest --test-dir build-asan -C Release --output-on-failure
    }
}

Invoke-Step 'every example exits zero' {
    $examples = Get-ChildItem -Path build\examples\Release -Filter 'srf_example_*.exe' -ErrorAction SilentlyContinue
    if (-not $examples) {
        Write-Host '    no example binaries found'
        $global:LASTEXITCODE = 1
        return
    }
    foreach ($example in $examples) {
        Write-Host ('    -> {0}' -f $example.Name)
        & $example.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host ('    example failed: {0}' -f $example.Name) -ForegroundColor Red
            $global:LASTEXITCODE = 1
            return
        }
    }
    $global:LASTEXITCODE = 0
}

Invoke-Step 'benchmark runs' {
    & build\bench\Release\srf_bench.exe 1
}

Invoke-Step 'install and build the independent consumer' {
    $prefix = Join-Path $root 'dist'
    Invoke-CMake --install build --config Release --prefix $prefix
    if ($LASTEXITCODE -ne 0) { return }
    $work = Join-Path ([System.IO.Path]::GetTempPath()) 'srf_consumer_verify'
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Path $work | Out-Null
    Copy-Item -Path (Join-Path $root 'consumer\*') -Destination $work -Recurse
    Push-Location $work
    try {
        Invoke-CMake -S . -B build -G $generator -A x64 -DCMAKE_PREFIX_PATH=$prefix
        if ($LASTEXITCODE -ne 0) { return }
        Invoke-CMake --build build --config Release
        if ($LASTEXITCODE -ne 0) { return }
        & build\Release\srf_consumer.exe
    } finally {
        Pop-Location
    }
}

Write-Host ''
if ($script:Failures.Count -eq 0) {
    Write-Host 'VERIFICATION PASSED' -ForegroundColor Green
    exit 0
}
Write-Host 'VERIFICATION FAILED:' -ForegroundColor Red
foreach ($failure in $script:Failures) {
    Write-Host ('  - {0}' -f $failure)
}
exit 1
