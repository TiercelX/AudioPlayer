<#
.SYNOPSIS
  Bounded three-position SpatialDynamicProbe plus WASAPI loopback diagnostic.

.DESCRIPTION
  Dry by default.  -Submit is an explicit live endpoint gate.  Every position
  gets an independent short capture; silence or indistinguishable captures are
  recorded as INCONCLUSIVE and stop further interpretation.
#>
[CmdletBinding()]
param(
    [string]$OutputRoot = "tmp\spatial-impulse-audit",
    [ValidateRange(1000, 600000)] [int]$DurationMs = 1500,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [ValidateSet("standard", "hrtf")]
    [string]$Renderer = "standard",
    [ValidateSet("small", "outdoors")]
    [string]$HrtfEnvironment = "small",
    [ValidateSet("front", "left", "right", "upper")]
    [string[]]$Positions = @('front', 'left', 'upper'),
    [switch]$NoBuild,
    [switch]$Submit,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
}
function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}
function Invoke-Captured([string]$Executable, [string[]]$Arguments, [string]$LogPath) {
    $old = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; $text = (& $Executable @Arguments 2>&1 | Out-String); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $old }
    Write-Utf8NoBom $LogPath $text
    return [pscustomobject]@{ Text = $text; ExitCode = $code }
}
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "self-test: $Message" } }
function New-RunDirectory([string]$Root) {
    $path = Resolve-RepoPath $Root; New-Item -ItemType Directory -Force -Path $path | Out-Null
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
    $candidate = Join-Path $path ("spatial-impulse-{0}" -f $stamp); $n = 0
    while (Test-Path -LiteralPath $candidate) { $n++; $candidate = Join-Path $path ("spatial-impulse-{0}-{1}" -f $stamp, $n) }
    New-Item -ItemType Directory -Path $candidate | Out-Null; return $candidate
}
function Get-ProbeArguments([string]$Position, [string]$ProbeRenderer = 'standard', [string]$Environment = 'small') {
    $args = @('--duration-ms', [string]$DurationMs, '--impulse-delay-ms', '300',
              '--objects', '1', '--signal', 'impulse', '--position', $Position,
              '--renderer', $ProbeRenderer)
    if ($ProbeRenderer -eq 'hrtf') { $args += @('--hrtf-environment', $Environment) }
    return ,$args
}
function Get-AuditExitCode([string]$Result) { if ($Result -like 'FAIL*') { return 1 }; return 0 }

if ($SelfTest) {
    Assert-True ((Get-ProbeArguments 'front') -contains '--signal') 'probe signal contract'
    Assert-True ((Get-ProbeArguments 'front') -contains 'impulse') 'impulse contract'
    Assert-True ((Get-ProbeArguments 'upper') -contains 'upper') 'upper position contract'
    Assert-True ((Get-ProbeArguments 'left').Count -eq 12) 'probe argument count'
    Assert-True ((Get-ProbeArguments 'left') -contains '300') 'impulse delay contract'
    Assert-True ((Get-ProbeArguments 'front' 'hrtf' 'outdoors') -contains 'hrtf') 'hrtf renderer contract'
    Assert-True ((Get-ProbeArguments 'front' 'hrtf' 'outdoors') -contains 'outdoors') 'hrtf environment contract'
    Assert-True ((Get-ProbeArguments 'front' 'standard' 'small') -notcontains 'outdoors') 'standard omits HRTF environment'
    Assert-True ((Quote-ProcessArgument 'C:\capture output\x.wav') -eq '"C:\capture output\x.wav"') 'space quoting'
    Assert-True ((Get-AuditExitCode 'INCONCLUSIVE_LOOPBACK_SILENT') -eq 0) 'inconclusive exit'
    Assert-True ((Get-AuditExitCode 'FAIL_CAPTURE') -eq 1) 'fail exit'
    Write-Host 'auditSpatialImpulseSelfTest=PASS cases=11'
    exit 0
}

$run = New-RunDirectory $OutputRoot
$reportPath = Join-Path $run 'audit-report.json'
$report = [ordered]@{
    schema = 'audioplayer.spatial-impulse-audit.v1'
    result = if ($Submit) { 'INCONCLUSIVE_NOT_YET_RUN' } else { 'INCONCLUSIVE_DRY_RUN' }
    evidenceLayer = 'spatial-endpoint-submission-and-wasapi-loopback'
    submitRequested = [bool]$Submit
    durationMs = $DurationMs
    positions = @($Positions)
    renderer = $Renderer
    hrtfEnvironment = $HrtfEnvironment
    buildDir = (Resolve-RepoPath $BuildDir)
    configuration = $Configuration
    runs = @()
    limits = @('No endpoint is touched without -Submit', 'loopback capture is not physical loudspeaker/headphone evidence', 'no subjective listening claim')
}
if (-not $Submit) {
    $report.note = 'Dry orchestration only; no capture or SpatialDynamicProbe process was invoked.'
    Write-Utf8NoBom $reportPath ($report | ConvertTo-Json -Depth 12)
    Write-Host "auditSpatialImpulse=$($report.result)"; Write-Host "report=$reportPath"; exit 0
}

$build = $report.buildDir
$captureExe = Join-Path $build "$Configuration\WasapiLoopbackCapture.exe"
$probeExe = Join-Path $build "$Configuration\SpatialDynamicProbe.exe"
$analyzer = Join-Path $repoRoot 'tools\atmos-render\analyze_loopback_wav.py'
$comparator = Join-Path $repoRoot 'scripts\compare_spatial_impulse_loopback.py'
if (-not $NoBuild) {
    & cmake --build $build --config $Configuration --target SpatialDynamicProbe WasapiLoopbackCapture -- /m:4
    if ($LASTEXITCODE -ne 0) { throw 'Spatial impulse diagnostic targets failed to build' }
}
foreach ($required in @($captureExe, $probeExe, $analyzer, $comparator)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required diagnostic file missing: $required" }
}
$python = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $python) { $report.result = 'INCONCLUSIVE_PYTHON_MISSING'; Write-Utf8NoBom $reportPath ($report | ConvertTo-Json -Depth 12); exit 0 }

$captureInputs = @{}
foreach ($position in $report.positions) {
    $positionDir = Join-Path $run $position; New-Item -ItemType Directory -Path $positionDir | Out-Null
    $wav = Join-Path $positionDir 'loopback.wav'; $captureReport = Join-Path $positionDir 'loopback.report.json'
    $ready = Join-Path $positionDir 'ready.json'; $stop = Join-Path $positionDir 'stop'
    $captureOut = Join-Path $positionDir 'capture.stdout.txt'; $captureErr = Join-Path $positionDir 'capture.stderr.txt'
    $probeOut = Join-Path $positionDir 'probe.stdout.txt'; $probeErr = Join-Path $positionDir 'probe.stderr.txt'
    $analysis = Join-Path $positionDir 'loopback-analysis.json'
    $entry = [ordered]@{ position=$position; wav=$wav; captureReport=$captureReport; probeResult='NOT_RUN'; probeExitCode=$null; captureExitCode=$null; endpoint=$null; maxDynamicObjects=$null; analyzerResult='NOT_RUN'; analyzerExitCode=$null }
    $capture = $null
    try {
        $captureArgs = @('--duration-ms', [string]$DurationMs, '--wav', $wav, '--report', $captureReport, '--ready-file', $ready, '--stop-file', $stop)
        $captureString = (($captureArgs | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join ' ')
        $capture = Start-Process -FilePath $captureExe -ArgumentList $captureString -PassThru -RedirectStandardOutput $captureOut -RedirectStandardError $captureErr
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Test-Path -LiteralPath $ready -PathType Leaf) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $ready -PathType Leaf)) { throw 'loopback capture did not become ready' }
        Start-Sleep -Milliseconds 200
        $probe = Invoke-Captured $probeExe (Get-ProbeArguments $position $Renderer $HrtfEnvironment) $probeOut
        $entry.probeExitCode = $probe.ExitCode
        if ($probe.Text -match 'probeResult=([^\s]+)') { $entry.probeResult = $Matches[1] }
        if ($probe.Text -match 'endpointId=([^\s]+)') { $entry.endpoint = $Matches[1] }
        if ($probe.Text -match 'maxDynamicObjects=([0-9]+)') { $entry.maxDynamicObjects = [int]$Matches[1] }
    } catch { $entry.error = $_.Exception.Message }
    finally {
        if (-not (Test-Path -LiteralPath $stop)) { Write-Utf8NoBom $stop 'stop' }
        if ($capture -and -not $capture.HasExited) { $capture.WaitForExit(10000) | Out-Null }
        if ($capture -and -not $capture.HasExited) { Stop-Process -Id $capture.Id -Force; $entry.cleanupForced = $true } else { $entry.cleanupForced = $false }
        if ($capture -and $capture.HasExited) { $entry.captureExitCode = $capture.ExitCode }
    }
    if (Test-Path -LiteralPath $captureReport) { try { $entry.capture = Get-Content -Raw $captureReport | ConvertFrom-Json } catch { $entry.captureParseError = $_.Exception.Message } }
    if (Test-Path -LiteralPath $wav) {
        $a = Invoke-Captured $python.Source @($analyzer, $wav, '--output', $analysis) (Join-Path $positionDir 'analyzer.stdout.txt')
        $entry.analyzerExitCode = $a.ExitCode
        if ($a.ExitCode -eq 0 -and (Test-Path -LiteralPath $analysis)) { try { $entry.analyzer = Get-Content -Raw $analysis | ConvertFrom-Json; $entry.analyzerResult = $entry.analyzer.result } catch { $entry.analyzerResult='INCONCLUSIVE_ANALYZER_FAILED' } }
        else { $entry.analyzerResult = 'INCONCLUSIVE_ANALYZER_FAILED' }
        $captureInputs[$position] = $wav
    }
    $report.runs += $entry
}

if ($captureInputs.Count -ge 2) {
    $compareArgs = @(); foreach ($position in $captureInputs.Keys) { $compareArgs += @('--input', ("{0}={1}" -f $position, $captureInputs[$position])) }
    $compareOut = Join-Path $run 'position-comparison.json'
    $comparison = Invoke-Captured $python.Source (@($comparator) + $compareArgs + @('--output', $compareOut)) (Join-Path $run 'position-comparison.stdout.txt')
    $report.comparisonExitCode = $comparison.ExitCode; $report.comparison = $compareOut
    if (Test-Path -LiteralPath $compareOut) { try { $report.positionComparison = Get-Content -Raw $compareOut | ConvertFrom-Json } catch { $report.positionComparisonParseError = $_.Exception.Message } }
}
$validProbe = @($report.runs | Where-Object { $_.probeResult -eq 'PASS' }).Count -eq $report.positions.Count
$anySignal = @($report.runs | Where-Object { $_.analyzerResult -eq 'PASS' }).Count -gt 0
if (-not $anySignal) { $report.result = 'INCONCLUSIVE_LOOPBACK_SILENT'; $report.decision = 'STOP_NO_VALID_TRANSIENT' }
elseif ($report.positions.Count -eq 1 -and $validProbe) { $report.result = 'INCONCLUSIVE_SINGLE_POSITION_CAPTURE'; $report.decision = 'SINGLE_POSITION_CAPABILITY_ONLY' }
elseif ($report.positionComparison -and $report.positionComparison.result -like 'PASS*') { $report.result = $report.positionComparison.result; $report.decision = $report.positionComparison.decision }
else { $report.result = 'INCONCLUSIVE_POSITION_NOT_DISTINGUISHABLE'; $report.decision = 'STOP_POSITION_NOT_DISTINGUISHABLE' }
$report.probeAllPass = $validProbe
$report.note = 'SpatialDynamicProbe endpoint submission and loopback are separate evidence layers; a clean probe is not proof of captured endpoint audio or listening quality.'
Write-Utf8NoBom $reportPath ($report | ConvertTo-Json -Depth 16)
Write-Host "auditSpatialImpulse=$($report.result)"; Write-Host "report=$reportPath"
exit (Get-AuditExitCode $report.result)
