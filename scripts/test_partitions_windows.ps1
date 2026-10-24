Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'x64/Release/clBitCrack.exe'
$folder = Join-Path $root ('build/partition-tests/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder -Force | Out-Null
function Hex-Number([string]$value) { [System.Numerics.BigInteger]::Parse(('0' + $value),[Globalization.NumberStyles]::AllowHexSpecifier) }
foreach($scenario in @(
    @{ Mode='full'; Count=1; Low='400000000000000000'; High='7FFFFFFFFFFFFFFFFF' },
    @{ Mode='full'; Count=3; Low='400000000000000000'; High='7FFFFFFFFFFFFFFFFF' },
    @{ Mode='gpu'; Count=3; Low='400000000000000000'; High='7FEFFFFFFFFFFFFFFF' },
    @{ Mode='cpu'; Count=1; Low='7FF000000000000000'; High='7FFFFFFFFFFFFFFFFF' }
)) {
$low = Hex-Number $scenario.Low
$high = Hex-Number $scenario.High
$expectedNext = $low
for ($index=0; $index -lt $scenario.Count; $index++) {
    $name = 'Local/RedCrackPartition-' + [guid]::NewGuid().ToString('N')
    $map = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($name,32)
    $view = $map.CreateViewAccessor()
    $view.Write(0,[int]0); $view.Write(12,[int]$PID)
    $checkpoint = Join-Path $folder "$($scenario.Mode)-$($scenario.Count)-slice$index.txt"
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    foreach($arg in @('--cpu','--cpu-threads','1','--puzzle','71','--partition',$scenario.Mode,'--partition-index',"$index",'--partition-count',"$($scenario.Count)",'--control',$name,'--continue',$checkpoint,'--follow')) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while($view.ReadInt32(8) -ne 2 -and !$process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 25 }
        if($view.ReadInt32(8) -ne 2) { throw 'Worker did not enter paused state' }
        $view.Write(4,[int]1)
        if(!$process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'Worker stop failed' }
        $data = @{}
        Get-Content -LiteralPath $checkpoint | ForEach-Object { $k,$v=$_ -split '=',2; $data[$k]=$v }
        $start=Hex-Number $data['start']; $end=Hex-Number $data['end']; $next=Hex-Number $data['next']
        if($start -ne $expectedNext -or $end -lt $start -or $next -ne $start) { throw 'Partition coverage or checkpoint mismatch' }
        $expectedNext=$end+1
    } finally {
        $view.Write(4,[int]1)
        if(!$process.WaitForExit(10000)) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose(); $view.Dispose(); $map.Dispose()
    }
}
if($expectedNext -ne $high+1) { throw 'Final partition does not cover GPU endpoint' }
Write-Output "Partition coverage PASS: $($scenario.Mode), $($scenario.Count) slice(s)"
}
# Resume at the inclusive endpoint: exactly one remaining key must be counted.
$checkpoint = Join-Path $folder 'full-3-slice2.txt'
$lines=Get-Content -LiteralPath $checkpoint
$endHex=($lines | Where-Object { $_ -like 'end=*' }) -replace '^end=',''
$lines | ForEach-Object { if($_ -like 'next=*') { "next=$endHex" } else { $_ } } | Set-Content -LiteralPath $checkpoint
& $exe --cpu --cpu-threads 1 --puzzle 71 --partition full --partition-index 2 --partition-count 3 --continue $checkpoint --follow | Out-Null
if($LASTEXITCODE -ne 0) { throw 'Endpoint resume failed' }
$nextHex=(Get-Content -LiteralPath $checkpoint | Where-Object { $_ -like 'next=*' }) -replace '^next=',''
if((Hex-Number $nextHex) -le (Hex-Number $endHex)) { throw 'Endpoint was not processed on resume' }
$output=(& $exe --cpu --puzzle 71 --partition full --partition-index 1 --partition-count 2 --continue $checkpoint --follow 2>&1) -join "`n"
if($LASTEXITCODE -ne 1 -or $output -notmatch 'Checkpoint conflicts') { throw 'Changed slice layout was not rejected' }
Write-Output 'Inclusive endpoint resume and layout mismatch: PASS'
