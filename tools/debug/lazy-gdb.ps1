Set-Location Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$env:ROLLBACK_SIM_LAZY = '1'; $env:ONEBOARD_NO_SHADOWS = '1'
$d = 'build\lazy-gdb'; New-Item -ItemType Directory -Force $d | Out-Null
$args0 = @('-batch', '-ex', 'set pagination off', '-ex', 'run', '-ex', 'bt 25', '-ex', 'info threads', '--args', 'build\gamerun.exe',
           'Z:\Emu\spikeout-recomp\build\roms\spikeofe', '--boards', '3', '--parallel', '--load-state', 'build-jit\bench\three-6000.state',
           '--frames', '7500', '--blocks', '900000000000', '--no-sound', '--no-2d', '--no-frame-hashes', '--bot', '1',
           '--one-board', '1@6010', '--one-board', '2@6010', '--one-board-stays', '0', '--one-board-tasks', '--rollback-sim', '10')
foreach ($k in 1..6) {
    $p = Start-Process -PassThru -NoNewWindow -FilePath gdb -ArgumentList $args0 -RedirectStandardOutput "$d\run$k.txt" -RedirectStandardError "$d\run$k.err"
    if (-not $p.WaitForExit(900000)) { Stop-Process -Id $p.Id -Force; Get-Process gamerun -ErrorAction SilentlyContinue | Stop-Process -Force; "run ${k}: STOPPED (over 15 min)"; break }
    $t = Get-Content "$d\run$k.txt" -Raw
    if ($t -match 'received signal|terminate called|Exception') { "run ${k}: CRASHED"; (Get-Content "$d\run$k.txt" | Select-String '^#|signal|terminate|what\(\)' | Select-Object -First 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }); break }
    "run ${k}: ran to the end"
}
