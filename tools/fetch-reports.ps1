# What the game has sent to the lobby (tools/problem_report.hpp part 2; lobby/src/index.js, "Reports sent by the game
# itself"), fetched to a folder of the owner's: each network game's numbers (stats.jsonl, a line each), the games' inputs
# (inputs/*.inputs.locked, for tools/guess-study.cpp) and the problem
# bundles (problems/*.zip.locked), unlocked with the owner's report key into problems/*.zip (gamerun --unlock).
#   tools/fetch-reports.ps1 [-Out DIR] [-TokenFile FILE] [-KeyFile FILE] [-Gamerun EXE]
# The admin token (the lobby's ADMIN_TOKEN secret) and the report key stay outside the repo, as files of the owner's.
# Fetches only what is new since the last time (Out\last.txt keeps where it got to).
param([string]$Out = "$env:USERPROFILE\spikeout-reports", [string]$TokenFile = "$env:USERPROFILE\spikeout-admin-token.txt",
      [string]$KeyFile = "$env:USERPROFILE\spikeout-report-key.txt", [string]$Gamerun = '',
      [string]$Lobby = 'https://spikeout-lobby.spikeout-lobby.workers.dev')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $TokenFile)) { "no admin token at $TokenFile (the lobby's ADMIN_TOKEN, a file of yours)"; exit 1 }
$headers = @{ Authorization = "Bearer $((Get-Content -Raw $TokenFile).Trim())" }
New-Item -ItemType Directory -Force "$Out\problems" | Out-Null
$last = @{ stats = 0; problems = 0 }
if (Test-Path "$Out\last.txt") { Get-Content "$Out\last.txt" | ForEach-Object { $k, $v = $_ -split '='; $last[$k] = [int]$v } }

$stats = 0
do {
    $rows = Invoke-RestMethod -Uri "$Lobby/v1/admin/stats?after=$($last.stats)" -Headers $headers
    foreach ($r in $rows) {
        $line = [ordered]@{ id = $r.id; at = [DateTimeOffset]::FromUnixTimeMilliseconds($r.at).ToString('u'); version = $r.version;
                            country = $r.country; game = ($r.json | ConvertFrom-Json) } | ConvertTo-Json -Compress -Depth 6
        Add-Content -Path "$Out\stats.jsonl" -Value $line -Encoding utf8
        $last.stats = [int]$r.id; $stats++
    }
} while ($rows.Count -ge 500)

New-Item -ItemType Directory -Force "$Out\inputs" | Out-Null
if (-not $last.ContainsKey('inputs')) { $last.inputs = 0 }
$files = @()
foreach ($kind in 'problems', 'inputs') { # (problem bundles, and games' inputs for tools/guess-study.cpp)
    do {
        $rows = Invoke-RestMethod -Uri "$Lobby/v1/admin/${kind}?after=$($last[$kind])" -Headers $headers
        foreach ($r in $rows) {
            $name = "$($r.id)-$($r.version)-$($r.country)-$([IO.Path]::GetFileName($r.key))"
            Invoke-WebRequest -Uri "$Lobby/v1/admin/file/$([Uri]::EscapeDataString($r.key))" -Headers $headers -OutFile "$Out\$kind\$name"
            $files += "$Out\$kind\$name"
            $last[$kind] = [int]$r.id
        }
    } while ($rows.Count -ge 500)
}
"stats=$($last.stats)", "problems=$($last.problems)", "inputs=$($last.inputs)" | Set-Content "$Out\last.txt"

"$stats games' numbers (in $Out\stats.jsonl), $(@($files | Where-Object { $_ -like '*\problems\*' }).Count) problem bundles, " +
    "$(@($files | Where-Object { $_ -like '*\inputs\*' }).Count) games' inputs"
if ($files.Count) {
    $exe = if ($Gamerun) { $Gamerun } else { Join-Path (Split-Path $PSScriptRoot) 'build\gamerun.exe' }
    if ((Test-Path $exe) -and (Test-Path $KeyFile)) { & $exe --unlock @files --report-key $KeyFile }
    else { "to open them: gamerun --unlock FILE.locked --report-key $KeyFile" }
}
# (this month's use of R2's free operations and the space, as the lobby counts them: it refuses past half of each)
$u = Invoke-RestMethod -Uri "$Lobby/v1/admin/usage" -Headers $headers
"R2 this month ($($u.month)): class A $($u.r2a[0]) of $($u.r2a[1]), class B $($u.r2b[0]) of $($u.r2b[1]); stored " +
    "$([math]::Round($u.stored / 1MB, 1)) MB of $([math]::Round($u.kept / 1GB, 1)) GB"
