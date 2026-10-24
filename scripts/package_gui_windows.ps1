param([switch]$SkipBuild)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$SkipBuild) {
    $savedPath = $env:PATH
    try {
        & (Join-Path $PSScriptRoot 'build_gui_windows.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'GUI build failed' }
    } finally {
        $env:PATH = $savedPath
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redistRoot = Join-Path $vsPath 'VC\Redist\MSVC'
$version = Get-ChildItem -LiteralPath $redistRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$version) { throw 'Visual C++ redistributable files were not found' }
$crt = Join-Path $version.FullName 'x64\Microsoft.VC143.CRT'
if (!(Test-Path -LiteralPath $crt)) { throw "Missing runtime: $crt" }

# Always create a new directory: never package personal checkpoints or results.
$name = 'RedCrack-Windows-x64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$dist = Join-Path $root 'dist'
$package = Join-Path $dist $name
New-Item -ItemType Directory -Path $package | Out-Null
foreach ($exe in @('RedCrack.exe', 'clBitCrack.exe')) {
    Copy-Item -LiteralPath (Join-Path $root "x64\Release\$exe") -Destination $package
}
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.MIT') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'packaging\README.txt') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'SECURITY.md') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'packaging/SUPPORT.md') -Destination $package
New-Item -ItemType Directory -Path (Join-Path $package 'assets') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'assets/donations') -Destination (Join-Path $package 'assets') -Recurse
$revision = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source revision' }
$dirty = & git -C $root status --porcelain --untracked-files=no
if ($dirty) { throw 'Commit source changes before making a release package' }
Set-Content -LiteralPath (Join-Path $package 'BUILD_INFO.txt') -Encoding ascii -Value @(
    "Source: https://github.com/HenrPons/RedCrack/tree/$revision",
    "Commit: $revision",
    'Platform: Windows x64; toolchain: MSVC v143',
    'Unsigned preview build. See the repository for validation limits.'
)

$hashes = Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    $relative = [IO.Path]::GetRelativePath($package,$_.FullName).Replace('\','/')
    $hash.Hash.ToLowerInvariant() + '  ' + $relative
}
Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Value $hashes -Encoding ascii
$zip = Join-Path $dist ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($zip + '.sha256') -Encoding ascii -Value ($zipHash + '  ' + [IO.Path]::GetFileName($zip))
Copy-Item -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Destination (Join-Path $dist ($name + '-SHA256SUMS.txt'))
Write-Host "Executable: $(Join-Path $package 'RedCrack.exe')"
Write-Host "Distribution: $zip"
