param([int[]]$Latencies = @(6, 3, 10), [int[]]$StaysList = @(1, 0, 2), [int]$Frames = 7500,
      [string]$State = 'build-jit\bench\three-6000.state', [int]$Boards = 3, [int]$From = 6010)
Set-Location Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$log = cmake --build build --target gamerun 2>&1
if ($LASTEXITCODE) { $log | Select-String error | Select-Object -First 5; 'STOPPED: build failed'; exit 2 }
foreach ($l in $Latencies) {
    foreach ($s in $StaysList) {
        $n = "build\onepc-diff-sim-$Boards-$s-$l"
        & .\tools\onepc-diff.ps1 -NoShadows -State $State -Boards $Boards -Stays $s -From $From -Frames $Frames -MaxBytes 16 -Out $n `
            -OneBoard @('--one-board-tasks', '--rollback-sim', "$l") *> "$n.log"
        $t = (Get-Content "$n.log") -join "`n"
        if ($t -match 'SAME:') { "latency $l, stays ${s}: SAME" } else { "latency $l, stays ${s}: FAILED"; Get-Content "$n.log" -TotalCount 45 | Select-Object -Skip 1; exit 1 }
    }
}
'ALL SAME'
