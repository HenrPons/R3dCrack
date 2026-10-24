Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$testDir = Join-Path $root "HostUnitTests"
$outDir = Join-Path $root "build\host-tests"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Find-VcVars {
    $vswhereCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )

    foreach ($vswhere in $vswhereCandidates) {
        if (Test-Path -LiteralPath $vswhere) {
            $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installPath) {
                $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path -LiteralPath $vcvars) {
                    return $vcvars
                }
            }
        }
    }

    return $null
}

function Quote-Arg($arg) {
    if ($arg -match '\s') {
        return '"' + $arg + '"'
    }
    return $arg
}

$includes = @(
    "/I$root\AddressUtil",
    "/I$root\CryptoUtil",
    "/I$root\KeyFinder",
    "/I$root\KeyFinderLib",
    "/I$root\Logger",
    "/I$root\secp256k1lib",
    "/I$root\util"
)

$sources = @(
    "$testDir\main.cpp",
    "$root\AddressUtil\Base58.cpp",
    "$root\AddressUtil\hash.cpp",
    "$root\CryptoUtil\checksum.cpp",
    "$root\CryptoUtil\Rng.cpp",
    "$root\CryptoUtil\sha256.cpp",
    "$root\CryptoUtil\ripemd160.cpp",
    "$root\KeyFinder\PuzzleCatalog.cpp",
    "$root\KeyFinderLib\KeyFinder.cpp",
    "$root\Logger\Logger.cpp",
    "$root\util\util.cpp",
    "$root\secp256k1lib\secp256k1.cpp"
)

$rsp = Join-Path $outDir "host_tests.rsp"
$clArgs = @("/nologo", "/EHsc", "/std:c++14", "/O2") + $includes + $sources + @("/Fe:HostUnitTests.exe", "Bcrypt.lib")
Set-Content -LiteralPath $rsp -Value ($clArgs | ForEach-Object { Quote-Arg $_ }) -Encoding ASCII

Push-Location $outDir
try {
    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if ($cl) {
        & $cl.Source "@$rsp"
    } else {
        $vcvars = Find-VcVars
        if (-not $vcvars) {
            Write-Error "cl.exe was not found and vcvars64.bat could not be located. Install Visual Studio Build Tools with C++ tools."
        }

        & cmd.exe /c "call `"$vcvars`" >nul && cl @`"$rsp`""
    }

    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & (Join-Path $outDir "HostUnitTests.exe")
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}
