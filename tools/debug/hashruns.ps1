param([string]$A, [string]$B, [int]$From, [int]$To)
# Every run (row) of frames From..To in B's --ram-hashes, against A's last row of each: pages that differ.
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class HRuns {
  const int Row = 2050 * 4;
  public static List<string> Go(string a, string b, uint from, uint to) {
    var last = new Dictionary<uint, byte[]>(); var x = File.ReadAllBytes(a);
    for (int o = 0; o + Row <= x.Length; o += Row) { var r = new byte[Row]; Array.Copy(x, o, r, 0, Row); last[BitConverter.ToUInt32(r, 0)] = r; }
    var y = File.ReadAllBytes(b); var outp = new List<string>(); int n = 0;
    for (int o = 0; o + Row <= y.Length; o += Row) { ++n; uint f = BitConverter.ToUInt32(y, o);
      if (f < from || f > to || !last.ContainsKey(f)) continue; var r = last[f]; var pages = new List<string>();
      for (int p = 0; p < 2048; ++p) if (BitConverter.ToUInt32(r, 8 + 4 * p) != BitConverter.ToUInt32(y, o + 8 + 4 * p)) pages.Add((p * 4096).ToString("x"));
      outp.Add(string.Format("row {0}: frame {1}: {2}", n, f, pages.Count == 0 ? "same" : pages.Count + " pages differ (" + string.Join(" ", pages.GetRange(0, Math.Min(6, pages.Count))) + ")")); }
    return outp; }
}
"@
[HRuns]::Go($A, $B, [uint32]$From, [uint32]$To)
