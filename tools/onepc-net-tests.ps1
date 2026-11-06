# One board a PC over the network (gamerun --net-one-board; docs/netplay.md, "One board a PC over the network"):
# gamerun processes on this machine joined over localhost, each running its own board only from the hand-over
# on, the others made by its tools::remote::Cabinets from their players' inputs (and with --one-board-auto, run
# whole as shadows where side runs can't follow them). Each process's own board is compared, frame by frame,
# with a plain linked run of every board in one process (--boards N --parallel, the same presses and input
# delay), for as long as all of that run's boards ran (and a shadow in the process: it stops at its own block
# limit, the board then on alone). Exit code 1 if any differs.
#
#   tools/onepc-net-tests.ps1 [-Exe PATH] [-Only two,four,apart,coin1,coin0,poweron,late1,late0,fourauto,three,leave,leave3,vanish,vanish3]
#
# two: two seats from build/onepc/mash-5990.state (tools/onepc-tests.ps1 -Prepare makes it), both players
# mashing (tools/onepc-mash-presses.txt), handed over at 6,000. four: four seats from
# build/linkstudy/four2/four-8000.state, no presses, handed over at 8,100. With the rules (--one-board-auto):
# apart, from build/onepc/mash-10890.state through both time overs, the continues and a game over; coin1 and
# coin0, the slave's and the master's coin at 6,100 in the middle of the fight. Input delay 2 (the rules see the
# other cabinet's coin only that far ahead). poweron, as the window plays (--one-board-held): from power-on,
# handed over at 100, the other board whole until the rules find the game calm; player 1 in a game alone at
# 1,300, player 2 joining it at 2,700. late1 and late0: the same game, the slave's or the master's coin at 6,100,
# 1,480 frames after the hand-back: a shadow from the other machine's real board (Session::want_board). fourauto: four
# seats from four-8000.state, each other cabinet held until calm, seat 3's coin at 8,600; three: three seats from power-on,
# the third player joining at 4,600: the rules following each other cabinet. Outputs go to build/onepc/net. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md); it loads every core for several minutes.
param([string[]]$Only, [string]$Exe, [switch]$LeaverFirst) # (-Exe: another gamerun, e.g. build-jit/gamerun-jit.exe;
# -LeaverFirst: leave3/vanish3's leaver joins before player 2, so it has seat 2: the seats as a race of the joins may give them)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$exe = if ($Exe) { $Exe } elseif (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
$out = 'build/onepc/net'
New-Item -ItemType Directory -Force $out | Out-Null
$mash = (Get-Content -Raw tools/onepc-mash-presses.txt).Trim() -split '\s+'
$auto = @('--one-board-auto')
# (from power-on, as the window plays: the link's roles set, player 1 in a game alone, player 2 joining it)
$poweron = @('--poke', '0:10019E=1@0:2000', '--poke', '1:10019E=2@0:2000', '--press', '0:coin@1200', '--press', '0:start@1300',
    '--press', '0:beat@1700', '--press', '1:coin@2600', '--press', '1:start@2700', '--press', '1:beat@3300', '--press', '1:beat@3650')
# (three cabinets from power-on: the third a slave too, its player joining at 4,600)
$three = $poweron + @('--poke', '2:10019E=2@0:3000', '--press', '2:coin@4600', '--press', '2:start@4700', '--press', '2:beat@5300',
    '--press', '2:beat@5650')

# Name, seats, state, blocks, hand-over frame, options of every run (presses), options of the network runs.
$tests = @(
    @('two',   2, 'build/onepc/mash-5990.state',           1400000000, 6000,  $mash, @()),
    @('four',  4, 'build/linkstudy/four2/four-8000.state', 700000000,  8100,  @(), @()),
    @('apart', 2, 'build/onepc/mash-10890.state',          1400000000, 10900, $mash, $auto),
    @('coin1', 2, 'build/onepc/mash-5990.state',           700000000,  6000,  ($mash + @('--press', '1:coin@6100')), $auto),
    @('coin0', 2, 'build/onepc/mash-5990.state',           700000000,  6000,  ($mash + @('--press', '0:coin@6100')), $auto),
    @('poweron', 2, '',                                    5000000000, 100,   $poweron, ($auto + @('--one-board-held'))),
    @('late1', 2, '',                                      5000000000, 100,   ($poweron + @('--press', '1:coin@6100')), ($auto + @('--one-board-held'))),
    @('late0', 2, '',                                      5000000000, 100,   ($poweron + @('--press', '0:coin@6100')), ($auto + @('--one-board-held'))),
    @('fourauto', 4, 'build/linkstudy/four2/four-8000.state', 1100000000, 8100, @('--press', '2:coin@8600'), ($auto + @('--one-board-held'))),
    @('three', 3, '',                                      5000000000, 100,   $three, ($auto + @('--one-board-held')))
)
$pattern = { param($b) "^board $b frame (\d+): RAM (\w+)" }
$last = { param($file, $b) Select-String -Path $file -Pattern (& $pattern $b) | Select-Object -Last 1 | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } }
$failed = 0
foreach ($t in $tests) {
    $name, $seats, $state, $blocks, $from, $every, $net = $t
    if ($Only -and $Only -notcontains $name) { continue }
    if ($state -and -not (Test-Path $state)) { "$name : no $state, skipped"; continue }
    $from_state = if ($state) { @('--load-state', $state) } else { @() } # (none: from power-on)
    $common = @('build/roms/spikeofe', '--no-sound', '--no-2d') + $from_state + @('--blocks', "$blocks", '--input-delay', '2') + $every
    $game = "onepc-$name-$PID"
    $procs = @(Start-Process $exe -ArgumentList ($common + @('--boards', "$seats", '--parallel', '--one-board', '1@99999999')) -RedirectStandardOutput "$out/$name-ref.txt" -NoNewWindow -PassThru)
    $procs += Start-Process $exe -ArgumentList ($common + $net + @('--net-host', $game, '--seats', "$seats", '--net-one-board', "$from")) -RedirectStandardOutput "$out/$name-0.txt" -RedirectStandardError "$out/$name-0.err" -NoNewWindow -PassThru
    Start-Sleep -Milliseconds 500
    for ($k = 1; $k -lt $seats; $k++) { # (each joins the next free seat: seat k + 1, board k)
        $procs += Start-Process $exe -ArgumentList ($common + $net + @('--net-join', $game, '--net-one-board', "$from")) -RedirectStandardOutput "$out/$name-$k.txt" -RedirectStandardError "$out/$name-$k.err" -NoNewWindow -PassThru
        Start-Sleep -Milliseconds 500
    }
    $procs | ForEach-Object { $_.WaitForExit() }

    # (the reference's hashes, by board and frame; compared up to where its first board stopped)
    $ref = @{}
    $refEnd = [int]::MaxValue
    foreach ($b in 0..($seats - 1)) {
        $ref[$b] = @{}
        Select-String -Path "$out/$name-ref.txt" -Pattern (& $pattern $b) | ForEach-Object { $ref[$b][[int]$_.Matches[0].Groups[1].Value] = $_.Matches[0].Groups[2].Value }
        $refEnd = [Math]::Min($refEnd, ($ref[$b].Keys | Measure-Object -Maximum).Maximum)
    }
    foreach ($k in 0..($seats - 1)) {
        $file = "$out/$name-$k.txt"
        $seat = Select-String -Path $file -Pattern 'playing seat (\d)' | Select-Object -First 1 | ForEach-Object { [int]$_.Matches[0].Groups[1].Value - 1 }
        if ($null -eq $seat) { "$name process $k : didn't play (see $file, $out/$name-$k.err)"; $failed++; continue }
        $end = $refEnd
        foreach ($b in 0..($seats - 1)) { # (a shadow here, stopped at its block limit before this board: up to there)
            if ($b -eq $seat) { continue }
            $event = Select-String -Path $file -Pattern "^board ${b}: (its shadow|the master's shadow)" | Select-Object -Last 1
            if (-not $event -or $event.Line -match 'handed back') { continue } # (no shadow at the end: side runs to the end)
            $l = & $last $file $b
            if ($l -and $l -gt $from + 5) { $end = [Math]::Min($end, $l - 2) }
        }
        $n = 0; $same = 0; $first = $null; $after = 0
        Select-String -Path $file -Pattern (& $pattern $seat) | ForEach-Object {
            $f = [int]$_.Matches[0].Groups[1].Value
            if ($f -gt $end -or -not $ref[$seat].ContainsKey($f)) { return }
            $n++
            if ($f -gt $from) { $after++ }
            if ($ref[$seat][$f] -eq $_.Matches[0].Groups[2].Value) { $same++ } elseif ($null -eq $first) { $first = $f }
        }
        $shadows = (Select-String -Path $file -Pattern "its shadow from|the master's shadow from frame").Count
        $notes = (@("$shadows shadow(s)") + (Select-String -Path $file -Pattern 'first failure|hash checks differed' | ForEach-Object { $_.Line })) -join ' / '
        $ok = $n -gt 0 -and $same -eq $n -and $after -gt 0
        if (-not $ok) { $failed++ }
        '{0,-5} board {1}: {2} of {3} frames the same ({4} after the hand-over){5}  {6}' -f $name, $seat, $same, $n, $after,
            $(if ($first) { ", first difference at $first" } else { '' }), $notes
    }
}
# leave: the poweron game; player 2 leaves after 5,030 (--net-leave-at 5000: its board's state sent to the host, which
# runs that cabinet on whole from it), then a new player joins in progress (every board's state from the host) and
# takes the seat. leave3: the three game, player 3 leaving after 7,030; the new player's join then finds player 2's
# board on side runs on the host, which asks player 2's machine for it first (every board whole for the state). Each
# machine's hashes (its own board and any it runs whole) are compared with the host's, and the joiner's (every board
# run there, Mode B) with every other machine's, frame by frame and the whole states every 100 frames. (The host and
# the others run 10,000 frames past the end: alone, gamerun runs far faster than the game, and a join takes a second or
# two to arrive.)
# vanish, vanish3: the same, the player gone without a word instead (--net-vanish-at: as a crash): each other machine
# runs its board on from its last backup (tools::backup); its RAM each frame there (replay board N) compared with the
# board's own up to where it went.
$hashes = { param($file) $t = @{}; Select-String -Path $file -Pattern '^(?:replay )?board (\d) frame (\d+): (RAM|whole state) (\w+)' | ForEach-Object { $g = $_.Matches[0].Groups; $t["$($g[1].Value):$($g[2].Value):$($g[3].Value)"] = $g[4].Value }; $t }
$leaves = @(
    @('leave', 2, $poweron, 5000, 8000, 'leave'),
    @('leave3', 3, $three, 7000, 10000, 'leave'),
    @('vanish', 2, $poweron, 5000, 8000, 'vanish'),
    @('vanish3', 3, $three, 7000, 10000, 'vanish')
)
foreach ($t in $leaves) {
    $name, $seats, $presses, $leave, $end, $how = $t
    if ($Only -and $Only -notcontains $name) { continue }
    $game = "onepc-$name-$PID"
    $common = @('build/roms/spikeofe', '--no-sound', '--no-2d', '--input-delay', '2', '--blocks', '30000000000') + $presses
    $net = $auto + @('--one-board-held', '--net-one-board', '100')
    $stay = @(Start-Process $exe -ArgumentList ($common + $net + @('--frames', "$($end + 10000)", '--net-host', $game, '--seats', "$seats")) -RedirectStandardOutput "$out/$name-0.txt" -RedirectStandardError "$out/$name-0.err" -NoNewWindow -PassThru)
    Start-Sleep -Milliseconds 500
    $leaver = $seats - 1
    $startLeaver = { Start-Process $exe -ArgumentList ($common + $net + @('--net-join', $game, "--net-$how-at", "$leave")) -RedirectStandardOutput "$out/$name-$leaver.txt" -RedirectStandardError "$out/$name-$leaver.err" -NoNewWindow -PassThru }
    if ($LeaverFirst -and $seats -gt 2) { $l = & $startLeaver; Start-Sleep -Milliseconds 1500 }
    for ($k = 1; $k -lt $seats - 1; $k++) {
        $stay += Start-Process $exe -ArgumentList ($common + $net + @('--frames', "$($end + 10000)", '--net-join', $game)) -RedirectStandardOutput "$out/$name-$k.txt" -RedirectStandardError "$out/$name-$k.err" -NoNewWindow -PassThru
        Start-Sleep -Milliseconds 500
    }
    if (-not ($LeaverFirst -and $seats -gt 2)) { $l = & $startLeaver }
    $l.WaitForExit()
    if ($how -eq 'vanish') { Start-Sleep -Seconds 6 } # (gone without a word: the others drop it after 3 seconds)
    $joiner = $seats
    $j = Start-Process $exe -ArgumentList @('build/roms/spikeofe', '--no-sound', '--no-2d', '--input-delay', '2', '--blocks', '30000000000', '--frames', "$end", '--frame-hashes', '--net-join', $game) -RedirectStandardOutput "$out/$name-$joiner.txt" -RedirectStandardError "$out/$name-$joiner.err" -NoNewWindow -PassThru
    $j.WaitForExit(); $stay | ForEach-Object { $_.WaitForExit() }
    $all = @{}
    foreach ($k in 0..$joiner) { $all[$k] = & $hashes "$out/$name-$k.txt" }
    $pairs = @(1..$joiner | ForEach-Object { , @(0, $_) }) + @(if ($seats -gt 2) { 1..($seats - 2) | ForEach-Object { , @($_, $joiner) } })
    foreach ($p in $pairs) {
        $a, $b = $p
        $n = 0; $same = 0
        foreach ($key in $all[$b].Keys) { if ($all[$a].ContainsKey($key)) { $n++; if ($all[$a][$key] -eq $all[$b][$key]) { $same++ } } }
        $label = { param($k) if ($k -eq 0) { 'the host' } elseif ($k -eq $leaver) { 'the leaver' } elseif ($k -eq $joiner) { 'the joiner' } else { "player $($k + 1)" } }
        $ok = $n -gt 100 -and $same -eq $n
        if (-not $ok) { $failed++ }
        '{0,-6} {1} and {2}: {3} of {4} hashes the same' -f $name, (& $label $a), (& $label $b), $same, $n
    }
    if ($how -eq 'vanish') { # (each other machine's replay of the board, where the board itself ran: the same)
        $leaverBoard = if ($LeaverFirst -and $seats -gt 2) { 1 } else { $leaver } # (-LeaverFirst: its board, seat 2)
        for ($k = 0; $k -lt $leaver; $k++) {
            $r = @(Select-String -Path "$out/$name-$k.txt" -Pattern "^replay board $leaverBoard frame").Count
            if (-not $r) { $failed++; "$name : machine $k didn't run board $leaverBoard on from its backup" }
        }
    }
    $notes = @(Select-String -Path "$out/$name-0.txt" -Pattern 'player gone|run on from its backup|leaving after frame|its shadow from its machine''s state|every board whole here|every board''s state at frame|taken from frame|hash checks differed' | ForEach-Object { $_.Line })
    if (-not (Select-String -Quiet -Path "$out/$name-$joiner.txt" -Pattern 'running on from the host')) { $failed++; "$name : the joiner didn't run on from the host's state" }
    $notes | ForEach-Object { "  $_" }
}
if ($failed) { "$failed failed"; exit 1 }
'all the same'
