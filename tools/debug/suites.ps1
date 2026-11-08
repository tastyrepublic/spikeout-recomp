Set-Location Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$log = cmake --build build 2>&1
if ($LASTEXITCODE) { $log | Select-String error | Select-Object -First 5; 'STOPPED: build failed'; exit 2 }
'build ok (every target)'
# (each step its own PowerShell process, as run by hand, with a time limit: stopped and said if it runs over)
function step($name, $minutes, [string]$command, [string]$log) {
    $p = Start-Process -PassThru -NoNewWindow -FilePath powershell.exe -ArgumentList @('-NoProfile', '-Command', "$command *> $log") -WorkingDirectory (Get-Location)
    if (-not $p.WaitForExit($minutes * 60000)) {
        Stop-Process -Id $p.Id -Force; Get-Process gamerun -ErrorAction SilentlyContinue | Stop-Process -Force
        "STOPPED: $name ran over $minutes minutes"; exit 3
    }
}
step 'onepc-tests' 20 '.\tools\onepc-tests.ps1' 'build\suite-onepc3.log'
$n = @(Select-String -Path build\suite-onepc3.log -Pattern 'frames the same, same').Count
"onepc-tests: $n of 14 the same"
if ($n -lt 14) { 'FAILED: onepc-tests'; Get-Content build\suite-onepc3.log | Select-Object -First 20; exit 1 }
step 'onepc-net-tests' 40 '.\tools\onepc-net-tests.ps1' 'build\suite-net3.log'
if (-not (Select-String -Path build\suite-net3.log -Pattern '^all the same' -Quiet)) { 'FAILED: onepc-net-tests'; Get-Content build\suite-net3.log -Tail 20; exit 1 }
'onepc-net-tests: all the same'
step 'leaver-first' 20 '.\tools\onepc-net-tests.ps1 -Only leave3,vanish3 -LeaverFirst' 'build\suite-net-lf3.log'
if (-not (Select-String -Path build\suite-net-lf3.log -Pattern '^all the same' -Quiet)) { 'FAILED: leaver-first'; Get-Content build\suite-net-lf3.log -Tail 20; exit 1 }
'leaver-first: all the same'
step 'three-process repro' 15 '.\build-jit\rbbot.ps1 -Tag mix3fix -Seats 3 -OneBoard 1 -Frames 9000' 'build-jit\bench\mix3fix-run.txt'
Get-Content build-jit\bench\mix3fix-run.txt -Tail 3 | ForEach-Object { $_.Substring(0, [Math]::Min(110, $_.Length)) }
if (Select-String -Path build-jit\bench\mix3fix-run.txt -Pattern ': [1-9]\d* differs' -Quiet) { 'FAILED: three-process repro'; exit 1 }
'ALL PASSED'
