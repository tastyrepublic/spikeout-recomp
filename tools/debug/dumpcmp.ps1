param([string]$A, [string]$B, [int]$Lo = 0x100000, [int]$Size = 0x20000, [int]$Max = 40)
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic; using System.Text;
public static class DCmp {
  public static Dictionary<uint, byte[]> Load(string f, int size) { var d = new Dictionary<uint, byte[]>(); int row = 8 + size;
    using (var s = File.OpenRead(f)) { var r = new byte[row]; while (s.Read(r, 0, row) == row) { d[BitConverter.ToUInt32(r, 0)] = (byte[])r.Clone(); } } return d; }
  public static string First(Dictionary<uint, byte[]> A, Dictionary<uint, byte[]> B, int lo, int size, int max) {
    var keys = new List<uint>(A.Keys); keys.Sort();
    foreach (var f in keys) { if (!B.ContainsKey(f)) continue; var x = A[f]; var y = B[f];
      var sb = new StringBuilder(); int n = 0;
      for (int i = 0; i < size; ++i) if (x[8 + i] != y[8 + i]) { if (n < max) sb.Append(string.Format("  {0:x6}: {1:x2} / {2:x2}\n", lo + i, x[8 + i], y[8 + i])); ++n; }
      if (n > 0) return string.Format("frame {0}: counters {1:x} / {2:x}, {3} bytes differ\n{4}", f, BitConverter.ToUInt32(x, 4), BitConverter.ToUInt32(y, 4), n, sb);
    }
    return "same in " + keys.Count + " frames";
  }
}
"@
[DCmp]::First([DCmp]::Load($A, $Size), [DCmp]::Load($B, $Size), $Lo, $Size, $Max)
