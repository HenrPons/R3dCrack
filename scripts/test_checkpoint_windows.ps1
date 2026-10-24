Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'x64\Release\clBitCrack.exe'
$testDir = Join-Path $root 'build\checkpoint-tests'
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
$fixture = Join-Path $testDir 'completed.txt'
$lines = @(
    'start=400000000000000000',
    'next=400000000000000011',
    'end=400000000000000010',
    'stride=10000000000000000',
    'elapsed=5000000000'
)
Set-Content -LiteralPath $fixture -Value $lines -Encoding ascii
function Check-Run([string[]]$Arguments, [int]$ExpectedExit, [string]$ExpectedText) {
    $output = (& $exe @Arguments 2>&1) -join "`n"
    if ($LASTEXITCODE -ne $ExpectedExit -or !$output.Contains($ExpectedText)) {
        throw "Checkpoint regression failed: $output"
    }
}
$baseArgs = @('--puzzle', '71', '--continue', $fixture)
Check-Run $baseArgs 0 'Checkpoint range already completed'
Check-Run ($baseArgs + @('--stride', '1')) 1 'Checkpoint conflicts'
Check-Run ($baseArgs + @('--keyspace', '400000000000000020:400000000000000030')) 1 'Checkpoint conflicts'
Set-Content -LiteralPath $fixture -Value ($lines -replace '^stride=.*$', 'stride=0') -Encoding ascii
Check-Run $baseArgs 1 'Invalid checkpoint'
Write-Host 'Checkpoint regression tests passed'
