# Network play in windows, several players and board modes, over a simulated distance (docs/netplay.md): windows on
# this machine, one hosting on this network (the seats set in its Host a game page), the others joining it in turn,
# all through their menus (SPIKEOUT_MENU_KEYS), each player's buttons pressed at set frames (SPIKEOUT_TEST_PRESS:
# player 1 at 1,200, 2 at 2,600, 3 at 4,600, 4 at 5,600, each joining the game in progress), every datagram held
# SPIKEOUT_TEST_LATENCY (-Latency, as a network that far away; default "40,110@45": 40 ms each way, 110 from 45 s
# on). Each window's board mode its own (-Modes: every, one or auto for each seat). Checks: the windows' hash checks
# (every 120 frames, through the session: "differs from seat" fails it), and prints what each chose (the network
# settings, the input delay in play, one board, rollback's counts).
#
#   tools/net-window-matrix.ps1 -Modes every,one [-Frames 7000] [-Latency '40,110@45']
#
# The windows open and run at the arcade's speed; don't touch them. Run from PowerShell with the toolchain's PATH.
# Outputs in build/netmatrix.
param([Parameter(Mandatory)][ValidateSet('every', 'one', 'auto')][string[]]$Modes, [int]$Frames = 7000, [string]$Latency = '40,110@45')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$out = 'build/netmatrix'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out/*" -ErrorAction SilentlyContinue
$roms = (Resolve-Path 'build/roms/spikeofe').Path
$exe = (Resolve-Path 'build/spikeout.exe').Path
$seats = $Modes.Count
if ($seats -lt 2 -or $seats -gt 4) { throw 'two to four windows' }
$flag = @{ every = '--net-every-board'; one = '--net-one-board'; auto = '--net-auto-board' }
$presses = @('coin@1200,start@1300,beat@1700', 'coin@2600,start@2700,beat@3300,beat@3650', 'coin@4600,start@4700,beat@5300',
    'coin@5600,start@5700,beat@6300')
$env:SPIKEOUT_TEST_LATENCY = $Latency
$windows = @()
for ($s = 0; $s -lt $seats; $s++) {
    # (the host: Network play, Host a game, Players (Right for each over 2), Start hosting, Start game once all have
    # joined; each other: Network play, Join a game two seconds on, the first game, Join)
    $keys = if ($s -eq 0) { 'esc,down,enter,down,enter' + (',right' * ($seats - 2)) + ',down,down,enter' + (',F13' * (16 + 8 * $seats)) + ',enter' }
            else { 'esc,down,enter' + (',F13' * 8) + ',enter,enter,enter' }
    $env:SPIKEOUT_MENU_KEYS = $keys
    $env:SPIKEOUT_TEST_PRESS = $presses[$s]
    $windows += Start-Process $exe -ArgumentList @($roms, '--frames', "$Frames", $flag[$Modes[$s]]) -WorkingDirectory $out -RedirectStandardOutput "$out/seat$($s + 1).txt" -RedirectStandardError "$out/seat$($s + 1).err" -PassThru
    Start-Sleep -Seconds 1
}
Remove-Item Env:SPIKEOUT_MENU_KEYS, Env:SPIKEOUT_TEST_PRESS, Env:SPIKEOUT_TEST_LATENCY -ErrorAction SilentlyContinue
$windows | ForEach-Object { $_.WaitForExit() }
$failed = $false
for ($s = 1; $s -le $seats; $s++) {
    $file = "$out/seat$s.txt"
    "--- seat $s ($($Modes[$s - 1]))"
    Select-String -Path $file -Pattern 'network settings|network game, seat|input delay \d+ \(|one board here|every board takes|one board takes|rollback: went back|differs from seat' |
        Select-Object -First 12 | ForEach-Object { '  ' + $_.Line }
    if (-not (Select-String -Path $file -Pattern 'network game, seat' -Quiet)) { "  seat $s never played"; $failed = $true }
}
if (Select-String -Path "$out/seat*.txt" -Pattern 'differs from seat' -Quiet) { $failed = $true }
if ($failed) { 'FAILED'; exit 1 }
'the same'
