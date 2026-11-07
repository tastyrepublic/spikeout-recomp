# The first frame two gamerun --ram-hashes files differ (each frame's last row: rollback runs frames again), and
# the pages that differ there. For network runs: every machine running a board writes its pages each frame.
#
#   tools/hash-first-diff.ps1 A.bin B.bin   (gamerun's RAM_HASH_IGNORE, set for both runs, leaves bytes out)
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class HashFirstDiff {
    const int Pages = 2048, Row = (2 + Pages) * 4;
    static Dictionary<uint, int> Last(byte[] x) {
        var d = new Dictionary<uint, int>();
        for (int o = 0; o + Row <= x.Length; o += Row) d[BitConverter.ToUInt32(x, o)] = o;
        return d;
    }
    public static string First(string a, string b) {
        byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
        var lx = Last(x); var ly = Last(y);
        var frames = new List<uint>(lx.Keys); frames.Sort();
        int compared = 0;
        foreach (uint f in frames) {
            int oy; if (!ly.TryGetValue(f, out oy)) continue;
            int ox = lx[f]; ++compared;
            var pages = new List<string>();
            for (int p = 0; p < Pages; ++p)
                if (BitConverter.ToUInt32(x, ox + 8 + 4 * p) != BitConverter.ToUInt32(y, oy + 8 + 4 * p)) pages.Add((p * 4096).ToString("x6"));
            uint cx = BitConverter.ToUInt32(x, ox + 4), cy = BitConverter.ToUInt32(y, oy + 4);
            if (pages.Count > 0 || cx != cy)
                return "FIRST DIFFERENCE at frame " + f + " (counts " + cx.ToString("x") + " / " + cy.ToString("x") + "), pages " + string.Join(" ", pages);
        }
        return "SAME over " + compared + " frames";
    }
}
"@
[HashFirstDiff]::First((Resolve-Path $A), (Resolve-Path $B))
