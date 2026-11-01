# One board a PC over the network (gamerun --net-one-board; docs/netplay.md, "One board a PC over the network"):
# gamerun processes on this machine joined over localhost, each running its own board only from the hand-over
# on, the others made by its tools::remote::Cabinets from their players' inputs (and with --one-board-auto, run
# whole as shadows where side runs can't follow them). Each process's own board is compared, frame by frame,
# with a plain linked run of every board in one process (--boards N --parallel, the same presses and input
# delay), for as long as all of that run's boards ran (and a shadow in the process: it stops at its own block
# limit, the board then on alone). Exit code 1 if any differs.
#
#   tools/onepc-net-tests.ps1 [-Only two,four,apart,coin1,coin0,poweron]
#
# two: two seats from build/onepc/mash-5990.state (tools/onepc-tests.ps1 -Prepare makes it), both players
# mashing (tools/onepc-mash-presses.txt), handed over at 6,000. four: four seats from
# build/linkstudy/four2/four-8000.state, no presses, handed over at 8,100. With the rules (--one-board-auto):
# apart, from build/onepc/mash-10890.state through both time overs, the continues and a game over; coin1 and
# coin0, the slave's and the master's coin at 6,100 in the middle of the fight. Input delay 2 (the rules see the
# other cabinet's coin only that far ahead). poweron, as the window plays (--one-board-held): from power-on,
# handed over at 100, the other board whole until the rules find the game calm; player 1 in a game alone at
# 1,300, player 2 joining it at 2,700. Outputs go to build/onepc/net. Run from PowerShell with the
# toolchain's PATH (CLAUDE.md); it loads every core for several minutes.
param([string[]]$Only)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$exe = if (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
$out = 'build/onepc/net'
New-Item -ItemType Directory -Force $out | Out-Null
$mash = (Get-Content -Raw tools/onepc-mash-presses.txt).Trim() -split '\s+'
$auto = @('--one-board-auto')
# (from power-on, as the window plays: the link's roles set, player 1 in a game alone, player 2 joining it)
$poweron = @('--poke', '0:10019E=1@0:2000', '--poke', '1:10019E=2@0:2000', '--press', '0:coin@1200', '--press', '0:start@1300',
    '--press', '0:beat@1700', '--press', '1:coin@2600', '--press', '1:start@2700', '--press', '1:beat@3300', '--press', '1:beat@3650')

# Name, seats, state, blocks, hand-over frame, options of every run (presses), options of the network runs.
$tests = @(
    @('two',   2, 'build/onepc/mash-5990.state',           1400000000, 6000,  $mash, @()),
    @('four',  4, 'build/linkstudy/four2/four-8000.state', 700000000,  8100,  @(), @()),
    @('apart', 2, 'build/onepc/mash-10890.state',          1400000000, 10900, $mash, $auto),
    @('coin1', 2, 'build/onepc/mash-5990.state',           700000000,  6000,  ($mash + @('--press', '1:coin@6100')), $auto),
    @('coin0', 2, 'build/onepc/mash-5990.state',           700000000,  6000,  ($mash + @('--press', '0:coin@6100')), $auto),
    @('poweron', 2, '',                                    5000000000, 100,   $poweron, ($auto + @('--one-board-held')))
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
if ($failed) { "$failed failed"; exit 1 }
'all the same'
