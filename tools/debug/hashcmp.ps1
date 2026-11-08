param([string]$A, [string]$B, [int]$Around = 0)
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class HCmp {
  const int Row = 2050 * 4;
  public static Dictionary<uint, byte[]> Load(string f) { var d = new Dictionary<uint, byte[]>(); var all = File.ReadAllBytes(f);
    for (int o = 0; o + Row <= all.Length; o += Row) { var r = new byte[Row]; Array.Copy(all, o, r, 0, Row); d[BitConverter.ToUInt32(r, 0)] = r; } return d; }
  public static string First(Dictionary<uint, byte[]> A, Dictionary<uint, byte[]> B) {
    var keys = new List<uint>(A.Keys); keys.Sort();
    foreach (var f in keys) { if (!B.ContainsKey(f)) continue; var x = A[f]; var y = B[f];
      var pages = new List<string>();
      for (int p = 0; p < 2048; ++p) if (BitConverter.ToUInt32(x, 8 + 4 * p) != BitConverter.ToUInt32(y, 8 + 4 * p)) pages.Add((p * 4096).ToString("x"));
      if (pages.Count > 0) return f + " counters " + BitConverter.ToUInt32(x, 4).ToString("x") + "/" + BitConverter.ToUInt32(y, 4).ToString("x") + " pages " + string.Join(" ", pages.GetRange(0, Math.Min(24, pages.Count)));
    }
    return "same in " + keys.Count + " frames";
  }
}
"@
$x = [HCmp]::Load($A); $y = [HCmp]::Load($B)
$r = [HCmp]::First($x, $y)
"first difference: frame $r"
if ($r -match '^(\d+) ') { # (is B's row there, or the next ones, A's row of another frame? a wrong frame put back)
    $f0 = [int]$Matches[1]
    foreach ($k in $f0..($f0 + 3)) {
        $q = $y[[uint32]$k]; if (-not $q) { continue }
        $hit = @($x.Keys | Where-Object { $p = $x[$_]; [System.Linq.Enumerable]::SequenceEqual([byte[]]$p[4..($p.Length - 1)], [byte[]]$q[4..($q.Length - 1)]) })
        "frame ${k}: B's RAM equals A's at frame(s): " + ($(if ($hit) { $hit -join ' ' } else { 'none' }))
    }
}
if ($Around -and $r -match '^(\d+) ') {
    $f = [int]$Matches[1]
    foreach ($k in ($f - $Around)..($f + 3)) {
        $p = $x[[uint32]$k]; $q = $y[[uint32]$k]
        if ($p -and $q) { "{0}: counters {1:x} / {2:x}" -f $k, [BitConverter]::ToUInt32($p, 4), [BitConverter]::ToUInt32($q, 4) }
    }
}
