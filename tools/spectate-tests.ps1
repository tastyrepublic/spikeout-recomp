# Spectators (gamerun --net-join NAME --net-spectate; docs/netplay.md, "Spectators"): gamerun processes on this
# machine over localhost, two players (every board each, delay-based) and a spectator, from power-on (player 1 in
# a game alone at 1,300, player 2 joining it at 2,700). The spectator's boards are compared, frame by frame, with
# the host's (or, after a host change, the new host's): every frame both ran. Exit code 1 if any differs.
#
#   tools/spectate-tests.ps1 [-Only before,late,migrate,leave,oneboard,oneboard2]
#
# before: the spectator in the room before the start. late: it comes 15 s into the game (every board's state from
# the host). migrate: in from the start; the host vanishes at frame 2,500 (as a crash: no goodbye), the other
# player hosts the room from then on, and the spectator goes on through it. leave: the same, the host leaving
# with a goodbye (--net-leave-at 2500). Outputs go to build/spectate. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md).
param([string[]]$Only)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$exe = if (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
$out = 'build/spectate'
New-Item -ItemType Directory -Force $out | Out-Null
$poweron = @('--poke', '0:10019E=1@0:2000', '--poke', '1:10019E=2@0:2000', '--press', '0:coin@1200', '--press', '0:start@1300',
    '--press', '0:beat@1700', '--press', '1:coin@2600', '--press', '1:start@2700', '--press', '1:beat@3300', '--press', '1:beat@3650')
$common = @('build/roms/spikeofe', '--no-sound', '--no-2d', '--input-delay', '2', '--frame-hashes', '--clock', '1000000000', '--blocks', '400000000000') + $poweron
# (a spectator on one board, as the window's: the other cabinet held whole until the game is calm, then made from its
# player's buttons and, where that can't follow, its player's backups run on here)
$oneboard = @('--net-one-board', '100', '--one-board-auto', '--one-board-held')

# Name, the spectator's delay (ms after the players), the host's extra options, whom to compare with (0: the host,
# 1: the player), the spectator's extra options, the boards compared (one board: only the one it watches runs whole),
# the players' frames (the spectator's 400 fewer) and more presses for every process.
$tests = @(
    @('before', 0, @(), 0, @(), @(0, 1), 4200, @()),
    @('late', 15000, @(), 0, @(), @(0, 1), 4200, @()),
    @('migrate', 0, @('--net-vanish-at', '2500'), 1, @(), @(0, 1), 4200, @()),
    @('leave', 0, @('--net-leave-at', '2500'), 1, @(), @(0, 1), 4200, @()),
    @('oneboard', 0, @(), 0, $oneboard, @(0), 8000, @('--pace', '--press', '1:coin@6100')),
    @('oneboard2', 0, @(), 0, ($oneboard + @('--net-spectate-seat', '1')), @(1), 8000, @('--pace', '--press', '0:coin@6100'))
)
$pattern = { param($b) "^board $b frame (\d+): RAM (\w+)" }
$failed = 0
foreach ($t in $tests) {
    $name, $delay, $hostopts, $against, $spectopts, $compared, $frames, $more = $t
    if ($Only -and $Only -notcontains $name) { continue }
    $game = "spectate-$name-$PID"
    $procs = @(Start-Process $exe -ArgumentList ($common + $more + $hostopts + @('--frames', "$frames", '--net-host', $game, '--seats', '2')) -RedirectStandardOutput "$out/$name-0.txt" -RedirectStandardError "$out/$name-0.err" -NoNewWindow -PassThru)
    Start-Sleep -Milliseconds 500
    $spect = { Start-Process $exe -ArgumentList ($common + $more + $spectopts + @('--frames', "$($frames - 400)", '--net-join', $game, '--net-spectate')) -RedirectStandardOutput "$out/$name-s.txt" -RedirectStandardError "$out/$name-s.err" -NoNewWindow -PassThru }
    if ($delay -eq 0) { $procs += & $spect; Start-Sleep -Milliseconds 800 }
    $procs += Start-Process $exe -ArgumentList ($common + $more + @('--frames', "$frames", '--net-join', $game)) -RedirectStandardOutput "$out/$name-1.txt" -RedirectStandardError "$out/$name-1.err" -NoNewWindow -PassThru
    if ($delay -gt 0) { Start-Sleep -Milliseconds $delay; $procs += & $spect }
    $procs | ForEach-Object { $_.WaitForExit() }

    $ref = "$out/$name-$against.txt"
    $mine = "$out/$name-s.txt"
    if (-not (Select-String -Path $mine -Pattern 'watching 2 cabinets' -Quiet)) { "$name : the spectator didn't start (see $mine, $out/$name-s.err)"; $failed++; continue }
    $same = 0; $differ = 0; $first = $null
    foreach ($b in $compared) {
        $want = @{}
        Select-String -Path $ref -Pattern (& $pattern $b) | ForEach-Object { $want[[int]$_.Matches[0].Groups[1].Value] = $_.Matches[0].Groups[2].Value }
        Select-String -Path $mine -Pattern (& $pattern $b) | ForEach-Object {
            $f = [int]$_.Matches[0].Groups[1].Value
            if (-not $want.ContainsKey($f)) { return }
            if ($want[$f] -eq $_.Matches[0].Groups[2].Value) { $same++ } else { $differ++; if ($null -eq $first) { $first = "board $b frame $f" } }
        }
    }
    $from = Select-String -Path $mine -Pattern "^board 0 frame (\d+): RAM" | Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $to = Select-String -Path $mine -Pattern "^board 0 frame (\d+): RAM" | Select-Object -Last 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
    if ($differ -gt 0 -or $same -lt 1000) { "$name : FAILED, $same the same, $differ differ (first: $first), frames $from-$to"; $failed++; continue }
    "$name : all $same the same (the spectator's frames $from-$to)"
    if ($spectopts -contains '--net-one-board') { # (on one board: the other cabinet on side runs at some point, its backups used)
        $back = @(Select-String -Path $mine -Pattern 'handed back to side runs').Count
        $runs = @(Select-String -Path $mine -Pattern 'run on from its backup at frame' | ForEach-Object { $_.Line })
        "  handed back to side runs $back time(s); backups run on: $($runs.Count)"; $runs | Select-Object -First 4 | ForEach-Object { "  $_" }
        if ($back -eq 0) { "$name : FAILED, the other cabinet never went to side runs"; $failed++ }
    }
}
if ($failed) { exit 1 }
