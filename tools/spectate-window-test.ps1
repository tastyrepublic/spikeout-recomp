# Spectators in the window (docs/netplay.md, "Spectators"): windows on this machine, through their menus
# (SPIKEOUT_MENU_KEYS) with their buttons pressed at set frames (SPIKEOUT_TEST_PRESS).
#   -Mode switch  (default) two windows: one hosting a two-player game and starting it alone (player 1: coin and start
#                 at 1,200), the other watching it (Network play, Join a game, the game, Watch); the spectator switches to
#                 seat 2 with Right a while in, and back with Left.
#   -Mode oneboard three windows: the host, a second player joining (coin and start at 2,600: player 1's game), and a
#                 spectator on one board (--net-one-board): the other cabinets held whole until the game is calm, then
#                 on side runs; player 2's coin at 6,100 wants a shadow of its cabinet: its player's backup run on there.
#                 Right after that switches to player 2 on one board: every cabinet made whole from the players'
#                 backups, then one board again around player 2's; the host's coin at 6,800 wants player 1's cabinet
#                 (on side runs now) as a shadow: its backup run on there.
# Checks each window's output. Screenshots (F12) in build/spectate/window. The windows open and run at the arcade's speed;
# don't touch them. Run from PowerShell with the toolchain's PATH.
#
#   tools/spectate-window-test.ps1 [-Mode switch|oneboard] [-Frames N]
param([ValidateSet('switch', 'oneboard')][string]$Mode = 'switch', [int]$Frames = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$out = 'build/spectate/window'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out/*" -ErrorAction SilentlyContinue
$roms = (Resolve-Path 'build/roms/spikeofe').Path
$exe = (Resolve-Path 'build/spikeout.exe').Path
if ($Frames -eq 0) { $Frames = if ($Mode -eq 'oneboard') { 8000 } else { 3000 } }
function Start-Window($name, $keys, $presses, $frames, $board) {
    $env:SPIKEOUT_MENU_KEYS = $keys
    $env:SPIKEOUT_TEST_PRESS = $presses
    $p = Start-Process $exe -ArgumentList @($roms, '--frames', "$frames", $board) -WorkingDirectory $out -RedirectStandardOutput "$out/$name.txt" -RedirectStandardError "$out/$name.err" -PassThru
    Remove-Item Env:SPIKEOUT_MENU_KEYS, Env:SPIKEOUT_TEST_PRESS -ErrorAction SilentlyContinue
    $p
}
$wait = ',F13' * 40
# (the host: Network play, Host a game, Start hosting (2 players, this network), then Start game six seconds on)
$hostWindow = Start-Window 'host' ('esc,down,enter,down,enter,down,down,enter' + (',F13' * 24) + ',enter') $(if ($Mode -eq 'oneboard') { 'coin@1200,start@1300,beat@1700,coin@6800' } else { 'coin@1200,start@1300,beat@1700' }) $Frames '--net-every-board'
Start-Sleep -Seconds 1
$windows = @($hostWindow)
if ($Mode -eq 'oneboard') {
    # (player 2: Network play, Join a game two seconds on, the first game, Join; its coin at 2,600 and again at 6,100)
    $windows += Start-Window 'joiner' ('esc,down,enter' + (',F13' * 8) + ',enter,enter,enter') 'coin@2600,start@2700,beat@3300,beat@3650,coin@6100' $Frames '--net-every-board'
    Start-Sleep -Seconds 1
    # (the spectator, on one board: Watch; Right after the hand-over (about frame 5,300: the other cabinet on side runs): player 2)
    $windows += Start-Window 'spectator' ('esc,down,enter' + (',F13' * 8) + ',enter,enter,down,enter' + (',F13' * 420) + ',right') '' ($Frames - 400) '--net-one-board'
} else {
    # (the spectator: Watch; then F12, Right (seat 2), F12, Left (seat 1), F12, a few seconds apart)
    $windows += Start-Window 'spectator' ('esc,down,enter' + (',F13' * 8) + ',enter,enter,down,enter' + $wait + ',F12' + $wait + ',right' + $wait + ',F12' + $wait + ',left' + $wait + ',F12') '' ($Frames - 300) '--net-every-board'
}
$windows | ForEach-Object { $_.WaitForExit() }
$failed = $false
foreach ($name in 'host', 'joiner', 'spectator') {
    if (-not (Test-Path "$out/$name.txt")) { continue }
    "--- $name"
    Select-String -Path "$out/$name.txt" -Pattern 'watching|watches|network game|one board here|handed back|run on from its backup|no backup|shadow from|switching|one board again|differs from seat' |
        Select-Object -First 14 | ForEach-Object { $_.Line }
}
if (-not (Select-String -Path "$out/host.txt" -Pattern 'is watching' -Quiet)) { 'the host never saw the spectator'; $failed = $true }
if (-not (Select-String -Path "$out/spectator.txt" -Pattern 'watching a network game' -Quiet)) { "the spectator didn't watch"; $failed = $true }
if ($Mode -eq 'switch' -and -not (Select-String -Path "$out/spectator.txt" -Pattern 'watching seat 2' -Quiet)) { "the spectator didn't switch to seat 2"; $failed = $true }
if ($Mode -eq 'oneboard') {
    if (-not (Select-String -Path "$out/spectator.txt" -Pattern 'handed back to side runs' -Quiet)) { "the spectator never put the other cabinet on side runs"; $failed = $true }
    if (-not (Select-String -Path "$out/spectator.txt" -Pattern 'one board again around it' -Quiet)) { "the spectator didn't switch to player 2 on one board"; $failed = $true }
    if (@(Select-String -Path "$out/spectator.txt" -Pattern 'run on from its backup').Count -lt 2) { "not a shadow from a backup at each coin"; $failed = $true }
    if (Select-String -Path "$out/host.txt", "$out/joiner.txt" -Pattern 'differs from seat' -Quiet) { "the players' boards differed"; $failed = $true }
}
if ($failed) { 'FAILED'; exit 1 }
'watched'
