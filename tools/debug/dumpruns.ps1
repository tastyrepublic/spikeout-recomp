param([string]$A, [string]$B, [int[]]$Frames, [int]$Size = 0x20000)
# Every run of each frame in B (rollback runs frames again: each a row), compared with A's last run of it.
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class DRuns {
  public static Dictionary<uint, byte[]> Last(string f, int size) { var d = new Dictionary<uint, byte[]>(); int row = 8 + size;
    using (var s = File.OpenRead(f)) { var r = new byte[row]; while (s.Read(r, 0, row) == row) d[BitConverter.ToUInt32(r, 0)] = (byte[])r.Clone(); } return d; }
  public static List<string> Runs(string f, int size, Dictionary<uint, byte[]> refs, uint[] frames) { var o = new List<string>(); int row = 8 + size; long n = 0;
    var want = new HashSet<uint>(frames);
    using (var s = File.OpenRead(f)) { var r = new byte[row]; while (s.Read(r, 0, row) == row) { ++n; uint fr = BitConverter.ToUInt32(r, 0);
      if (!want.Contains(fr) || !refs.ContainsKey(fr)) continue; var x = refs[fr]; int d = 0;
      for (int i = 0; i < size; ++i) if (x[8 + i] != r[8 + i]) ++d;
      o.Add(string.Format("row {0}: frame {1} counter {2:x}: {3} bytes differ from A", n, fr, BitConverter.ToUInt32(r, 4), d)); } }
    return o; }
}
"@
$refs = [DRuns]::Last($A, $Size)
[DRuns]::Runs($B, $Size, $refs, [uint32[]]$Frames)
