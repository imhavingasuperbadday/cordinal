# Builds and runs the console test companion. Each invocation gets a fresh result folder unless
# -Log is explicit. Repeats use separate processes and artifacts to expose intermittent bugs.
[CmdletBinding()]
param(
    [string]$Fixtures = 'build\fixtures',
    [string]$Log = '',
    [string[]]$Suite = @(),
    [switch]$Headless,
    [switch]$List,
    [switch]$FailFast,
    [ValidateRange(1, 1000)][int]$Repeat = 1,
    [uint32]$Seed = 1,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 180,
    [switch]$NoBuild,
    # Runs the suites that cover the files changed since -Since (default HEAD, so uncommitted and
    # untracked work), chosen by tools\test-map.psd1. With -List it prints the choice and its reasons.
    [switch]$Changed,
    [string]$Since = 'HEAD',
    # Prints every START/PASS line. By default the console leaves out suites that passed (the log keeps
    # them all) so a passing run is a few lines: agents read this output after every change.
    [switch]$ShowPasses
)

# The companion's console output without the lines of suites that passed: a passing suite's PASS line,
# its START line, and the companion's own Reports line (the runner prints its own). A START with no
# result after it stays, since it names the suite that crashed or hung.
function Format-Output([string]$text) {
    if ($ShowPasses -or !$text) { return $text }
    $lines = $text -split "`r?`n"
    $finished = @{}
    foreach ($line in $lines) { if ($line -match '^(PASS|FAIL|SKIP) (\S+) \(') { $finished[$Matches[2]] = $true } }
    $kept = foreach ($line in $lines) {
        if ($line -match '^START (\S+)$' -and $finished[$Matches[1]]) { continue }
        if ($line -match '^PASS \S+ \(\d+ checks?, 0 failed') { continue }
        if ($line -match '^Reports: ') { continue }
        $line
    }
    ($kept -join "`n")
}

# A path under the repository as the short relative form the console shows (the runner runs from the root).
function Show-Path([string]$path) {
    $prefix = $root.TrimEnd('\') + '\'
    if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $path.Substring($prefix.Length) } else { $path }
}

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (!$NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Unpacked }
$exe = @('build\unpacked\cordinal-test.exe', 'build\cordinal-test.exe') |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$exe) { throw 'cordinal-test.exe is missing. Run build.bat unpacked first.' }
$exe = (Resolve-Path -LiteralPath $exe).Path

# Requirements come from the executable, so there is no second feature list to maintain.
# Fixture-free suites work without FFmpeg installed.
$inventory = @(& $exe --list-tests)
if ($LASTEXITCODE -ne 0) { throw 'Cannot list test suites. Rebuild the companion.' }
$patterns = @($Suite | ForEach-Object { $_ -split ',' })

# Turns the working tree's changes into suites, so the fast loop does not depend on remembering which
# suite owns which file. Everything it decides is printed, and a file no rule covers is named rather
# than silently skipped.
if ($Changed) {
    if ($Suite.Count) { throw '-Changed chooses the suites itself; leave out -Suite.' }
    if (!(Get-Command git -ErrorAction SilentlyContinue)) { throw '-Changed needs git on PATH.' }
    $paths = @(& git diff --name-only $Since -- 2>$null)
    if ($LASTEXITCODE) { throw "git cannot compare with '$Since'." }
    $paths = @(($paths + @(& git ls-files --others --exclude-standard)) | Where-Object { $_ } | Sort-Object -Unique)
    $rules = (Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'test-map.psd1')).Rules
    $owners = @{}  # test source -> its suites, from the inventory
    foreach ($line in @($inventory | Select-Object -Skip 1)) {
        $fields = $line -split "`t"
        if ($fields[2] -match '\(([^()]+)\)\s*$') { $owners[$Matches[1]] += @($fields[0]) }
    }
    $chosen = [ordered]@{}  # suite -> the first path that chose it
    $notes = [ordered]@{}
    $unmapped = @()
    foreach ($path in $paths) {
        $hit = $false
        foreach ($suiteId in @($owners[$path])) { if ($suiteId) { $hit = $true; if (!$chosen.Contains($suiteId)) { $chosen[$suiteId] = $path } } }
        foreach ($rule in $rules) {
            if (!@($rule.Paths | Where-Object { $path -like $_ }).Count) { continue }
            $hit = $true
            foreach ($suiteId in @($rule.Suites)) { if (!$chosen.Contains($suiteId)) { $chosen[$suiteId] = $path } }
            if ($rule.Note) { $notes[$rule.Note] = $true }
        }
        if (!$hit -and $path -match '^(src|tools)/' -and $path -notmatch '\.(md|txt)$') { $unmapped += $path }
    }
    Write-Host ("==> {0} changed file(s) since {1}" -f $paths.Count, $Since)
    if ($chosen.Contains('*')) {
        Write-Host "    every suite: $($chosen['*']) is shared by nearly everything"
        $patterns = @()
    } else {
        foreach ($entry in $chosen.GetEnumerator()) { Write-Host ("    {0,-20} {1}" -f $entry.Key, $entry.Value) }
        $patterns = @($chosen.Keys)
    }
    foreach ($note in $notes.Keys) { Write-Host "    note: $note" }
    foreach ($path in $unmapped) { Write-Host "    unmapped: $path (add a rule to tools\test-map.psd1; consider test.bat -Headless)" }
    if (!$chosen.Count) {
        Write-Host '    No suite covers these changes.'
        exit 0
    }
    if ($List) { exit 0 }
} elseif ($List) { $inventory; exit 0 }
$selected = @($inventory | Select-Object -Skip 1 | Where-Object {
    $id = ($_ -split "`t")[0]
    !$patterns.Count -or @($patterns | Where-Object { $id -like $_ }).Count -gt 0
})
foreach ($pattern in $patterns) {
    if (!$pattern -or !@($inventory | Select-Object -Skip 1 | Where-Object { ($_ -split "`t")[0] -like $pattern }).Count) {
        throw "No suite matches '$pattern'. Run test.bat -List."
    }
}
$needsFixtures = @($selected | Where-Object {
    $requirements = ($_ -split "`t")[1]
    $requirements.Contains(',') -and (!$Headless -or $requirements.StartsWith('headless'))
}).Count -gt 0
if ($needsFixtures) {
    $fixtureFiles = @('media\sine.flac', 'media\sine.mp3', 'media\sine.m4a', 'media\alac.m4a', 'media\sine.ogg',
        'media\sine.opus', 'media\sine.wav', 'media\sine.wma', 'gap2\a.flac', 'gap2\b.flac', 'gap2\a.mp3',
        'gap2\b.mp3', 'gap2\a.m4a', 'gap2\b.m4a', 'gap2\a.ogg', 'gap2\b.ogg', 'gap2\ref.wav',
        'rg\rg.flac', 'rg\rg.mp3', 'rg\rg.ogg', 'rg\rg.opus', 'tags2\full.flac', 'tags2\full.mp3',
        'tags2\full.m4a', 'tags2\full.ogg', 'native\sine.wv', 'native\sine.ac3', 'native\sine.mka', 'native\video-aac.mp4',
        'native\preview.flac', 'native\sine.au', 'native\sine.caf', 'native\alac.caf', 'native\pcm16.wv', 'native\sine.oga',
        'native\sine.voc',
        'native\ulaw-au-ref.wav', 'native\ima4-caf-ref.wav', 'native\ima4-aifc-ref.wav', 'native\u8-voc-ref.wav',
        'native\fib-8svx-ref.wav', 'native\stereo-8svx-ref.wav', 'native\g722-wav-ref.wav', 'native\g722-au-ref.wav',
        'native\raw-g722-ref.wav', 'native\raw-gsm-ref.wav', 'native\gsm-wav-ref.wav', 'native\raw-dfpwm-ref.wav',
        'native\dfpwm-wav-ref.wav', 'native\stereo-adx-ref.wav')
    $missing = @($fixtureFiles | Where-Object {
        $path = Join-Path $Fixtures $_
        !(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0
    })
    if ($missing.Count -and !$PSBoundParameters.ContainsKey('Fixtures')) {
        if (!(Get-Command ffmpeg.exe -ErrorAction SilentlyContinue)) {
            throw 'FFmpeg is needed to generate fixtures. Install it on PATH or pass -Fixtures <existing folder>.'
        }
        Write-Host "==> Making fixtures in $Fixtures ($($missing.Count) missing or empty files)"
        & (Join-Path $PSScriptRoot 'make-fixtures.ps1') -Out $Fixtures
    }
    # Explicit fixture folders are read as supplied. Missing files are reported per selected suite.
}

if (!$Log) { $Log = 'build\test-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + "-$PID\selftest.log" }
if (![IO.Path]::IsPathRooted($Log)) { $Log = Join-Path $root $Log }
if (![IO.Path]::IsPathRooted($Fixtures)) { $Fixtures = Join-Path $root $Fixtures }
$Log = [IO.Path]::GetFullPath($Log)
$Fixtures = [IO.Path]::GetFullPath($Fixtures)

# Avoid cmd expansion of %, &, parentheses and quoted path names. Works with PowerShell 5,
# which has no ProcessStartInfo.ArgumentList property.
function Quote-Argument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

# A full run gives the long suites that share nothing with the rest (the real player's memory, real
# playback) processes of their own beside it: in one process the whole set outgrew one timeout, and side
# by side the run takes about as long as its longest part. Each process has its own log and report.
$alone = @()
if (!$patterns.Count) {
    $ids = @($selected | ForEach-Object { ($_ -split "`t")[0] })
    $alone = @(@('memory-budget', 'engine') | Where-Object { $ids -contains $_ })
    $rest = @($ids | Where-Object { $alone -notcontains $_ }) -join ','
}

$totalFailures = 0
$stop = 0  # a timeout or a broken run ends the whole invocation with its code, as before
for ($iteration = 1; $iteration -le $Repeat -and !$stop; ++$iteration) {
    $iterationLog = if ($Repeat -eq 1) { $Log } else {
        Join-Path (Split-Path -Parent $Log) ("run-{0:D3}\{1}" -f $iteration, (Split-Path -Leaf $Log))
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $iterationLog) | Out-Null
    $shards = @([pscustomobject]@{ Suites = $(if ($alone.Count) { $rest } else { $patterns -join ',' }); Log = $iterationLog })
    foreach ($id in $alone) {
        $shards += [pscustomobject]@{ Suites = $id; Log = ($iterationLog -replace '\.log$', '') + "-$id.log" }
    }
    $runs = @(foreach ($shard in $shards) {
        $arguments = @('--selftest', $Fixtures, $shard.Log, '--seed', [string]$Seed)
        if ($Headless) { $arguments += '--headless' }
        if ($FailFast) { $arguments += '--fail-fast' }
        if ($shard.Suites) { $arguments += @('--suite', $shard.Suites) }
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $exe
        $start.Arguments = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
        $start.WorkingDirectory = $root
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.StandardOutputEncoding = [Text.Encoding]::UTF8
        $start.StandardErrorEncoding = [Text.Encoding]::UTF8
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $start
        Write-Host "==> Self-test $iteration/$Repeat seed=$Seed timeout=${TimeoutSeconds}s -> $(Show-Path $shard.Log)"
        $run = [pscustomobject]@{ Log = $shard.Log; Process = $process; Timer = [Diagnostics.Stopwatch]::StartNew()
                                  StartedAt = [DateTime]::UtcNow; Pid = 0; Stdout = $null; Stderr = $null; Failed = $null }
        try {
            if (!$process.Start()) { throw 'Could not start cordinal-test.exe.' }
            $run.Pid = $process.Id
            $run.Stdout = $process.StandardOutput.ReadToEndAsync()
            $run.Stderr = $process.StandardError.ReadToEndAsync()
        } catch { $run.Failed = $_ }
        $run
    })
    foreach ($run in $runs) {
        $process = $run.Process
        $runLog = $run.Log
        $timer = $run.Timer
        $startedAt = $run.StartedAt
        $childPid = $run.Pid
        $stdout = $run.Stdout
        $stderr = $run.Stderr
        try {
            if ($run.Failed) { throw $run.Failed }
            $timedOut = !$process.WaitForExit([Math]::Max(0, $TimeoutSeconds * 1000 - [int]$timer.ElapsedMilliseconds))
            if ($timedOut) {
                if ($PSVersionTable.PSVersion.Major -ge 7) { $process.Kill($true) }
                else { & taskkill.exe /PID $childPid /T /F | Out-Null }
                $process.WaitForExit()
            }
            $code = if ($timedOut) { 124 } else { $process.ExitCode }
            Write-Host (Format-Output $stdout.GetAwaiter().GetResult()) -NoNewline
            $errorText = $stderr.GetAwaiter().GetResult()
            if ($errorText) { Write-Host $errorText -NoNewline }
        } finally {
            $timer.Stop()
            # Its own run time: the processes ran side by side, and this one may have been waited on last.
            $elapsed = try { ($process.ExitTime - $process.StartTime).TotalSeconds } catch { $timer.Elapsed.TotalSeconds }
            $process.Dispose()
        }

        $problem = ''
        $reportPath = $runLog + '.json'
        $report = $null
        if ((Test-Path -LiteralPath $reportPath) -and (Get-Item -LiteralPath $reportPath).LastWriteTimeUtc -ge $startedAt.AddSeconds(-1)) {
            try {
                $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
                if ($report.processId -ne $childPid) { $report = $null }
            } catch { $problem = 'Invalid JSON report.' }
        }
        if ($timedOut) { $problem = "Timed out after $TimeoutSeconds seconds." }
        elseif (!$report -or !$report.complete) { $problem = "Process exited before completing its report (exit $code)." }
        elseif ($code -eq 0 -and $report.failures -ne 0) { $problem = 'Exit code disagrees with the report.' }
        elseif (!(Test-Path -LiteralPath ($runLog + '.xml'))) { $problem = 'JUnit report is missing.' }
        if ($problem) {
            $active = if ($report) { @($report.suites | Where-Object status -eq 'running' | ForEach-Object id) -join ',' } else { 'startup' }
            $diagnostic = "$problem Active suite: $active. Log and artifacts: $runLog"
            Write-Host $diagnostic
            Set-Content -LiteralPath ($runLog + '.runner-error.txt') -Value $diagnostic -Encoding UTF8
            if ($code -eq 0) { $code = 3 }
        }
        if ($report) {
            foreach ($failure in @($report.suites | Where-Object status -eq 'failed')) {
                $mode = if ($Headless) { ' -Headless' } else { '' }
                Write-Host "Reproduce: test.bat -NoBuild$mode -Suite $($failure.id) -Seed $Seed -Fixtures `"$Fixtures`""
            }
        }
        Write-Host ("Self-test exited {0} in {1:N1}s. Reports: {2}.json and .xml" -f $code, $elapsed, (Show-Path $runLog))
        if ($code -lt 0 -or $code -gt 65535) { $code = 3; $problem = 'Invalid exit code.' }
        # The other processes of this run still finish and report before the invocation ends.
        if (($code -eq 124 -or $problem) -and !$stop) { $stop = $code }
        if (!$problem) { $totalFailures += $code }
    }
    if ($totalFailures -ne 0 -and $FailFast) { break }
}
if ($stop) { exit $stop }
exit $totalFailures
