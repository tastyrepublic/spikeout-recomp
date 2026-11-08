# A play test: the window on this PC hosts a private 3-player internet game, on one board (--net-one-board), with
# sound; two bots (gamerun --bot: random presses, coin and start every 3,000 frames) join it through the lobby on
# this PC, each its own board, rollback and Auto input delay as the internet tests. Logs in build-jit\shots\play3.
# Close the window to end: the bots then end too.
param([int]$Seats = 3, [string[]]$Extra = @('--net-one-board'))
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = "Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31"
$dir = "$root\build-jit\shots\play3"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*.txt" -Force -ErrorAction SilentlyContinue
Copy-Item "$root\build\spikeout.exe" "$dir\spikeout.exe" -Force
Set-Content "$dir\portable.txt" ""
"picture_shape = 3`nrender_scale = 2`nantialiasing = 1`nquads = 1`nshow_fps = 1`nshow_net = 1`ncheck_updates = 0`nplayer_name = DON" | Set-Content "$dir\settings.ini"
$env:SPIKEOUT_TEST_HOST = "$Seats"
$env:SPIKEOUT_NET_DEBUG = "1"
$w = Start-Process "$dir\spikeout.exe" -ArgumentList (@('Z:\Emu\spikeout-recomp\build\roms\spikeofe') + $Extra) -WorkingDirectory $dir `
    -RedirectStandardOutput "$dir\window.txt" -RedirectStandardError "$dir\window.err" -PassThru
Remove-Item Env:SPIKEOUT_TEST_HOST
$code = $null
for ($i = 0; $i -lt 60 -and -not $code; $i++) {
    Start-Sleep 1
    $m = Select-String -Path "$dir\window.txt" -Pattern 'test room ([A-Z2-9]{6})' | Select-Object -First 1
    if ($m) { $code = $m.Matches[0].Groups[1].Value }
}
if (-not $code) { "STOPPED: the window offered no room in 60 s"; exit 1 }
"room $code (window $($w.Id))"
$lobby = "wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby"
$bots = @()
for ($s = 1; $s -lt $Seats; $s++) {
    Start-Sleep 5
    $a = @('Z:\Emu\spikeout-recomp\build\roms\spikeofe', '--blocks', '900000000000', '--no-sound', '--no-2d', '--pace', '--frames', '300000',
           '--lobby', $lobby, '--net-rollback', '12', '--input-delay', 'auto', '--net-join', $code, '--net-catch-up', '--bot', "$($s + 10)")
    $bots += Start-Process -PassThru -WindowStyle Hidden -FilePath "$root\build\gamerun.exe" -ArgumentList $a `
        -RedirectStandardOutput "$dir\bot$s.txt" -RedirectStandardError "$dir\bot$s.err"
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep 1; if (Select-String -Path "$dir\bot$s.txt" -Pattern 'joined' -Quiet) { break } }
    "bot $s joined: $([bool](Select-String -Path "$dir\bot$s.txt" -Pattern 'joined' -Quiet))"
}
Remove-Item Env:SPIKEOUT_NET_DEBUG -ErrorAction SilentlyContinue
"bots: $($bots.Id -join ', ')"
