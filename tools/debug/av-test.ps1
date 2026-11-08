# One board with the simulated late network, 2D and sound on (as the window), against every board whole with them on.
param([int]$Stays = 0, [int]$Latency = 6, [int]$Frames = 7000, [switch]$Lazy)
Set-Location Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31
$d = "build\av-test-$Stays-$Latency"; New-Item -ItemType Directory -Force $d | Out-Null
$common = @('Z:\Emu\spikeout-recomp\build\roms\spikeofe', '--boards', '3', '--parallel', '--load-state', 'build-jit\bench\three-6000.state',
            '--frames', "$Frames", '--blocks', '900000000000', '--no-frame-hashes', '--bot', '1', '--ram-hashes', "$Stays@6010")
function run($tag, [string[]]$more) {
    $p = Start-Process -PassThru -NoNewWindow -FilePath build\gamerun.exe -ArgumentList ($common + @("$d\$tag.hashes") + $more) `
        -RedirectStandardOutput "$d\$tag.txt" -RedirectStandardError "$d\$tag.err"
    $null = $p.Handle
    if (-not $p.WaitForExit(1200000)) { Stop-Process -Id $p.Id -Force; "STOPPED: $tag over 20 minutes"; exit 3 }
    "$tag exit $($p.ExitCode)"
}
if (-not (Test-Path "$d\ref.hashes")) { run 'ref' @() }
$made = @(0..2 | Where-Object { $_ -ne $Stays } | ForEach-Object { @('--one-board', "$_@6010") })
$env:ONEBOARD_NO_SHADOWS = '1'; if ($Lazy) { $env:ROLLBACK_SIM_LAZY = '1' }
run 'ob' ($made + @('--one-board-stays', "$Stays", '--one-board-tasks', '--rollback-sim', "$Latency"))
Remove-Item Env:ONEBOARD_NO_SHADOWS, Env:ROLLBACK_SIM_LAZY -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'hashcmp.ps1') (Resolve-Path "$d\ref.hashes").Path (Resolve-Path "$d\ob.hashes").Path -Around 2
