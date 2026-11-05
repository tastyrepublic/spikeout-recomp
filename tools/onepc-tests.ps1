# One board a PC (tools/remote_cabinet.hpp, gamerun --one-board): short runs from two saved two-board states of
# the mashing fight (both players pressing every button, tools/onepc-mash-presses.txt; docs/netplay.md, "One
# board a PC"), in parallel, no 2D or sound. Each run's staying board is compared, frame by frame, with the
# two-board run's RAM hashes, for as long as both boards ran; its notes (shadows, hand-backs, side runs that
# failed) shown beside.
#
#   tools/onepc-tests.ps1 [-Only NAME,NAME] [-Prepare] [-Exe PATH]   (-Exe: another gamerun, e.g. build-jit/gamerun-jit.exe)
#
# Needs build/onepc/fight-4700.state (the two-board fight, saved with --save-state at frame 4700) and the
# ROMs in build/roms/spikeofe. -Prepare makes, from it, the reference (build/onepc/mash.txt) and the two
# states the runs start from (mash-5990.state, mash-10890.state); once, or after a change that moves the game.
# Outputs go to build/onepc/tests. Run from PowerShell with the toolchain's PATH (CLAUDE.md).
param([string[]]$Only, [switch]$Prepare, [string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$exe = if ($Exe) { $Exe } elseif (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
$roms = 'build/roms/spikeofe'
$dir = 'build/onepc'
$out = "$dir/tests"
New-Item -ItemType Directory -Force $out | Out-Null
$presses = (Get-Content -Raw tools/onepc-mash-presses.txt).Trim() -split '\s+'
$common = @('--boards', '2', '--no-sound', '--no-2d')

if ($Prepare) {
    # (--one-board with a frame never reached: its hooks only, and every frame's RAM hash printed)
    $never = @('--one-board', '1@99999999')
    $jobs = @(
        @("$dir/mash.txt", (@('--blocks', '7000000000') + $never)),
        @("$out/save-5990.txt", @('--blocks', '7000000000', '--frames', '5991', '--save-state', "$dir/mash-5990.state@5990")),
        @("$out/save-10890.txt", @('--blocks', '7000000000', '--frames', '10891', '--save-state', "$dir/mash-10890.state@10890"))
    )
    $running = foreach ($j in $jobs) {
        Start-Job -ArgumentList $root, $exe, $roms, ($common + @('--load-state', "$dir/fight-4700.state") + $presses + $j[1]), $j[0] {
            param($root, $exe, $roms, $opts, $file)
            Set-Location $root
            & $exe $roms @opts > $file
        }
    }
    Wait-Job $running | Out-Null
    Get-ChildItem "$dir/mash-5990.state", "$dir/mash-10890.state", "$dir/mash.txt" | ForEach-Object { "$($_.Name): $($_.Length) bytes" }
    return
}

# Name, state, blocks, staying board, options. (Board 0 is the link's master; a run whose board 1 goes has
# the master staying, the slave made on the side.)
$tests = @(
    @('slave-tasks',     'mash-5990',  350000000,  0, @('--one-board', '1@6000', '--one-board-camera', "$dir/cam3.rec", '--one-board-tasks')),
    @('master-tasks',    'mash-5990',  350000000,  1, @('--one-board', '0@6000', '--one-board-tasks')),
    @('slave-shadow',    'mash-5990',  350000000,  0, @('--one-board', '1@6000', '--one-board-camera', "$dir/cam3.rec", '--one-board-tasks', '--one-board-shadow', '6100')),
    @('master-shadow',   'mash-5990',  350000000,  1, @('--one-board', '0@6000', '--one-board-tasks', '--one-board-shadow', '6100')),
    @('slave-handback',  'mash-5990',  350000000,  0, @('--one-board', '1@6000', '--one-board-camera', "$dir/cam3.rec", '--one-board-tasks', '--one-board-shadow', '6100', '--one-board-hand-back', '6250')),
    @('master-handback', 'mash-5990',  350000000,  1, @('--one-board', '0@6000', '--one-board-tasks', '--one-board-shadow', '6100', '--one-board-hand-back', '6250')),
    @('slave-apart',     'mash-10890', 1400000000, 0, @('--one-board', '1@10900', '--one-board-camera', "$dir/cam3.rec", '--one-board-tasks', '--one-board-shadow', '10910')),
    @('slave-nocam',     'mash-5990',  350000000,  0, @('--one-board', '1@6000', '--one-board-tasks')),
    @('slave-auto',      'mash-5990',  1400000000, 0, @('--one-board', '1@6000', '--one-board-auto')),
    @('master-auto',     'mash-5990',  1400000000, 1, @('--one-board', '0@6000', '--one-board-auto')),
    @('slave-auto2',     'mash-10890', 1400000000, 0, @('--one-board', '1@10900', '--one-board-auto')),
    @('master-auto2',    'mash-10890', 1400000000, 1, @('--one-board', '0@10900', '--one-board-auto')),
    # (the leaving cabinet's coin mid-fight: a shadow wanted for 60 frames before it, handed back 300 calm frames after;
    # these with presses of their own have a two-board run of their own beside them, the reference)
    @('slave-midgame',   'mash-5990',  700000000,  0, @('--one-board', '1@6000', '--one-board-auto'), @('--press', '1:coin@6100')),
    @('master-midgame',  'mash-5990',  700000000,  1, @('--one-board', '0@6000', '--one-board-auto'), @('--press', '0:coin@6100')),
    # (four players in one game (build/linkstudy/four2/four-8000.state, docs/netplay.md), no presses: seats 2, 3 and 4
    # made by the master; one four-board run beside them, the reference)
    @('four-seat2',      'four',       1400000000, 0, @('--one-board', '1@8100', '--one-board-tasks')),
    @('four-seat3',      'four',       1400000000, 0, @('--one-board', '2@8100', '--one-board-tasks')),
    @('four-seat4',      'four',       1400000000, 0, @('--one-board', '3@8100', '--one-board-tasks')),
    @('four-seat4-auto', 'four',       1400000000, 0, @('--one-board', '3@8100', '--one-board-auto'))
)
$have = Test-Path "$dir/cam3.rec"
$four = 'build/linkstudy/four2/four-8000.state'
$run = {
    param($root, $exe, $roms, $opts, $file)
    Set-Location $root
    (Measure-Command { & $exe $roms @opts > $file }).TotalSeconds
}
$running = @()
$fourRef = $null
foreach ($t in $tests) {
    if ($Only -and $Only -notcontains $t[0]) { continue }
    if (-not $have -and $t[4] -contains '--one-board-camera') { continue } # (its camera recording: an older test's)
    if ($t[1] -eq 'four') {
        if (-not (Test-Path $four)) { continue }
        $base = @('--boards', '4', '--no-sound', '--no-2d', '--load-state', $four, '--blocks', $t[2])
        if (-not $fourRef) { $fourRef = Start-Job -ArgumentList $root, $exe, $roms, ($base + @('--one-board', '3@99999999')), "$out/four-ref.txt" $run }
        $job = Start-Job -ArgumentList $root, $exe, $roms, ($base + $t[4]), "$out/$($t[0]).txt" $run
        $running += [pscustomobject]@{ Test = $t; Job = $job; Ref = $fourRef; RefFile = "$out/four-ref.txt" }
        continue
    }
    $extra = if ($t.Count -gt 5) { $t[5] } else { @() }
    $base = $common + @('--load-state', "$dir/$($t[1]).state", '--blocks', $t[2]) + $presses + $extra
    $job = Start-Job -ArgumentList $root, $exe, $roms, ($base + $t[4]), "$out/$($t[0]).txt" $run
    $ref = if ($extra) { Start-Job -ArgumentList $root, $exe, $roms, ($base + @('--one-board', '1@99999999')), "$out/$($t[0])-ref.txt" $run }
    $running += [pscustomobject]@{ Test = $t; Job = $job; Ref = $ref; RefFile = $(if ($ref) { "$out/$($t[0])-ref.txt" } else { "$dir/mash.txt" }) }
}
Wait-Job ($running.Job + @($running.Ref | Where-Object { $_ } | Select-Object -Unique)) | Out-Null

# Board B's frame hashes of the reference and a run, by frame: how many the run has that the reference has too,
# how many are the same, the last before the first that differs.
$pattern = { param($b) "^board $b frame (\d+): RAM (\w+)" }
$hashes = { param($path)
    $h = @{ 0 = @{}; 1 = @{} }
    foreach ($b in 0, 1) {
        Select-String -Path $path -Pattern (& $pattern $b) | ForEach-Object { $h[$b][[int]$_.Matches[0].Groups[1].Value] = $_.Matches[0].Groups[2].Value }
    }
    $h
}
$references = @{}
$failed = 0
foreach ($r in $running) {
    $name, $stay = $r.Test[0], $r.Test[3]
    $file = "$out/$name.txt"
    $secs = [math]::Round((Receive-Job $r.Job), 0)
    if (-not $references.ContainsKey($r.RefFile)) {
        # (and where the reference's own boards stopped: from the first of them to stop, the others went on without it)
        $last = foreach ($b in 0..3) { Select-String -Path $r.RefFile -Pattern (& $pattern $b) | Select-Object -Last 1 | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } }
        $references[$r.RefFile] = @((& $hashes $r.RefFile), ($last | Measure-Object -Minimum).Minimum)
    }
    $reference, $refEnd = $references[$r.RefFile]
    $frames = { param($b) Select-String -Path $file -Pattern (& $pattern $b) | ForEach-Object { , @([int]$_.Matches[0].Groups[1].Value, $_.Matches[0].Groups[2].Value) } }
    $mine = & $frames $stay
    $spec = ($r.Test[4][[array]::IndexOf($r.Test[4], '--one-board') + 1]) -split '@'
    $leaveLast = (& $frames ([int]$spec[0]) | Select-Object -Last 1)[0]
    # (up to the last frame both boards ran: a shadow at its block limit before the other, the other on alone)
    $upto = [math]::Min($mine[-1][0], $refEnd + 1)
    if ($r.Test[1] -ne 'four' -and (Select-String -Path $file -Pattern 'Stopped: block limit reached').Count -ge 2) { $upto = [math]::Min($upto, $leaveLast) - 2 }
    if ($r.Test[1] -eq 'four' -and $leaveLast -gt [int]$spec[1] + 5) { $upto = [math]::Min($upto, $leaveLast) - 2 }
    if ($r.Test[4] -contains '--one-board-shadow' -and $r.Test[4] -notcontains '--one-board-hand-back') { $upto = [math]::Min($upto, $leaveLast) - 2 }
    $both = 0; $same = 0; $first = $null
    foreach ($f in $mine) {
        if ($f[0] -gt $upto -or -not $reference[$stay].ContainsKey($f[0])) { continue }
        $both++
        if ($reference[$stay][$f[0]] -eq $f[1]) { $same++ } elseif (-not $first) { $first = $f[0] }
    }
    if ($first -or $both -eq 0) { $failed++ }
    $verdict = if ($first) { "DIFFERS from frame $first" } elseif ($both) { 'same' } else { 'nothing compared' }
    $notes = (Select-String -Path $file -Pattern 'shadow from|handed back|first failure|heavy frames|no shadow state|too late' |
              Select-Object -First 4 | ForEach-Object { $_.Line -replace '^board \d+: ', '' }) -join ' / '
    '{0,-16} {1,4}s  board {2}: {3} of {4} frames the same, {5}.  {6}' -f $name, $secs, $stay, $same, $both, $verdict, $notes
}
if ($failed) { "$failed run(s) not the same"; exit 1 }
