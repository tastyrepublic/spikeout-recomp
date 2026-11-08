param([string[]]$Patterns = @('8:9:6020', '12:13:6020', '3:4:6020', '6:20:6020', '2:3:6020'), [int]$Stays = 1, [int]$Frames = 7500,
      [string]$State = 'build-jit\bench\three-6000.state', [int]$Boards = 3, [int]$From = 6010, [switch]$Shadows)
Set-Location Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$log = cmake --build build --target gamerun 2>&1
if ($LASTEXITCODE) { $log | Select-String error | Select-Object -First 5; 'STOPPED: build failed'; exit 2 }
foreach ($p in $Patterns) {
    $n = "build\onepc-diff-rbf-$Boards-$Stays-" + ($p -replace ':', '-')
    $a = @{ State = $State; Boards = $Boards; Stays = $Stays; From = $From; Frames = $Frames; MaxBytes = 16; Out = $n
            OneBoard = @('--one-board-tasks', '--rollback-force', $p) }
    if (-not $Shadows) { $a.NoShadows = $true }
    & .\tools\onepc-diff.ps1 @a *> "$n.log"
    $t = (Get-Content "$n.log") -join "`n"
    if ($t -match 'SAME:') { "${p}: SAME" } else { "${p}: FAILED"; Get-Content "$n.log" -TotalCount 40 | Select-Object -Skip 1; exit 1 }
}
'ALL SAME'
