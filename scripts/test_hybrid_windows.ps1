Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'x64\Release\clBitCrack.exe'
$folder = Join-Path $root ('build\hybrid-tests\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$checkpoint = Join-Path $folder 'cpu.txt'
$name = 'Local\PuzzleTest-' + [guid]::NewGuid().ToString('N')
$map = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($name, 32)
$view = $map.CreateViewAccessor()
$view.Write(0, [int]0)
$view.Write(12, [int]$PID)
$jobs = [Collections.Generic.List[object]]::new()
function Start-Worker {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($arg in @('--cpu', '--cpu-threads', '2', '--puzzle', '71', '--partition', 'cpu', '--continue', $checkpoint, '--control', $name, '--follow')) {
        $info.ArgumentList.Add($arg)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    [void]$process.Start()
    $job = [pscustomobject]@{ Process=$process; Out=$process.StandardOutput.ReadToEndAsync(); Err=$process.StandardError.ReadToEndAsync() }
    $jobs.Add($job)
    return $job
}
function Require($condition, $message) { if (!$condition) { throw $message } }
try {
    $first = Start-Worker
    Start-Sleep -Milliseconds 1000
    Require (!$first.Process.HasExited) 'Paused CPU worker exited'
    Require ($view.ReadInt32(8) -eq 2) 'Zero percent did not pause CPU'
    $duplicate = Start-Worker
    Require ($duplicate.Process.WaitForExit(5000)) 'Duplicate checkpoint worker did not exit'
    Require ($duplicate.Process.ExitCode -eq 1) 'Duplicate checkpoint was not rejected'
    Require ($duplicate.Err.Result.Contains('Checkpoint is in use')) 'Missing checkpoint lock error'

    $view.Write(0, [int]25)
    Start-Sleep -Milliseconds 4500
    $slow = $view.ReadInt64(16)
    Require ($slow -gt 0) 'CPU did not publish throughput at 25 percent'
    $view.Write(0, [int]100)
    Start-Sleep -Milliseconds 4500
    $fast = $view.ReadInt64(16)
    Require ($fast -gt $slow) 'Raising CPU limit did not increase measured throughput'

    $view.Write(0, [int]0)
    Start-Sleep -Milliseconds 250
    $first.Process.Refresh()
    $before = $first.Process.TotalProcessorTime.TotalMilliseconds
    Start-Sleep -Milliseconds 800
    $first.Process.Refresh()
    $pausedCpu = $first.Process.TotalProcessorTime.TotalMilliseconds - $before
    Require ($pausedCpu -lt 200) 'Paused worker still consumes significant CPU'
    $view.Write(4, [int]1)
    Require ($first.Process.WaitForExit(10000)) 'Graceful stop timed out'
    Require ($first.Process.ExitCode -eq 0) 'CPU worker failed'
    Require (Test-Path -LiteralPath $checkpoint) 'Final checkpoint missing'
    $nextLine = Get-Content -LiteralPath $checkpoint | Where-Object { $_.StartsWith('next=') }
    $next = $nextLine.Substring(5)
    Require ($next -ne '00000000000000000000000000000000000000000000007FF000000000000000') 'CPU checkpoint did not advance'

    $view.Write(4, [int]0)
    $view.Write(8, [int]0)
    $resume = Start-Worker
    Start-Sleep -Milliseconds 700
    $view.Write(4, [int]1)
    Require ($resume.Process.WaitForExit(10000)) 'Resume stop timed out'
    Require ($resume.Process.ExitCode -eq 0) 'Resume failed'
    Require ($resume.Err.Result.Contains('Starting at: ' + $next)) 'Resume ignored saved next key'
    Require ((Get-Content -LiteralPath $checkpoint | Where-Object { $_.StartsWith('next=') }) -eq $nextLine) 'Paused resume changed progress'
    Write-Host "Hybrid control tests passed: 25%=$slow Key/s; 100%=$fast Key/s; paused CPU=$pausedCpu ms/800ms."
} finally {
    $view.Write(4, [int]1)
    foreach ($job in $jobs) {
        if (!$job.Process.HasExited -and !$job.Process.WaitForExit(10000)) { $job.Process.Kill(); $job.Process.WaitForExit() }
        $job.Process.Dispose()
    }
    $view.Dispose()
    $map.Dispose()
}
