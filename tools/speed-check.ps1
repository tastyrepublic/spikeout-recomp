# How smoothly a run went, from its log's speed lines (tools/speed_report.hpp: the window with SPIKEOUT_NET_DEBUG or
# Detailed logging, gamerun --pace with SPIKEOUT_NET_DEBUG): each second's frames and stutters, and the run's totals.
# A second under -Min frames (56.5: the arcade's 57.52 less the measure's own wobble), or with a stutter (a frame over
# 1.5 frames after the last), after the first -Skip seconds (the start, the first loads), makes the run SLOW: said, each
# slow second with its frames (to match them to the log's events: a join, a coin), and exit code 1.
#
#   tools/speed-check.ps1 FILE[,FILE...] [-Skip 5] [-Min 56.5] [-Quiet]   (several: a comma between them, or an array)
param([Parameter(Mandatory, Position = 0)][string[]]$Files, [int]$Skip = 5, [double]$Min = 56.5, [switch]$Quiet)
$slow = $false
foreach ($file in $Files) {
    $lines = @(Get-Content $file | Select-String -Pattern 'speed: frames (\d+)-(\d+): ([\d.]+) a second, (\d+) stutters, the longest gap (\d+) ms')
    $total = Get-Content $file | Select-String -Pattern 'speed: in all' | Select-Object -Last 1
    if ($lines.Count -eq 0) { "${file}: no speed lines (SPIKEOUT_NET_DEBUG, and --pace for gamerun?)"; continue }
    $bad = @()
    for ($i = $Skip; $i -lt $lines.Count; $i++) {
        $g = $lines[$i].Matches[0].Groups
        $rate = [double]$g[3].Value; $stutters = [int]$g[4].Value; $gap = [int]$g[5].Value
        if ($rate -lt $Min -or $stutters -gt 0) { $bad += "  frames $($g[1].Value)-$($g[2].Value): $rate a second, $stutters stutters, the longest gap $gap ms" }
    }
    $rates = @($lines | Select-Object -Skip $Skip | ForEach-Object { [double]$_.Matches[0].Groups[3].Value })
    $lowest = if ($rates.Count) { ($rates | Measure-Object -Minimum).Minimum } else { 0 }
    $verdict = if ($bad.Count) { 'SLOW' } else { 'smooth' }
    "${file}: $verdict ($($lines.Count) seconds, the first $Skip skipped; the slowest second $lowest a second; $($bad.Count) slow seconds)"
    if ($total) { "  $($total.Line -replace '^.*speed: ', '')" }
    if ($bad.Count) {
        $slow = $true
        if (-not $Quiet) { $bad | Select-Object -First 20; if ($bad.Count -gt 20) { "  ... $($bad.Count - 20) more" } }
    }
}
if ($slow) { exit 1 }
