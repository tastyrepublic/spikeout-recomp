# Our 3D against Supermodel's, scene by scene (docs/frame-dump.md): each 3D frame dump drawn by our renderer (frame3d)
# and by the patched Supermodel (tools\supermodel\build.ps1, draw.ps1); for each, ours | Supermodel's | where they
# differ (side by side, the differences x4), how much of the picture differs, and the models only one drew. A report
# in -Out\index.html. Dumps hold the ROM's models and textures: -Out stays outside the repository's tracked files.
#
#   tools\compare-supermodel.ps1 -Dumps A.m3d,B.m3d [-Out build\compare]
#   tools\compare-supermodel.ps1 -Scenes 'STATE@FRAME', 'STATE@FRAME@INPUTS' [-Out build\compare]
#     (a scene from one of our save states: gamerun runs it to FRAME (with a bot's recorded INPUTS, gamerun --inputs)
#      and writes its dump; Supermodel's own dumps: SUPERMODEL_DUMP_3D=1 and Alt+S in the patched Supermodel)
param([string[]]$Dumps = @(), [string[]]$Scenes = @(), [string]$Out = 'build\compare', [string]$Renderer = 'gpu',
      [string]$Roms = 'Z:\Emu\spikeout-recomp\build\roms\spikeofe', [string]$SupermodelRoms = 'Z:\Emu\Sega Supermodel 3\ROMs\spikeofe.zip',
      [string]$Work = 'Z:\Emu\supermodel-build', [int]$Threshold = 24)
$ErrorActionPreference = 'Continue' # (a scene that fails is skipped, the rest go on)
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
$root = Split-Path $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# Scenes from save states: their dumps first
$all = @($Dumps | ForEach-Object { (Resolve-Path $_).Path })
foreach ($s in $Scenes) {
    $parts = $s -split '@'
    $state = $parts[0]; $frame = [uint64]$parts[1]
    $name = "$(Split-Path (Split-Path $state -Parent) -Leaf)-$([IO.Path]::GetFileNameWithoutExtension($state))-$frame" # (its folder too: bot runs share names)
    $dump = Join-Path $Out "$name.m3d"
    if (Test-Path $dump) { $all += $dump; continue } # (made by an earlier run)
    $a = @($Roms, '--load-state', $state, '--frames', "$($frame + 1)", '--blocks', '900000000000', '--no-sound', '--dump-3d', "$dump@$frame")
    if ($parts.Count -ge 3) { $a += @('--inputs', $parts[2]) }
    $null = & "$build\gamerun.exe" @a 2>&1
    foreach ($n in 2..4) { # (a state of linked boards: tried as 2, 3, 4 of them; board 0's dump)
        if (Test-Path $dump) { break }
        $null = & "$build\gamerun.exe" @('--boards', "$n", '--parallel') @a 2>&1
    }
    if (-not (Test-Path $dump)) { "no dump for $s"; continue }
    $all += $dump
}

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging;
public static class Cmp {
  // Ours | Supermodel's | the differences (x4), into `sheet`; the share of pixels where a channel differs over `threshold`.
  public static double Sheet(string ours, string theirs, string sheet, int threshold) {
    using (var a = new Bitmap(ours)) using (var b0 = new Bitmap(theirs)) using (var b = new Bitmap(b0, a.Width, a.Height))
    using (var s = new Bitmap(a.Width * 3, a.Height)) {
      long differ = 0;
      for (int y = 0; y < a.Height; y++) for (int x = 0; x < a.Width; x++) {
        Color p = a.GetPixel(x, y), q = b.GetPixel(x, y);
        int dr = Math.Abs(p.R - q.R), dg = Math.Abs(p.G - q.G), db = Math.Abs(p.B - q.B);
        if (Math.Max(dr, Math.Max(dg, db)) > threshold) differ++;
        s.SetPixel(x, y, p); s.SetPixel(x + a.Width, y, q);
        s.SetPixel(x + 2 * a.Width, y, Color.FromArgb(Math.Min(255, dr * 4), Math.Min(255, dg * 4), Math.Min(255, db * 4)));
      }
      s.Save(sheet, ImageFormat.Png);
      return 100.0 * differ / (a.Width * a.Height);
    }
  }
}
"@

function counts($file) {
    $h = @{}
    if (Test-Path $file) { Get-Content $file | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $k = ($_ -split ' ')[0]; $h[$k] = 1 + [int]$h[$k] } }
    $h
}

$rows = @()
foreach ($dump in $all) {
    $name = [IO.Path]::GetFileNameWithoutExtension($dump)
    $ours = Join-Path $Out "$name-ours"; $theirs = Join-Path $Out "$name-supermodel"
    $null = & "$build\frame3d.exe" $Roms $dump --renderer $Renderer --png "$ours.png" --models "$ours-models.txt" 2>&1
    $null = & "$PSScriptRoot\supermodel\draw.ps1" -Dump $dump -Out $theirs -Roms $SupermodelRoms -Work $Work
    if (-not (Test-Path "$ours.png") -or -not (Test-Path "$theirs.png")) { "${name}: not drawn by both"; continue }
    $pct = [Cmp]::Sheet("$ours.png", "$theirs.png", (Join-Path $Out "$name-sheet.png"), $Threshold)
    $a = counts "$ours-models.txt"; $b = counts "$theirs-models.txt"
    $onlyOurs = @($a.Keys | Where-Object { [int]$a[$_] -gt [int]$b[$_] } | Sort-Object)
    $onlyTheirs = @($b.Keys | Where-Object { [int]$b[$_] -gt [int]$a[$_] } | Sort-Object)
    $oursN = ($a.Values | Measure-Object -Sum).Sum; $theirsN = ($b.Values | Measure-Object -Sum).Sum
    "{0}: {1:N2}% of the picture differs; models ours {2}, Supermodel {3}; only ours {4}, only Supermodel's {5}" -f $name, $pct, $oursN, $theirsN, $onlyOurs.Count, $onlyTheirs.Count
    $rows += [pscustomobject]@{ Name = $name; Pct = $pct; Ours = $oursN; Theirs = $theirsN; OnlyOurs = $onlyOurs; OnlyTheirs = $onlyTheirs }
}

# The report
$html = @('<!DOCTYPE html><html><head><meta charset="utf-8"><title>Our 3D against Supermodel</title>',
          '<style>body{font-family:sans-serif;background:#111;color:#ddd}img{max-width:100%}td,th{padding:4px 8px;text-align:left}code{color:#9cf}</style></head><body>',
          "<h1>Our 3D ($Renderer) against Supermodel</h1><p>Each row: ours | Supermodel's | the differences, x4. A pixel counts as different over $Threshold in a colour channel.</p>",
          '<table><tr><th>Scene</th><th>Differs</th><th>Models (ours / Supermodel)</th><th>Only ours</th><th>Only Supermodel''s</th></tr>')
foreach ($r in $rows) {
    $html += "<tr><td>$($r.Name)</td><td>$('{0:N2}' -f $r.Pct)%</td><td>$($r.Ours) / $($r.Theirs)</td><td><code>$(($r.OnlyOurs | Select-Object -First 12) -join ' ')</code></td><td><code>$(($r.OnlyTheirs | Select-Object -First 12) -join ' ')</code></td></tr>"
}
$html += '</table>'
foreach ($r in $rows) { $html += "<h2>$($r.Name)</h2><img src=""$($r.Name)-sheet.png"">" }
$html += '</body></html>'
Set-Content (Join-Path $Out 'index.html') $html -Encoding utf8
"report: $(Join-Path $Out 'index.html')"
