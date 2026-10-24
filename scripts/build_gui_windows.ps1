Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $root "BitCrack.sln"
$guiProject = Join-Path $root "PuzzleGui\PuzzleGui.vcxproj"
$localOpenCl = Join-Path $root "deps\OpenCL-SDK"

function Find-MSBuild {
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $vswhereCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )

    foreach ($vswhere in $vswhereCandidates) {
        if (Test-Path -LiteralPath $vswhere) {
            $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
            if ($path) {
                return $path
            }
        }
    }

    return $null
}

$msbuild = Find-MSBuild
if (-not $msbuild) {
    Write-Error "MSBuild was not found. Install Visual Studio 2022 Build Tools with the Desktop development with C++ workload."
}

if (-not $env:OPENCL_INCLUDE) {
    $candidate = Join-Path $localOpenCl "include"
    if (Test-Path -LiteralPath (Join-Path $candidate "CL\cl.h")) {
        $env:OPENCL_INCLUDE = $candidate
    }
}

if (-not $env:OPENCL_LIB) {
    $candidate = Join-Path $localOpenCl "lib"
    if (Test-Path -LiteralPath (Join-Path $candidate "OpenCL.lib")) {
        $env:OPENCL_LIB = $candidate
    }
}

if (-not $env:OPENCL_INCLUDE) {
    Write-Error "Set OPENCL_INCLUDE to the directory containing CL\cl.h, for example deps\OpenCL-SDK\include."
}

if (-not $env:OPENCL_LIB) {
    Write-Error "Set OPENCL_LIB to the directory containing OpenCL.lib, for example deps\OpenCL-SDK\lib."
}

$savedPath = $env:PATH
try {
Write-Host "Building kernel embedding tool from source..."
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$toolDir = Join-Path $root 'tools'
New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
& $msbuild (Join-Path $root 'embedcl/embedcl.vcxproj') /m:1 /nr:false /p:PreferredToolArchitecture=x64 /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$toolDir\"
if ($LASTEXITCODE -ne 0) { throw 'Kernel embedding tool build failed' }
Write-Host "Building OpenCL worker..."
Remove-Item Env:PATH -ErrorAction SilentlyContinue
& $msbuild $solution /m:1 /nr:false /p:PreferredToolArchitecture=x64 /p:Configuration=Release /p:Platform=x64 /t:clKeyFinder
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Building GUI..."
& $msbuild $guiProject /m:1 /nr:false /p:PreferredToolArchitecture=x64 /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$worker = Join-Path $root "x64\Release\clBitCrack.exe"
$gui = Join-Path $root "x64\Release\RedCrack.exe"

if (-not (Test-Path -LiteralPath $worker)) {
    Write-Error "Build finished, but clBitCrack.exe was not found at $worker"
}

if (-not (Test-Path -LiteralPath $gui)) {
    Write-Error "Build finished, but RedCrack.exe was not found at $gui"
}

Write-Host "Built worker: $worker"
Write-Host "Built GUI:    $gui"

} finally { $env:PATH = $savedPath }
