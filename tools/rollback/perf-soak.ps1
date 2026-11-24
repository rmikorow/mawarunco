# Soak test before a release: runs the game for a few minutes in the modes players use and checks the
# performance report (logs\perf-*.txt) for memory growth and frames that get slower over time.
#
#   .\tools\rollback\perf-soak.ps1 -Iso <path to your disc image> [-Minutes 4]
#
# Modes:
#   training  practice mode with run-ahead (a snapshot save and restore every frame)
#   synctest  netplay sync test (a rollback of 2 frames every frame, resimulated and compared)
# Uses the Release build in bin\ (portable). Exit code 1 if a check fails.
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [double]$Minutes = 4,
    [ValidateSet('training', 'synctest', 'all')][string]$Mode = 'all',
    # Limits for a failure.
    [double]$MaxMemoryMBPerMinute = 20,
    [double]$MaxWorkRiseMs = 1.5
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$bin = Join-Path $repo 'bin'
$exe = Join-Path $bin 'pcsx2-qtx64-clang.exe'
$logs = Join-Path $bin 'logs'
if (-not (Test-Path $exe)) { throw 'Release build missing (bin\pcsx2-qtx64-clang.exe).' }
if (-not (Test-Path $Iso)) { throw "Disc image not found: $Iso" }
if (-not (Test-Path (Join-Path $bin 'portable.txt'))) { New-Item -ItemType File (Join-Path $bin 'portable.txt') | Out-Null }

function Invoke-Soak([string]$name, [string[]]$arguments) {
    $before = Get-Date
    Write-Host "== $name ($Minutes min)"
    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList $arguments -PassThru
    Start-Sleep -Seconds ([int]($Minutes * 60))
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 1
    $report = Get-ChildItem $logs -Filter 'perf-*.txt' | Where-Object { $_.LastWriteTime -gt $before } |
        Sort-Object Name | Select-Object -Last 1
    if (-not $report) { Write-Host '   FAIL: no performance report written'; return $false }

    # Lines: " m:ss | frames | work avg / max | stutters, worst | memory (change) | ..."
    $rows = foreach ($line in Get-Content $report.FullName) {
        if ($line -match '^\s*(\d+):(\d\d) \|\s*(\d+) \|\s*([\d.]+) / [\d.]+ \|\s*(\d+),\s*[\d.]+ \|\s*(\d+) ') {
            [pscustomobject]@{
                T = [int]$Matches[1] * 60 + [int]$Matches[2]
                Work = [double]$Matches[4]
                Stutters = [int]$Matches[5]
                Mem = [double]$Matches[6]
            }
        }
    }
    $late = @($rows | Where-Object { $_.T -ge 60 })
    if ($late.Count -lt 6) { Write-Host '   FAIL: too few report lines (run longer, at least 2 minutes)'; return $false }

    # Least-squares slope of memory over time (MB per minute).
    $n = $late.Count
    $st = ($late | Measure-Object T -Sum).Sum; $sm = ($late | Measure-Object Mem -Sum).Sum
    $stt = ($late | ForEach-Object { $_.T * $_.T } | Measure-Object -Sum).Sum
    $stm = ($late | ForEach-Object { $_.T * $_.Mem } | Measure-Object -Sum).Sum
    $slope = (($n * $stm - $st * $sm) / ($n * $stt - $st * $st)) * 60
    $k = [Math]::Min(6, [int]($n / 2))
    $early = ($late | Select-Object -First $k | Measure-Object Work -Average).Average
    $last = ($late | Select-Object -Last $k | Measure-Object Work -Average).Average
    $stutters = ($rows | Measure-Object Stutters -Sum).Sum

    Write-Host ('   memory {0:N0} -> {1:N0} MB, trend {2:+0.0;-0.0} MB/min' -f $late[0].Mem, $late[-1].Mem, $slope)
    Write-Host ('   frame work {0:N2} ms early, {1:N2} ms late; {2} stutters' -f $early, $last, $stutters)
    Write-Host "   report: $($report.Name)"
    $ok = $true
    if ($slope -gt $MaxMemoryMBPerMinute) { Write-Host '   FAIL: memory keeps growing'; $ok = $false }
    if ($last - $early -gt $MaxWorkRiseMs) { Write-Host '   FAIL: frames get slower over time'; $ok = $false }
    if ($ok) { Write-Host '   ok' }
    return $ok
}

$results = @()
if ($Mode -in 'training', 'all') {
    $results += Invoke-Soak 'training' @('-batch', '-bootshortcut', 'practice', '--', "`"$Iso`"")
}
if ($Mode -in 'synctest', 'all') {
    $results += Invoke-Soak 'synctest' @('-batch', '-netplay', 'synctest,boot=vs2p', '--', "`"$Iso`"")
}
if ($results -contains $false) { Write-Host 'SOAK TEST FAILED'; exit 1 }
Write-Host 'SOAK TEST PASSED'
