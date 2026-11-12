# The first difference between one board a PC and every board whole, found automatically (no network).
#
# From one state (every board, --boards N --parallel) the same bots play twice: once with every board whole (the
# reference), once with -Stays the only board run whole from -From on (the others made by side runs, as a one-board
# player's PC does). The staying board's main RAM is hashed page by page each frame in both (gamerun --ram-hashes);
# the first frame where a page differs is dumped in both, the differing bytes listed, and both runs repeated with a
# write watch on them over the frames before, so each byte comes with the code that wrote it in each run.
#
#   tools/onepc-diff.ps1 -State build-jit\bench\two-3800.state -Boards 2 -Stays 1 -From 3810 -Frames 4600
#
# A network game's recording (the window's saves/net-game-*: docs/netplay.md, "A network game recorded"):
#   tools/onepc-diff.ps1 -State NAME.state -Boards 4 -Stays 1 -MadeBoard 0 -From F -Frames G -NoBot -Extra '--net-replay',NAME.inputs
# -MadeBoard N: only board N made by side runs, the others whole (offline, more than one made board has no camera or rules).
# -Exe: the gamerun to use; -BotSeed: the bots' seed; -Extra: more gamerun arguments for both runs (e.g. a poke);
# -OneBoard: the one-board run's own arguments; -Watch: frames watched before the first difference (default 3);
# -Build: build gamerun first. -NoShadows: no shadows after the first hand-back (side runs alone through
# everything; a shadow offline starts from this board's state, an approximation, where network play's come from
# the other machine's real board).
param([Parameter(Mandatory)][string]$State, [int]$Boards = 2, [int]$Stays = 1, [int]$From = 3810, [int]$Frames = 4600,
      [string]$Exe = "build\gamerun.exe", [string]$Rom = "Z:\Emu\spikeout-recomp\build\roms\spikeofe", [int]$BotSeed = 1,
      [string[]]$Extra = @(), [string[]]$OneBoard = @("--one-board-auto", "--one-board-held"), [int]$Watch = 3,
      [string]$Out = "build\onepc-diff", [int]$MaxBytes = 40, [switch]$Build, [switch]$NoShadows, [string]$Ignore = "", [switch]$NoBot,
      [int]$TimeoutMin = 15, [int]$MadeBoard = -1)
if ($NoShadows) { $env:ONEBOARD_NO_SHADOWS = "1" } else { Remove-Item Env:ONEBOARD_NO_SHADOWS -ErrorAction SilentlyContinue }
# (-Ignore "LO-HI,...": hex RAM ranges left out of the comparison, bytes known to differ, to look past them)
if ($Ignore) { $env:RAM_HASH_IGNORE = $Ignore } else { Remove-Item Env:RAM_HASH_IGNORE -ErrorAction SilentlyContinue }
$ignored = @($Ignore -split "," | Where-Object { $_ } | ForEach-Object { $a, $b = $_ -split "-"; ,@([Convert]::ToInt32($a, 16), [Convert]::ToInt32($b, 16)) })
$ErrorActionPreference = "Stop"
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
if ($Build) { # (-Build: gamerun built first; stops on a failed build)
    $log = cmake --build (Split-Path $Exe) --target gamerun 2>&1
    if ($LASTEXITCODE) { $log | Select-String "error" | Select-Object -First 5 | Write-Host; throw "build failed" }
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path # (whole: .NET's file calls read paths from the process's folder, not PowerShell's)
# (which build: the exe's time against the newest source's, so a run on an old one shows at once)
$newest = Get-ChildItem tools\*.hpp, tools\*.cpp, src -Recurse -File -Include *.hpp, *.cpp | Sort-Object LastWriteTime | Select-Object -Last 1
$built = (Get-Item $Exe).LastWriteTime
Write-Host ("Build: {0} from {1:HH:mm:ss}; newest source {2} {3:HH:mm:ss}{4}" -f $Exe, $built, $newest.Name, $newest.LastWriteTime,
    $(if ($newest.LastWriteTime -gt $built) { "  ** OLDER THAN THE SOURCES **" } else { "" }))
$made = @(0..($Boards - 1) | Where-Object { $_ -ne $Stays })
if ($MadeBoard -ge 0) { $made = @($MadeBoard) } # (-MadeBoard N: only board N made, the rest whole: one made board offline has its camera and rules)
$obArgs = @()
foreach ($b in $made) { $obArgs += @("--one-board", "$b@$From") }
$obArgs += @("--one-board-stays", "$Stays") + $OneBoard
$common = @($Rom, "--boards", "$Boards", "--parallel", "--load-state", $State, "--frames", "$Frames",
            "--blocks", "900000000000", "--no-sound", "--no-2d", "--no-frame-hashes") + $Extra
if (-not $NoBot) { $common += @("--bot", "$BotSeed") } # (-NoBot: the inputs from -Extra, e.g. @tools/onepc-mash-presses.txt)

function run($tag, [string[]]$more) {
    $a = $common + $more
    $p = Start-Process -PassThru -NoNewWindow -FilePath $Exe -ArgumentList $a -RedirectStandardOutput "$Out\$tag.txt" -RedirectStandardError "$Out\$tag.err"
    # (a time limit: a run that hangs stopped and said, not waited on for hours)
    if (-not $p.WaitForExit($TimeoutMin * 60000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Write-Host "STOPPED: $tag ran over $TimeoutMin minutes (hung?)"
        exit 3
    }
    if ($p.ExitCode -ne 0) { Write-Warning "$tag exited $($p.ExitCode): $(Get-Content "$Out\$tag.err" -Tail 3)" }
}

Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class OnePcDiff {
    // The first row (frame) whose page hashes differ: {frame, counterA, counterB, pages...}, or null.
    // (each frame's last row: a frame run again after a going back (rollback) has a row each time)
    static SortedDictionary<uint, int> Last(byte[] x, int row) {
        var d = new SortedDictionary<uint, int>();
        for (int o = 0; o + row <= x.Length; o += row) d[BitConverter.ToUInt32(x, o)] = o;
        return d;
    }
    public static long[] FirstRow(string a, string b, int pages) {
        int row = (2 + pages) * 4;
        byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
        var lx = Last(x, row); var ly = Last(y, row);
        int n = 0;
        foreach (var kv in lx) {
            int oy;
            if (!ly.TryGetValue(kv.Key, out oy)) continue;
            int ox = kv.Value; ++n;
            var diff = new List<long>();
            for (int p = 0; p < pages; ++p)
                if (BitConverter.ToUInt32(x, ox + 8 + 4 * p) != BitConverter.ToUInt32(y, oy + 8 + 4 * p)) diff.Add(p);
            if (diff.Count > 0 || BitConverter.ToUInt32(x, ox + 4) != BitConverter.ToUInt32(y, oy + 4)) {
                diff.InsertRange(0, new long[] { kv.Key, BitConverter.ToUInt32(x, ox + 4), BitConverter.ToUInt32(y, oy + 4) });
                return diff.ToArray();
            }
        }
        Console.WriteLine("(compared " + n + " frames)");
        return null;
    }
    // The differing bytes of two --ram-dump files of one frame (8-byte header, then RAM from 0): address, a, b.
    public static List<uint[]> Bytes(string a, string b) {
        byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
        const int row = 8 + 0x800000; // (the last of a frame's rows: one each time it ran, after a going back)
        int ox = Math.Max(0, (x.Length / row - 1) * row), oy = Math.Max(0, (y.Length / row - 1) * row);
        var d = new List<uint[]>();
        for (int i = 8; i < Math.Min(x.Length - ox, y.Length - oy); ++i)
            if (x[ox + i] != y[oy + i]) d.Add(new uint[] { (uint)(i - 8), x[ox + i], y[oy + i] });
        return d;
    }
}
"@

$pages = 0x800000 / 4096
Write-Host "Running the reference and the one-board run, frames to $Frames..."
run "ref" @("--ram-hashes", "$Stays@$From", "$Out\ref.hashes")
run "ob" (@("--ram-hashes", "$Stays@$From", "$Out\ob.hashes") + $obArgs)
$first = [OnePcDiff]::FirstRow("$Out\ref.hashes", "$Out\ob.hashes", $pages)
if (-not $first) { Write-Host "SAME: board $Stays equal in every frame from $From"; exit 0 }
$f = $first[0]
$pagesDiff = $first[3..($first.Count - 1)] | ForEach-Object { "{0:x6}" -f ($_ * 4096) }
Write-Host ("FIRST DIFFERENCE at frame {0} (logic counter {1:x} / {2:x}), pages {3}" -f $f, $first[1], $first[2], ($pagesDiff -join " "))

run "ref-dump" @("--ram-dump", "${Stays}:0-800000@${f}:1", "$Out\ref.bin")
run "ob-dump" (@("--ram-dump", "${Stays}:0-800000@${f}:1", "$Out\ob.bin") + $obArgs)
$bytes = @([OnePcDiff]::Bytes("$Out\ref.bin", "$Out\ob.bin") | Where-Object {
    $x = [int]$_[0]; -not ($ignored | Where-Object { $x -ge $_[0] -and $x -lt $_[1] }) })
Write-Host "$($bytes.Count) bytes differ (address: reference / one board):"
$bytes | Select-Object -First $MaxBytes | ForEach-Object { "  {0:x6}: {1:x2} / {2:x2}" -f $_[0], $_[1], $_[2] } | Write-Host
if ($bytes.Count -eq 0) { exit 1 }

# The writes: a watch over the differing range's first few bytes (the first run of differing bytes, 64 at most).
$lo = [int]$bytes[0][0]; $hi = $lo + 1
foreach ($b in $bytes) { if ($b[0] -lt $lo + 64 -and $b[0] -ge $hi) { $hi = [int]$b[0] + 1 } }
$lo = $lo -band -4; $hi = ($hi + 3) -band -4
$w = "{0:x}-{1:x}@{2}:{3}" -f $lo, $hi, ($f - $Watch + 1), $Watch
Write-Host "Writes to $('{0:x6}-{1:x6}' -f $lo, $hi) in frames $($f - $Watch + 1)-${f}:"
run "ref-watch" @("--watch-write", $w, "--frames", "$($f + 1)")
run "ob-watch" (@("--watch-write", $w, "--frames", "$($f + 1)") + $obArgs)
$pick = { param($file) Select-String -Path $file -Pattern 'write\d+ ' | ForEach-Object { ($_.Line -replace '; r3-r12.*$', '') } }
Write-Host "  reference:"; & $pick "$Out\ref-watch.txt" | Select-Object -Last 30 | ForEach-Object { "    $_" } | Write-Host
Write-Host "  one board:"; & $pick "$Out\ob-watch.txt" | Select-Object -Last 30 | ForEach-Object { "    $_" } | Write-Host
exit 1
