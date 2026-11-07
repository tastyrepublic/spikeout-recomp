# Many first-difference searches (tools/onepc-diff.ps1): each state, each board staying, each bot seed; one line
# each (SAME or the first difference), the whole of each run's output in -Out\<case>\.
#
#   tools/onepc-fuzz.ps1 -Cases "build-jit\bench\two-3800.state:2:3810","build-jit\bench\three-6000.state:3:6010" -Seeds 1,2,3
#
# A case is STATE:BOARDS:FROM; -Frames how long each runs; -NoShadows as onepc-diff.ps1's.
param([string[]]$Cases = @("build-jit\bench\two-3800.state:2:3810"), [int[]]$Seeds = @(1, 2, 3), [int]$Frames = 8000,
      [string]$Out = "build\onepc-fuzz", [string]$Exe = "build\gamerun.exe", [switch]$NoShadows)
New-Item -ItemType Directory -Force $Out | Out-Null
$summary = @()
foreach ($case in $Cases) {
    $p = $case -split ":"
    $state = $p[0..($p.Count - 3)] -join ":"; $boards = [int]$p[-2]; $from = [int]$p[-1]
    foreach ($stays in 0..($boards - 1)) {
        foreach ($seed in $Seeds) {
            $tag = "{0}-b{1}-s{2}" -f [IO.Path]::GetFileNameWithoutExtension($state), $stays, $seed
            $o = & "$PSScriptRoot\onepc-diff.ps1" -State $state -Boards $boards -Stays $stays -From $from -Frames $Frames -BotSeed $seed `
                -Out "$Out\$tag" -Exe $Exe -MaxBytes 12 -NoShadows:$NoShadows *>&1 | Out-String
            Set-Content "$Out\$tag.txt" $o
            $line = ($o -split "`n" | Where-Object { $_ -match "^(SAME|FIRST DIFFERENCE)" } | Select-Object -First 1)
            if (-not $line) { $line = "NO RESULT (see $Out\$tag.txt)" }
            $summary += "${tag}: $($line.Trim())"
            Write-Host "${tag}: $($line.Trim())"
        }
    }
}
Set-Content "$Out\summary.txt" $summary
