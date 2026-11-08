# Repeat tools/window-autoplay-test.ps1 with seeds until a run goes out of step (per-frame hashes from -HashFrom kept
# for it in build-jit\shots\auto-<seed>); stops at the first.
param([int[]]$Seeds = @(2, 3, 4, 5, 6), [int]$Frames = 6000, [int]$HashFrom = 4000)
$root = Split-Path (Split-Path $PSScriptRoot)
foreach ($s in $Seeds) {
    $out = & "$root\tools\window-autoplay-test.ps1" -Frames $Frames -Seed $s -HashFrom $HashFrom -Tag "auto-$s"
    $out | ForEach-Object { "seed ${s}: $_" }
    if (($out -join "`n") -match '[1-9]\d* differs') { "OUT OF STEP with seed $s"; exit 1 }
    if (($out -join "`n") -match 'STOPPED') { "STOPPED with seed $s"; exit 3 }
}
'all in step'
