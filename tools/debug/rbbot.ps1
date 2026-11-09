# Players pressing all the time (gamerun --bot) on this PC through the lobby, each its datagrams held -LatencyMs:
# -Seats players, -OneBoard the processes (0: the host) on one board. Reports every "differs" and any crash.
param([string]$Tag = "rbbot", [string]$Net = "--net-rollback 12 --input-delay 1", [int]$Seats = 2, [int]$Frames = 8000, [int]$LatencyMs = 70, [string[]]$Extra = @(),
      [hashtable]$Env = @{}, [switch]$Hashes, [int[]]$OneBoard = @(), [string]$DumpEach = "", [switch]$LinkDump,
      [string]$Load = "", [int]$BotSeed = 0, [int]$OneBoardFrame = 100, [string]$HashBoard = "", [string]$Exe = "build\gamerun.exe") # (-Exe: another gamerun, an A/B)
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
Set-Location "Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31"
$d = "build-jit\bench"; New-Item -ItemType Directory -Force $d | Out-Null
$lobby = "wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby"
$common = @("Z:\Emu\spikeout-recomp\build\roms\spikeofe", "--blocks", "900000000000", "--no-sound", "--no-2d", "--pace", "--lobby", $lobby) + $(if ($Hashes) { @() } else { @("--no-frame-hashes") }) + ($Net -split " " | Where-Object { $_ }) + $Extra
if ($Load) { $common += @("--load-state", $Load) } # (every machine from its own copy of the same state)
function own($s) {
    $a = @("--bot", "$(if ($BotSeed) { $BotSeed } else { $s + 1 })")
    if ($OneBoard -contains $s) { $a += @("--one-board-auto", "--one-board-held", "--net-one-board", "$OneBoardFrame") }
    if ($DumpEach) { $a += @("--ram-dump", $DumpEach, "$d\$Tag-dump-$s.bin") }
    if ($LinkDump) { $a += @("--link-dump", "$d\$Tag-link-$s.bin") }
    if ($HashBoard) { $a += @("--ram-hashes", $HashBoard, "$d\$Tag-hashes-$s.bin") } # B@FROM: board B's RAM pages each frame
    $a
}
$env:SPIKEOUT_NET_DEBUG = "1"
if ($LatencyMs -gt 0) { $env:SPIKEOUT_TEST_LATENCY = "$LatencyMs" }
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$procs = @()
$procs += Start-Process -PassThru -WindowStyle Hidden -FilePath $Exe -ArgumentList ($common + @("--frames", "$Frames", "--net-host", "RB", "--seats", "$Seats", "--net-start-alone", "--private") + (own 0)) -RedirectStandardOutput "$d\$Tag-0.txt" -RedirectStandardError "$d\$Tag-0.err"
$code = $null
for ($i = 0; $i -lt 40 -and -not $code; $i++) { Start-Sleep 1; $m = Select-String -Path "$d\$Tag-0.txt" -Pattern 'on the lobby as ([A-Z2-9]{6})' | Select-Object -First 1; if ($m) { $code = $m.Matches[0].Groups[1].Value } }
for ($s = 1; $s -lt $Seats; $s++) {
    Start-Sleep 5
    $procs += Start-Process -PassThru -WindowStyle Hidden -FilePath $Exe -ArgumentList ($common + @("--frames", "$($Frames - 200 * $s)", "--net-join", $code, "--net-catch-up") + (own $s)) -RedirectStandardOutput "$d\$Tag-$s.txt" -RedirectStandardError "$d\$Tag-$s.err"
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep 1; if (Select-String -Path "$d\$Tag-0.txt" -Pattern "seat $($s + 1) taken" -Quiet) { break } }
}
Remove-Item Env:SPIKEOUT_NET_DEBUG, Env:SPIKEOUT_TEST_LATENCY -ErrorAction SilentlyContinue
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
$procs | Wait-Process -Timeout 500 -ErrorAction SilentlyContinue
foreach ($x in $procs) { if (-not $x.HasExited) { Stop-Process -Id $x.Id -Force } }
for ($s = 0; $s -lt $Seats; $s++) {
    $f = "$d\$Tag-$s.txt"
    $diff = @(Select-String -Path $f -Pattern 'differs from seat' | ForEach-Object Line)
    $rb = (Select-String -Path $f -Pattern '^Rollback: ' | Select-Object -First 1).Line
    $crash = (Select-String -Path "$d\$Tag-$s.err" -Pattern 'terminate|what\(\)|Assertion|gamerun:' | Select-Object -First 2 | ForEach-Object Line) -join ' / '
    "$Tag-${s}: $($diff.Count) differs; first: $($diff | Select-Object -First 1); $rb $(if ($crash) { "CRASH: $crash" })"
}
# How smoothly each player ran (tools/speed-check.ps1, the --pace speed lines; on one PC all three share its CPU)
& "$PSScriptRoot\..\speed-check.ps1" -Quiet (0..($Seats - 1) | ForEach-Object { "$d\$Tag-$_.txt" })
