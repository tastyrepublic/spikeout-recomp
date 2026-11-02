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
#   plain         both on Auto as they are: on a PC that keeps up, neither should go to one board (two games on one PC
#                 may not keep up)
# Every 120 frames each window's RAM hash of a board both run is compared through the session. Exit code 1 if
# any differed, or the one-board window never handed the other cabinet over to side runs (or, plain, one did).
# Each window's frame times every 300 frames (SPIKEOUT_NET_DEBUG) are in its output.
#
#   tools/onepc-window-test.ps1 [-OneBoard joiner|host|auto|none|plain] [-Frames 7000]
#
# The windows open and run at the arcade's speed (two minutes); don't touch them. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md). Outputs in build/onepc/window.
param([ValidateSet('joiner', 'host', 'auto', 'none', 'plain')][string]$OneBoard = 'joiner', [int]$Frames = 7000)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$out = 'build/onepc/window'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out/*.txt" -ErrorAction SilentlyContinue
$roms = 'build/roms/spikeofe'
function Start-Window($name, $keys, $presses) {
    $env:SPIKEOUT_MENU_KEYS = $keys
    $env:SPIKEOUT_TEST_PRESS = $presses
    $env:SPIKEOUT_NET_DEBUG = '1' # (every 300 frames: the frames' times, the waits for inputs, the game's logic)
    $mode = switch ($OneBoard) {
        'joiner' { if ($name -eq 'joiner') { '--net-one-board' } else { '--net-every-board' } }
        'host' { if ($name -eq 'host') { '--net-one-board' } else { '--net-every-board' } }
        'none' { '--net-every-board' }
        default { '--net-auto-board' }
    }
    if ($OneBoard -eq 'auto' -and $name -eq 'joiner') { $env:SPIKEOUT_TEST_AUTO_MS = '1' }
    $arglist = @($roms, '--frames', "$Frames") + $(if ($mode) { @($mode) } else { @() })
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
$joinWindow = Start-Window 'joiner' ('esc,down,enter' + (',F13' * 8) + ',enter,enter') $joinPresses
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
$expect = switch ($OneBoard) { 'joiner' { 'joiner' } 'host' { 'host' } 'auto' { 'joiner' } default { $null } }
if ($expect) {
    $file = "$out/$expect.txt"
    if (-not (Select-String -Path $file -Pattern 'one board here' -Quiet)) { "the $expect didn't go to one board"; $failed = $true }
    elseif (-not (Select-String -Path $file -Pattern 'handed back to side runs' -Quiet)) { "the $expect never handed the other cabinet over"; $failed = $true }
} elseif (Select-String -Path "$out/host.txt", "$out/joiner.txt" -Pattern 'one board here' -Quiet) {
    'a window went to one board'; $failed = $true
}
if ($failed) { 'FAILED'; exit 1 }
'the same'
