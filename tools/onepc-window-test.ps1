# One board a PC in the window (docs/netplay.md, "One board a PC in the window"): two windows on this machine,
# one hosting a two-player game on this network (player 1: coin and start at 1,200), the other joining it
# (player 2: at 2,600, joining player 1's game), both through their menus (SPIKEOUT_MENU_KEYS) with their buttons
# pressed at set frames (SPIKEOUT_TEST_PRESS). -OneBoard:
#   joiner, host  that window on one board from the start (--net-one-board), the other on every board
#                 (--net-every-board): the other cabinet held whole until the game is calm, then made from its
#                 player's buttons
#   auto          both on Auto (--net-auto-board), the joiner told its frames are too slow (SPIKEOUT_TEST_AUTO_MS=1):
#                 one board from frame ~900 (its first 300 timed frames after 600)
#   none          both on every board (--net-every-board)
#   leave         both on one board; the joiner leaving at 5,500 (its board handed to the host), then a gamerun
#                 joining the game in progress (every board's state from the host window), its hash checks compared
#   crash         the same, the joiner's window ended as a crash would (about frame 5,600): the host runs its board
#                 on from its last backup (tools::backup)
#   plain         both on Auto as they are: on a PC that keeps up, neither should go to one board (two games on one PC
#                 may not keep up)
# Every 120 frames each window's RAM hash of a board both run is compared through the session. Exit code 1 if
# any differed, or the one-board window never handed the other cabinet over to side runs (or, plain, one did).
# Each window's frame times every 300 frames (SPIKEOUT_NET_DEBUG) are in its output.
#
#   tools/onepc-window-test.ps1 [-OneBoard joiner|host|auto|none|plain|leave|crash] [-Frames 7000]
#
# The windows open and run at the arcade's speed (two minutes); don't touch them. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md). Outputs in build/onepc/window.
param([ValidateSet('joiner', 'host', 'auto', 'none', 'plain', 'leave', 'crash')][string]$OneBoard = 'joiner', [int]$Frames = 7000)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$out = 'build/onepc/window'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out/*.txt" -ErrorAction SilentlyContinue
$roms = 'build/roms/spikeofe'
function Start-Window($name, $keys, $presses, $frames = $Frames) {
    $env:SPIKEOUT_MENU_KEYS = $keys
    $env:SPIKEOUT_TEST_PRESS = $presses
    $env:SPIKEOUT_NET_DEBUG = '1' # (every 300 frames: the frames' times, the waits for inputs, the game's logic)
    $mode = switch ($OneBoard) {
        'joiner' { if ($name -eq 'joiner') { '--net-one-board' } else { '--net-every-board' } }
        'host' { if ($name -eq 'host') { '--net-one-board' } else { '--net-every-board' } }
        'none' { '--net-every-board' }
        'leave' { '--net-one-board' }
        'crash' { '--net-one-board' }
        default { '--net-auto-board' }
    }
    if ($OneBoard -eq 'auto' -and $name -eq 'joiner') { $env:SPIKEOUT_TEST_AUTO_MS = '1' }
    $arglist = @($roms, '--frames', "$frames") + $(if ($mode) { @($mode) } else { @() })
    $p = Start-Process build/spikeout.exe -ArgumentList $arglist -RedirectStandardOutput "$out/$name.txt" -RedirectStandardError "$out/$name.err" -PassThru
    Remove-Item Env:SPIKEOUT_MENU_KEYS, Env:SPIKEOUT_TEST_PRESS, Env:SPIKEOUT_NET_DEBUG
    Remove-Item Env:SPIKEOUT_TEST_AUTO_MS -ErrorAction SilentlyContinue
    $p
}
# (the host: Network play, Host a game, Start hosting (2 players, this network), then Start game six seconds on;
# the joiner, a second later: Network play, Join a game two seconds on (the game found nearby), the first game)
# (the other player's coin at 6,100, long after the hand-back: the one-board window's rules want a shadow then, from
# the other window's real board)
$hostPresses = 'coin@1200,start@1300,beat@1700' + $(if ($OneBoard -eq 'joiner') { ',coin@6100' } else { '' })
$joinPresses = 'coin@2600,start@2700,beat@3300,beat@3650' + $(if ($OneBoard -eq 'host') { ',coin@6100' } else { '' })
$hostWindow = Start-Window 'host' ('esc,down,enter,down,enter,down,down,enter' + (',F13' * 24) + ',enter') $hostPresses
Start-Sleep -Seconds 1
$joinFrames = if ($OneBoard -eq 'leave') { 5500 } else { $Frames }
$joinWindow = Start-Window 'joiner' ('esc,down,enter' + (',F13' * 8) + ',enter,enter') $joinPresses $joinFrames
$gone = $OneBoard -eq 'leave' -or $OneBoard -eq 'crash'
if ($OneBoard -eq 'crash') { # (the joiner's window ended as a crash would, mid-game: about frame 5,600)
    Start-Sleep -Seconds 104
    Stop-Process -Id $joinWindow.Id -Force
}
if ($gone) { # (the joiner's window gone: a gamerun joins the host's game in progress, as a new player)
    $joinWindow.WaitForExit()
    $game = Select-String -Path "$out/host.txt" -Pattern '^net: (\S+) joined, seat 2' | Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $exe = if (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
    $late = Start-Process $exe -ArgumentList @($roms, '--no-sound', '--no-2d', '--blocks', '30000000000', '--frames', "$($Frames - 300)", '--net-join', $game) -RedirectStandardOutput "$out/late.txt" -RedirectStandardError "$out/late.err" -NoNewWindow -PassThru
    if (-not $late.WaitForExit(240000)) { $late.Kill(); "the gamerun didn't finish in 4 minutes: stopped" } # (refused: it would wait on)
}
$hostWindow.WaitForExit()
$joinWindow.WaitForExit()
Remove-Item "spikeout-$Frames.png" -ErrorAction SilentlyContinue # (each window's last picture)

$failed = $false
foreach ($name in 'host', 'joiner') {
    "--- $name"
    Select-String -Path "$out/$name.txt" -Pattern 'seat \d of|one board here|every board takes|handed back|shadow from|shadow of|state at frame|differs from seat|stopped at frame' |
        Select-Object -First 12 | ForEach-Object { $_.Line }
}
$differs = @(Select-String -Path "$out/host.txt", "$out/joiner.txt" -Pattern 'differs from seat')
if ($differs.Count) { $failed = $true }
if ($gone) { # (the joiner's board handed to the host as it left, or run on there from its backup; the gamerun given every board's state)
    "--- the joiner gone, a gamerun joining"
    Select-String -Path "$out/host.txt", "$out/joiner.txt", "$out/late.txt" -Pattern 'leaving|gone|backup|shadow from its machine|every board|running on from|hash checks differed|taken from' | ForEach-Object { "$(Split-Path -Leaf $_.Path): $($_.Line)" }
    if ($OneBoard -eq 'leave') {
        if (-not (Select-String -Path "$out/joiner.txt" -Pattern "this board's state sent, leaving" -Quiet)) { "the joiner didn't send its board as it left"; $failed = $true }
        if (-not (Select-String -Path "$out/host.txt" -Pattern 'shadow from its machine''s state' -Quiet)) { "the host didn't run the joiner's board on"; $failed = $true }
    } elseif (-not (Select-String -Path "$out/host.txt" -Pattern 'run on from its backup at frame' -Quiet)) { "the host didn't run the joiner's board on from its backup"; $failed = $true }
    if (-not (Select-String -Path "$out/late.txt" -Pattern 'running on from the host' -Quiet)) { "the gamerun didn't join in progress"; $failed = $true }
    if (-not (Select-String -Path "$out/late.txt" -Pattern 'Network: 0 hash checks differed' -Quiet)) { "the gamerun's boards differed from the host's"; $failed = $true }
}
$expect = switch ($OneBoard) { 'joiner' { 'joiner' } 'host' { 'host' } 'auto' { 'joiner' } 'leave' { 'host' } 'crash' { 'host' } default { $null } }
if ($expect) {
    $file = "$out/$expect.txt"
    if (-not (Select-String -Path $file -Pattern 'one board here' -Quiet)) { "the $expect didn't go to one board"; $failed = $true }
    elseif (-not (Select-String -Path $file -Pattern 'handed back to side runs' -Quiet)) { "the $expect never handed the other cabinet over"; $failed = $true }
} elseif (Select-String -Path "$out/host.txt", "$out/joiner.txt" -Pattern 'one board here' -Quiet) {
    'a window went to one board'; $failed = $true
}
if ($failed) { 'FAILED'; exit 1 }
'the same'
