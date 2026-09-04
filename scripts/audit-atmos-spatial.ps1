<#
.SYNOPSIS
  Bounded, report-first Spatial Audio plus WASAPI loopback diagnostic.

.DESCRIPTION
  The default is a dry orchestration check and never submits audio.  Pass
  -Submit explicitly to start WasapiLoopbackCapture before the existing
  play-atmos-spatial.ps1 delegate.  A clean loopback capture remains
  INCONCLUSIVE under the repository's endpoint evidence contract; this script
  does not turn submitted PCM into an endpoint-output PASS.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string]$InputPath = "",
    [string]$OutputRoot = "tmp\atmos-spatial-audit",
    [ValidateRange(1, 1000000)] [int]$MaxAccessUnits = 8,
    [switch]$DisableLfe,
    [ValidateSet("source", "unit")]
    [string]$PositionRadiusMode = "source",
    [ValidateSet("metadata", "front")]
    [string]$PositionDirectionMode = "metadata",
    [ValidateRange(0.0, 1.0)]
    [double]$AzimuthFocus = 0.0,
    [ValidateSet("standard", "hrtf")]
    [string]$SpatialRenderer = "standard",
    [ValidateSet("small", "outdoors")]
    [string]$HrtfEnvironment = "small",
    [switch]$Submit,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$NoBuild,
    [switch]$Force,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-PowerShellExecutable {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pwsh) { return $pwsh.Source }
    return (Get-Command powershell.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

function Get-SafeStem([string]$Path) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($Path) -replace '[^A-Za-z0-9._-]+', '-'
    $stem = $stem.Trim('-')
    if ([string]::IsNullOrWhiteSpace($stem)) { return 'atmos-input' }
    return $stem
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "self-test: $Message" }
}

function Get-AuditExitCode([string]$Result, [string]$OperationError) {
    if ($OperationError -or $Result -like 'FAIL*') { return 1 }
    return 0
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
}

function Quote-ProcessArgument([string]$Value) {
    # Start-Process -ArgumentList accepts a single command-line string on
    # Windows PowerShell 5.1. Quote paths with spaces explicitly.
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function New-AuditDirectory([string]$Root, [string]$Stem) {
    $rootPath = Resolve-RepoPath $Root
    New-Item -ItemType Directory -Force -Path $rootPath | Out-Null
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
    $path = Join-Path $rootPath ("{0}-{1}" -f $Stem, $stamp)
    $suffix = 0
    while (Test-Path -LiteralPath $path) {
        $suffix++
        $path = Join-Path $rootPath ("{0}-{1}-{2}" -f $Stem, $stamp, $suffix)
    }
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}

function Invoke-Captured([string]$Executable, [string[]]$Arguments, [string]$LogPath) {
    $previous = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 promotes native stderr to ErrorRecord. Keep
        # it in the captured log without aborting before LASTEXITCODE is read.
        $ErrorActionPreference = 'Continue'
        $text = (& $Executable @Arguments 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    Write-Utf8NoBom $LogPath $text
    return [pscustomobject]@{ Text = $text; ExitCode = $exitCode }
}

function Get-DelegateArguments([string]$InputFile, [string]$OutputDir, [int]$MaxUnits,
                               [string]$Build, [string]$Config, [switch]$NoBuildSwitch,
                               [switch]$ForceSwitch, [switch]$DisableLfeSwitch,
                               [string]$RadiusMode = "source",
                               [string]$DirectionMode = "metadata",
                               [double]$Focus = 0.0,
                               [string]$Renderer = "standard",
                               [string]$Environment = "small") {
    $args = @('-InputPath', $InputFile, '-OutputDir', $OutputDir,
              '-MaxAccessUnits', [string]$MaxUnits,
              '-BuildDir', $Build, '-Configuration', $Config)
    if ($NoBuildSwitch) { $args += '-NoBuild' }
    if ($ForceSwitch) { $args += '-Force' }
    if ($DisableLfeSwitch) { $args += '-DisableLfe' }
    $args += @('-PositionRadiusMode', $RadiusMode)
    $args += @('-PositionDirectionMode', $DirectionMode)
    $args += @('-AzimuthFocus', $Focus.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
    $args += @('-SpatialRenderer', $Renderer, '-HrtfEnvironment', $Environment)
    return ,$args
}

if ($SelfTest) {
    Assert-True ((Get-SafeStem 'Riptide.m4a') -eq 'Riptide') 'safe stem'
    Assert-True ((Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release').Count -ge 10) 'delegate base args'
    $disabled = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release' -DisableLfeSwitch
    Assert-True ($disabled -contains '-DisableLfe') 'DisableLfe delegate contract'
    $default = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release'
    Assert-True (-not ($default -contains '-DisableLfe')) 'default LFE delegate contract'
    $unit = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release' -RadiusMode 'unit'
    $front = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release' -DirectionMode 'front'
    $focus = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release' -Focus 0.5
    $hrtf = Get-DelegateArguments 'in.ec3' 'out' 8 'build-mm' 'Release' -DisableLfeSwitch -Renderer 'hrtf' -Environment 'outdoors'
    Assert-True (($unit -contains '-PositionRadiusMode') -and ($unit -contains 'unit')) 'unit position-radius delegate contract'
    Assert-True (-not ($default -contains 'unit')) 'default source position-radius delegate contract'
    Assert-True (($front -contains '-PositionDirectionMode') -and ($front -contains 'front')) 'front position-direction delegate contract'
    Assert-True (-not ($default -contains 'front')) 'default metadata position-direction delegate contract'
    Assert-True (($focus -contains '-AzimuthFocus') -and ($focus -contains '0.5')) 'azimuth-focus delegate contract'
    Assert-True (-not ($default -contains '0.5')) 'default zero azimuth-focus delegate contract'
    Assert-True (($hrtf -contains '-SpatialRenderer') -and ($hrtf -contains 'hrtf') -and ($hrtf -contains '-HrtfEnvironment') -and ($hrtf -contains 'outdoors')) 'HRTF delegate contract'
    Assert-True ((Quote-ProcessArgument 'C:\capture output\loopback.wav') -eq '"C:\capture output\loopback.wav"') 'space-containing process argument quoting'
    Assert-True ((Quote-ProcessArgument 'loopback.wav') -eq 'loopback.wav') 'simple process argument remains unquoted'
    Assert-True ((Get-AuditExitCode 'FAIL_SPATIAL_DELEGATE' '') -eq 1) 'delegate failure exit code'
    Assert-True ((Get-AuditExitCode 'INCONCLUSIVE_DRY_RUN' '') -eq 0) 'inconclusive report exit code'
    Assert-True ((Get-PowerShellExecutable) -and (Test-Path (Get-PowerShellExecutable))) 'PowerShell resolver'
    $captureTestLog = Join-Path ([IO.Path]::GetTempPath()) ('audioplayer-capture-{0}.log' -f [Guid]::NewGuid().ToString('N'))
    try {
        $captured = Invoke-Captured 'cmd.exe' @('/d', '/c', 'echo audit-stderr 1>&2') $captureTestLog
        Assert-True ($captured.ExitCode -eq 0 -and $captured.Text -match 'audit-stderr') 'native stderr capture under Stop policy'
    } finally {
        if (Test-Path -LiteralPath $captureTestLog) { Remove-Item -LiteralPath $captureTestLog -Force }
    }
    Write-Host 'auditAtmosSpatialSelfTest=PASS cases=17'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputPath)) { throw 'InputPath is required unless -SelfTest is used' }
if ($SpatialRenderer -eq 'hrtf' -and -not $DisableLfe) {
    throw 'SpatialRenderer hrtf requires -DisableLfe; HRTF backend does not submit a static LFE object'
}
$resolvedInput = Resolve-RepoPath $InputPath
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) { throw "Input file does not exist: $resolvedInput" }
$resolvedBuild = Resolve-RepoPath $BuildDir
$output = New-AuditDirectory $OutputRoot (Get-SafeStem $resolvedInput)
$runId = Split-Path -Leaf $output
$captureExe = Join-Path $resolvedBuild "$Configuration\WasapiLoopbackCapture.exe"
$spatialScript = Join-Path $repoRoot 'scripts\play-atmos-spatial.ps1'
$captureReport = Join-Path $output 'loopback.report.json'
$captureWav = Join-Path $output 'loopback.wav'
$readyFile = Join-Path $output 'loopback.ready.json'
$stopFile = Join-Path $output 'loopback.stop'
$captureLog = Join-Path $output 'loopback.stdout.txt'
$captureErrorLog = Join-Path $output 'loopback.stderr.txt'
$spatialLog = Join-Path $output 'spatial.delegate.txt'

$report = [ordered]@{
    schema = 'audioplayer.atmos-spatial-audit.v1'
    result = if ($Submit) { 'INCONCLUSIVE_NOT_YET_RUN' } else { 'INCONCLUSIVE_DRY_RUN' }
    evidenceLayer = 'orchestration-and-wasapi-loopback-sidecar'
    input = $resolvedInput
    inputReadOnly = $true
    maxAccessUnits = $MaxAccessUnits
    disableLfe = [bool]$DisableLfe
    positionRadiusMode = $PositionRadiusMode
    positionDirectionMode = $PositionDirectionMode
    azimuthFocus = $AzimuthFocus
    spatialRenderer = $SpatialRenderer
    hrtfEnvironment = $HrtfEnvironment
    hrtfPolicy = if ($SpatialRenderer -eq 'hrtf') { 'DISABLE_LFE_ONLY_DIAGNOSTIC' } else { 'NOT_APPLICABLE_STANDARD' }
    staticObjectTypeMask = if ($SpatialRenderer -eq 'hrtf') { 'None' } else { 'LowFrequency' }
    hrtfObjectPolicy = if ($SpatialRenderer -eq 'hrtf') { '15_DYNAMIC_ONLY' } else { 'STANDARD_DYNAMIC_PLUS_STATIC_LFE' }
    buildDir = $resolvedBuild
    configuration = $Configuration
    endpointSubmissionRequested = [bool]$Submit
    endpointSubmissionInvoked = $false
    actualEndpointSubmission = $false
    actualEndpointSubmissionResult = 'NOT_RUN'
    endpoint = $null
    loopback = [ordered]@{ started = $false; report = $captureReport; wav = $captureWav; readyFile = $readyFile; stopFile = $stopFile; exitCode = $null; cleanupForced = $false; reportResult = 'NOT_RUN'; analyzerExitCode = $null; analyzerReport = $null; analyzerResult = 'NOT_RUN' }
    limits = @('No endpoint is touched without -Submit', 'loopback clean result is INCONCLUSIVE by repository contract', 'no subjective listening claim')
}

if (-not $Submit) {
    $report.note = 'Dry orchestration only; no capture process, Spatial delegate, or audio endpoint was invoked.'
    $reportPath = Join-Path $output 'audit-report.json'
    Write-Utf8NoBom $reportPath ($report | ConvertTo-Json -Depth 10)
    Write-Host "auditAtmosSpatial=$($report.result)"
    Write-Host "report=$reportPath"
    exit 0
}

if (-not (Test-Path -LiteralPath $captureExe -PathType Leaf)) {
    $report.result = 'INCONCLUSIVE_CAPTURE_TOOL_MISSING'
    $report.note = "WasapiLoopbackCapture not found: $captureExe"
    Write-Utf8NoBom (Join-Path $output 'audit-report.json') ($report | ConvertTo-Json -Depth 10)
    throw $report.note
}
if (-not (Test-Path -LiteralPath $spatialScript -PathType Leaf)) { throw "Spatial delegate missing: $spatialScript" }

$captureArgs = @('--duration-ms', '30000', '--wav', $captureWav, '--report', $captureReport, '--ready-file', $readyFile, '--stop-file', $stopFile)
if (-not $NoBuild) {
    & cmake --build $resolvedBuild --config $Configuration --target WasapiLoopbackCapture Eac3AccessUnitProbe -- /m:4
    if ($LASTEXITCODE -ne 0) { throw 'Required diagnostic targets failed to build' }
}
$captureArgumentString = (($captureArgs | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join ' ')
$captureProcess = Start-Process -FilePath $captureExe -ArgumentList $captureArgumentString -PassThru -RedirectStandardOutput $captureLog -RedirectStandardError $captureErrorLog
$report.loopback.started = $true
$captureReady = $false
$operationError = $null
try {
    $readyDeadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path -LiteralPath $readyFile -PathType Leaf) -and (Get-Date) -lt $readyDeadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $readyFile -PathType Leaf)) {
        $report.result = 'INCONCLUSIVE_LOOPBACK_NOT_READY'
        throw 'Loopback capture did not produce ready file'
    }
    $captureReady = $true
    $ps = Get-PowerShellExecutable
    $delegateArgs = Get-DelegateArguments $resolvedInput $output $MaxAccessUnits $resolvedBuild $Configuration -NoBuildSwitch:$NoBuild -ForceSwitch:$Force -DisableLfeSwitch:$DisableLfe -RadiusMode:$PositionRadiusMode -DirectionMode:$PositionDirectionMode -Focus:$AzimuthFocus -Renderer:$SpatialRenderer -Environment:$HrtfEnvironment
    $spatial = Invoke-Captured $ps (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $spatialScript) + $delegateArgs) $spatialLog
    $report.endpointSubmissionInvoked = $true
    $report.spatialDelegateExitCode = $spatial.ExitCode
    if ($spatial.ExitCode -ne 0) { $report.result = 'FAIL_SPATIAL_DELEGATE'; $report.note = 'Spatial delegate failed; inspect spatial.delegate.txt' }
} catch {
    $operationError = $_.Exception.Message
    $report['operationError'] = $operationError
} finally {
    if (-not (Test-Path -LiteralPath $stopFile)) { Write-Utf8NoBom $stopFile 'stop' }
    if ($captureProcess -and -not $captureProcess.HasExited) { $captureProcess.WaitForExit(10000) | Out-Null }
    $report.loopback.cleanupForced = $false
    if ($captureProcess -and -not $captureProcess.HasExited) {
        Stop-Process -Id $captureProcess.Id -Force
        $report.loopback.cleanupForced = $true
    }
    $report.loopback.exitCode = if ($captureProcess -and $captureProcess.HasExited) { $captureProcess.ExitCode } else { $null }
}

if (Test-Path -LiteralPath $captureReport -PathType Leaf) {
    try {
        $loopback = Get-Content -Raw $captureReport | ConvertFrom-Json
        $report.loopback.reportResult = $loopback.result
        $report.endpoint = [ordered]@{
            id = if ($loopback.deviceId) { $loopback.deviceId } else { $null }
            name = if ($loopback.deviceName) { $loopback.deviceName } else { $null }
            provider = $null
            source = 'WasapiLoopbackCapture default eRender/eConsole endpoint'
        }
    } catch { $report.loopback.reportResult = 'UNPARSEABLE' }
}
$spatialProvenance = Join-Path $output 'spatial-provenance.json'
if (Test-Path -LiteralPath $spatialProvenance -PathType Leaf) {
    try {
        $provenance = Get-Content -Raw $spatialProvenance | ConvertFrom-Json
        if (-not $report.endpoint) { $report.endpoint = [ordered]@{} }
        $report.endpoint.provider = if ($provenance.provider) { $provenance.provider } else { $null }
        $report.actualEndpointSubmissionResult = if ($provenance.result) { $provenance.result } else { 'UNKNOWN' }
        $report.actualEndpointSubmission = ($provenance.result -eq 'PASS_ENDPOINT_SUBMISSION_ONLY' -and $report.spatialDelegateExitCode -eq 0)
        $report.spatialProvenance = [ordered]@{
            result = $provenance.result
            renderer = $provenance.spatialRenderer
            environment = $provenance.hrtfEnvironment
            interfaceAvailable = $provenance.interfaceAvailable
            hrtfDistanceDecay = $provenance.hrtfDistanceDecay
            preScaleSamples = $provenance.preScaleSamples
            preScaleRange = $provenance.preScaleRange
            lfePolicy = $provenance.lfePolicy
            lfeVolume = $provenance.lfeVolume
        }
    } catch { $report.actualEndpointSubmissionResult = 'UNPARSEABLE' }
} else {
    $report.actualEndpointSubmissionResult = 'NO_PROVENANCE'
    $report.actualEndpointSubmission = $false
}
$analyzerFailed = $false
if ($captureReady -and (Get-Command python.exe -ErrorAction SilentlyContinue)) {
    $analyzer = Join-Path $repoRoot 'tools\atmos-render\analyze_loopback_wav.py'
    $analyzerReport = Join-Path $output 'loopback-analysis.json'
    if (Test-Path -LiteralPath $captureWav -PathType Leaf) {
        $analysis = Invoke-Captured 'python.exe' @($analyzer, $captureWav, '--output', $analyzerReport) (Join-Path $output 'loopback-analyzer.txt')
        $report.loopback.analyzerExitCode = $analysis.ExitCode
        $report.loopback.analyzerReport = $analyzerReport
        if ($analysis.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $analyzerReport -PathType Leaf)) {
            $analyzerFailed = $true
            $report.loopback.analyzerResult = 'UNPARSEABLE_OR_NONZERO'
        } else {
            try {
                $analysisJson = Get-Content -Raw $analyzerReport | ConvertFrom-Json
                $report.loopback.analyzerResult = $analysisJson.result
            } catch {
                $analyzerFailed = $true
                $report.loopback.analyzerResult = 'UNPARSEABLE'
            }
        }
    } else {
        $analyzerFailed = $true
        $report.loopback.analyzerResult = 'WAV_MISSING'
    }
} elseif ($captureReady) {
    $analyzerFailed = $true
    $report.loopback.analyzerResult = 'PYTHON_MISSING'
}
if ($analyzerFailed) {
    $report.result = 'INCONCLUSIVE_ANALYZER_FAILED'
} elseif ($report.result -eq 'INCONCLUSIVE_NOT_YET_RUN') {
    $report.result = 'INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN'
}
$report['operationError'] = $operationError
$report.note = 'Spatial submission and loopback are sidecars; endpoint output quality requires synchronized loopback interpretation and remains INCONCLUSIVE without a stronger acceptance oracle.'
$reportPath = Join-Path $output 'audit-report.json'
Write-Utf8NoBom $reportPath ($report | ConvertTo-Json -Depth 10)
Write-Host "auditAtmosSpatial=$($report.result)"
Write-Host "report=$reportPath"
exit (Get-AuditExitCode $report.result $operationError)
