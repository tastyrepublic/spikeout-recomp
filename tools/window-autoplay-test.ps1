# The play test without a person: the window hosts a private 3-player game on one board (as play3.ps1), its own
# player a random press list in the bot's way (SPIKEOUT_TEST_PRESS: moves, beats, jumps, charges, shifts; coin and
# start at 1,200), two gamerun bots join; the window stops after -Frames. Reports each process's "differs" lines.
param([int]$Frames = 6000, [int]$Seed = 1, [string[]]$Extra = @('--net-one-board'), [string]$Tag = 'auto', [int]$HashFrom = 0)
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = "Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31"
$dir = "$root\build-jit\shots\$Tag"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*.txt", "$dir\*.err" -Force -ErrorAction SilentlyContinue
Copy-Item "$root\build\spikeout.exe" "$dir\spikeout.exe" -Force
Set-Content "$dir\portable.txt" ""
"picture_shape = 3`nrender_scale = 2`nantialiasing = 1`nquads = 1`nshow_fps = 1`nshow_net = 1`ncheck_updates = 0`nplayer_name = WIN" | Set-Content "$dir\settings.ini"
# (the window's player: random presses from 1,200 on, as gamerun --bot makes them)
$r = [System.Random]::new($Seed)
$p = @('coin@1200:6', 'start@1260:6')
$dirs = 'up', 'down', 'left', 'right'
for ($f = 1400; $f -lt $Frames - 100;) {
    switch ($r.Next(10)) {
        { $_ -le 3 } { $p += "$($dirs[$r.Next(4)])@${f}:$(10 + $r.Next(35))"; $f += 8 + $r.Next(20); break }
        { $_ -le 6 } { $p += "beat@${f}:$(2 + $r.Next(5))"; $f += 10 + $r.Next(15); break }
        7 { $p += "jump@${f}:$(3 + $r.Next(4))"; $f += 20 + $r.Next(20); break }
        8 { $p += "charge@${f}:$(20 + $r.Next(30))"; $f += 40 + $r.Next(20); break }
        default { $p += "shift@${f}:4"; $f += 15 + $r.Next(15) }
    }
}
$env:SPIKEOUT_TEST_PRESS = $p -join ','
$env:SPIKEOUT_TEST_HOST = '3'
$env:SPIKEOUT_NET_DEBUG = '1'
if ($HashFrom) { $env:SPIKEOUT_RAM_HASHES = "0@${HashFrom}:$dir\window.hashes" } # (-HashFrom: board 0's RAM each frame, here and in each bot)
$w = Start-Process "$dir\spikeout.exe" -ArgumentList (@('Z:\Emu\spikeout-recomp\build\roms\spikeofe', '--mute', '--frames', "$Frames") + $Extra) `
    -WorkingDirectory $dir -RedirectStandardOutput "$dir\window.txt" -RedirectStandardError "$dir\window.err" -PassThru
Remove-Item Env:SPIKEOUT_TEST_HOST, Env:SPIKEOUT_TEST_PRESS, Env:SPIKEOUT_RAM_HASHES -ErrorAction SilentlyContinue
$code = $null
for ($i = 0; $i -lt 60 -and -not $code; $i++) {
    Start-Sleep 1
    $m = Select-String -Path "$dir\window.txt" -Pattern 'test room ([A-Z2-9]{6})' | Select-Object -First 1
    if ($m) { $code = $m.Matches[0].Groups[1].Value }
}
if (-not $code) { Stop-Process -Id $w.Id -Force -ErrorAction SilentlyContinue; "STOPPED: the window offered no room in 60 s"; exit 1 }
"room $code"
$lobby = "wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby"
$bots = @()
foreach ($s in 1, 2) {
    Start-Sleep 5
    $a = @('Z:\Emu\spikeout-recomp\build\roms\spikeofe', '--blocks', '900000000000', '--no-sound', '--no-2d', '--pace', '--frames', "$($Frames - 300 * $s)",
           '--lobby', $lobby, '--net-rollback', '12', '--input-delay', 'auto', '--net-join', $code, '--net-catch-up', '--bot', "$($Seed * 10 + $s)")
    if ($HashFrom) { $a += @('--ram-hashes', "0@$HashFrom", (Join-Path $dir "bot$s.hashes")) }
    $bots += Start-Process -PassThru -WindowStyle Hidden -FilePath "$root\build\gamerun.exe" -ArgumentList $a `
        -RedirectStandardOutput "$dir\bot$s.txt" -RedirectStandardError "$dir\bot$s.err"
}
Remove-Item Env:SPIKEOUT_NET_DEBUG -ErrorAction SilentlyContinue
# (the window ends itself at -Frames; a time limit all the same)
if (-not $w.WaitForExit(($Frames / 50 + 240) * 1000)) { Stop-Process -Id $w.Id -Force; "STOPPED: the window ran over its time" }
$bots | ForEach-Object { if (-not $_.WaitForExit(60000)) { Stop-Process -Id $_.Id -Force } }
foreach ($n in 'window', 'bot1', 'bot2') {
    $diffs = @(Select-String -Path "$dir\$n.txt" -Pattern 'differs from seat' | ForEach-Object Line)
    $hb = @(Select-String -Path "$dir\$n.txt" -Pattern 'handed back to side runs from' | ForEach-Object Line)
    "${n}: $($diffs.Count) differs$(if ($diffs) { '; first: ' + $diffs[0] }); hand-backs: $($hb.Count)$(if ($hb) { ' first: ' + $hb[0] })"
}
# How smoothly each ran (tools/speed-check.ps1: the window's and the bots' speed lines; SLOW: exit code 2)
& "$PSScriptRoot\speed-check.ps1" "$dir\window.txt", "$dir\bot1.txt", "$dir\bot2.txt"
if ($LASTEXITCODE -eq 1) { 'SLOW (the speed lines above)'; exit 2 }
