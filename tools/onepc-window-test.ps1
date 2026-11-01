# One board a PC in the window (spikeout --net-one-board; docs/netplay.md, "One board a PC in the window"): two
# windows on this machine, one hosting a two-player game on this network (player 1: coin and start at 1,200),
# the other joining it (player 2: at 2,600, joining player 1's game), both through their menus
# (SPIKEOUT_MENU_KEYS) with their buttons pressed at set frames (SPIKEOUT_TEST_PRESS). One of them runs one board
# (-OneBoard host or joiner): its own board whole, the other cabinet made from its player's buttons (run whole
# as a shadow until the game is calm), the other window every board. Every 120 frames each window's RAM hash of
# a board both run is compared through the session: the one-board window's own against the other's copy of it.
# Exit code 1 if any differed, or the one-board window never handed the other cabinet over to side runs.
#
#   tools/onepc-window-test.ps1 [-OneBoard joiner|host|none] [-Frames 7000]
#
# The windows open and run at the arcade's speed (two minutes); don't touch them. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md). Outputs in build/onepc/window.
param([ValidateSet('joiner', 'host', 'none')][string]$OneBoard = 'joiner', [int]$Frames = 7000)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$out = 'build/onepc/window'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out/*.txt" -ErrorAction SilentlyContinue
$roms = 'build/roms/spikeofe'
function Start-Window($name, $keys, $presses, $one) {
    $env:SPIKEOUT_MENU_KEYS = $keys
    $env:SPIKEOUT_TEST_PRESS = $presses
    $env:SPIKEOUT_NET_DEBUG = '1' # (every 300 frames: the frames' times, the waits for inputs, the game's logic)
    $arglist = @($roms, '--frames', "$Frames") + $(if ($one) { @('--net-one-board') } else { @() })
    $p = Start-Process build/spikeout.exe -ArgumentList $arglist -RedirectStandardOutput "$out/$name.txt" -RedirectStandardError "$out/$name.err" -PassThru
    Remove-Item Env:SPIKEOUT_MENU_KEYS, Env:SPIKEOUT_TEST_PRESS, Env:SPIKEOUT_NET_DEBUG
    $p
}
# (the host: Network play, Host a game, Start hosting (2 players, this network), then Start game six seconds on;
# the joiner, a second later: Network play, Join a game two seconds on (the game found nearby), the first game)
$hostWindow = Start-Window 'host' ('esc,down,enter,down,enter,down,down,enter' + (',F13' * 24) + ',enter') 'coin@1200,start@1300,beat@1700' ($OneBoard -eq 'host')
Start-Sleep -Seconds 1
$joinWindow = Start-Window 'joiner' ('esc,down,enter' + (',F13' * 8) + ',enter,enter') 'coin@2600,start@2700,beat@3300,beat@3650' ($OneBoard -eq 'joiner')
$hostWindow.WaitForExit()
$joinWindow.WaitForExit()

$failed = $false
foreach ($name in 'host', 'joiner') {
    "--- $name"
    Select-String -Path "$out/$name.txt" -Pattern 'seat \d of|one board here|handed back|shadow from|shadow of|differs from seat|stopped at frame' |
        Select-Object -First 12 | ForEach-Object { $_.Line }
}
$differs = @(Select-String -Path "$out/host.txt", "$out/joiner.txt" -Pattern 'differs from seat')
if ($differs.Count) { $failed = $true }
if ($OneBoard -ne 'none') {
    $file = "$out/$OneBoard.txt"
    if (-not (Select-String -Path $file -Pattern 'one board here' -Quiet)) { "the $OneBoard didn't run one board"; $failed = $true }
    elseif (-not (Select-String -Path $file -Pattern 'handed back to side runs' -Quiet)) { "the $OneBoard never handed the other cabinet over"; $failed = $true }
}
if ($failed) { 'FAILED'; exit 1 }
'the same'
