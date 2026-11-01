# One board a PC over the network (gamerun --net-one-board; docs/netplay.md, "One board a PC over the network"):
# gamerun processes on this machine joined over localhost, each running its own board only from the hand-over
# on, the others made by its tools::remote::Cabinets from their players' inputs. Each process's own board is
# compared, frame by frame, with a plain linked run of every board in one process (--boards N --parallel, the
# same presses and input delay), for as long as all of that run's boards ran. Exit code 1 if any differs.
#
#   tools/onepc-net-tests.ps1 [-Only two,four]
#
# two: two seats from build/onepc/mash-5990.state (tools/onepc-tests.ps1 -Prepare makes it), both players
# mashing (tools/onepc-mash-presses.txt), handed over at 6,000. four: four seats from
# build/linkstudy/four2/four-8000.state, no presses, handed over at 8,100. Input delay 2. Outputs go to
# build/onepc/net. Run from PowerShell with the toolchain's PATH (CLAUDE.md); it loads every core for a minute or two.
param([string[]]$Only)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root
$exe = if (Test-Path build/gamerun.exe) { 'build/gamerun.exe' } else { 'build/gamerun' }
$out = 'build/onepc/net'
New-Item -ItemType Directory -Force $out | Out-Null
$mash = (Get-Content -Raw tools/onepc-mash-presses.txt).Trim() -split '\s+'

# Name, seats, state, blocks, hand-over frame, presses.
$tests = @(
    @('two',  2, 'build/onepc/mash-5990.state',            1400000000, 6000, $mash),
    @('four', 4, 'build/linkstudy/four2/four-8000.state',  700000000,  8100, @())
)
$pattern = { param($b) "^board $b frame (\d+): RAM (\w+)" }
$failed = 0
foreach ($t in $tests) {
    $name, $seats, $state, $blocks, $from, $presses = $t
    if ($Only -and $Only -notcontains $name) { continue }
    if (-not (Test-Path $state)) { "$name : no $state, skipped"; continue }
    $common = @('build/roms/spikeofe', '--no-sound', '--no-2d', '--load-state', $state, '--blocks', "$blocks", '--input-delay', '2') + $presses
    $game = "onepc-$name-$PID"
    $procs = @(Start-Process $exe -ArgumentList ($common + @('--boards', "$seats", '--parallel', '--one-board', '1@99999999')) -RedirectStandardOutput "$out/$name-ref.txt" -NoNewWindow -PassThru)
    $procs += Start-Process $exe -ArgumentList ($common + @('--net-host', $game, '--seats', "$seats", '--net-one-board', "$from")) -RedirectStandardOutput "$out/$name-0.txt" -RedirectStandardError "$out/$name-0.err" -NoNewWindow -PassThru
    Start-Sleep -Milliseconds 500
    for ($k = 1; $k -lt $seats; $k++) { # (each joins the next free seat: seat k + 1, board k)
        $procs += Start-Process $exe -ArgumentList ($common + @('--net-join', $game, '--net-one-board', "$from")) -RedirectStandardOutput "$out/$name-$k.txt" -RedirectStandardError "$out/$name-$k.err" -NoNewWindow -PassThru
        Start-Sleep -Milliseconds 500
    }
    $procs | ForEach-Object { $_.WaitForExit() }

    # (the reference's hashes, by board and frame; compared up to where its first board stopped)
    $ref = @{}
    $end = [int]::MaxValue
    foreach ($b in 0..($seats - 1)) {
        $ref[$b] = @{}
        Select-String -Path "$out/$name-ref.txt" -Pattern (& $pattern $b) | ForEach-Object { $ref[$b][[int]$_.Matches[0].Groups[1].Value] = $_.Matches[0].Groups[2].Value }
        $end = [Math]::Min($end, ($ref[$b].Keys | Measure-Object -Maximum).Maximum)
    }
    foreach ($k in 0..($seats - 1)) {
        $file = "$out/$name-$k.txt"
        $seat = Select-String -Path $file -Pattern 'playing seat (\d)' | Select-Object -First 1 | ForEach-Object { [int]$_.Matches[0].Groups[1].Value - 1 }
        if ($null -eq $seat) { "$name process $k : didn't play (see $file, $out/$name-$k.err)"; $failed++; continue }
        $n = 0; $same = 0; $first = $null; $after = 0
        Select-String -Path $file -Pattern (& $pattern $seat) | ForEach-Object {
            $f = [int]$_.Matches[0].Groups[1].Value
            if ($f -gt $end -or -not $ref[$seat].ContainsKey($f)) { return }
            $n++
            if ($f -gt $from) { $after++ }
            if ($ref[$seat][$f] -eq $_.Matches[0].Groups[2].Value) { $same++ } elseif ($null -eq $first) { $first = $f }
        }
        $notes = (Select-String -Path $file -Pattern 'first failure|hash checks differed' | ForEach-Object { $_.Line }) -join ' / '
        $ok = $n -gt 0 -and $same -eq $n -and $after -gt 0
        if (-not $ok) { $failed++ }
        '{0,-5} board {1}: {2} of {3} frames the same ({4} after the hand-over){5}  {6}' -f $name, $seat, $same, $n, $after,
            $(if ($first) { ", first difference at $first" } else { '' }), $notes
    }
}
if ($failed) { "$failed failed"; exit 1 }
'all the same'
