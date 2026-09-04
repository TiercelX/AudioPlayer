param(
    [string]$JsonlDiagnosticFile = "",
    [string]$HarnessReportFile = "",
    [string]$LoopbackReportFile,
    [string]$ReportFile = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common-paths.ps1")

function Resolve-OptionalFile {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $resolved = Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $Path
    if (-not (Test-Path $resolved -PathType Leaf)) {
        throw "File not found: $resolved"
    }
    return $resolved
}

function Read-JsonFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    return Get-Content -Path $Path -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Get-JsonProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function ConvertTo-DateTimeOffsetOrNull {
    param([object]$Value)

    if ($null -eq $Value) {
        return $null
    }
    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $null
    }
    try {
        return [DateTimeOffset]::Parse($text, [System.Globalization.CultureInfo]::InvariantCulture)
    } catch {
        return $null
    }
}

function Get-MillisecondsBetween {
    param(
        [DateTimeOffset]$Start,
        [DateTimeOffset]$End
    )

    return [int][Math]::Round(($End - $Start).TotalMilliseconds)
}

function Test-MessageCritical {
    param([string]$Message)

    return $Message -match "activeOutputSwitch|sameOutputInvalidation|WaitingForInvalidation|absorbed-output|conservative-rebuild|startPipeline|configureOutput|firstDataBlockAfterConfigure|renderMirror|setPlaybackState|action=|quit fired|aboutToQuit|audioState|sink_state"
}

function Get-EventLabel {
    param([object]$Entry)

    $eventName = [string](Get-JsonProperty -Object $Entry -Name "event")
    $category = [string](Get-JsonProperty -Object $Entry -Name "category")
    $message = [string](Get-JsonProperty -Object $Entry -Name "message")

    if ($eventName -ne "log" -and -not [string]::IsNullOrWhiteSpace($eventName)) {
        return $eventName
    }
    if ([string]::IsNullOrWhiteSpace($message)) {
        return $category
    }

    if ($message -match "^(\S+)") {
        return $Matches[1]
    }
    return $category
}

function Test-StructuredCriticalEvent {
    param([object]$Entry)

    $eventName = [string](Get-JsonProperty -Object $Entry -Name "event")
    if ([string]::IsNullOrWhiteSpace($eventName) -or $eventName -eq "log") {
        return $false
    }
    if ($eventName -match "active|switch|sink|backend|error|recovery|render_mirror|glitch|stale|underrun|starvation|source") {
        return $true
    }
    return $false
}

function ConvertTo-TimelineEvent {
    param(
        [object]$Entry,
        [DateTimeOffset]$RunStart
    )

    $timestamp = ConvertTo-DateTimeOffsetOrNull -Value (Get-JsonProperty -Object $Entry -Name "timestamp")
    if ($null -eq $timestamp) {
        return $null
    }

    $message = [string](Get-JsonProperty -Object $Entry -Name "message")
    $eventName = [string](Get-JsonProperty -Object $Entry -Name "event")
    $category = [string](Get-JsonProperty -Object $Entry -Name "category")
    $critical = (Test-StructuredCriticalEvent -Entry $Entry) -or (Test-MessageCritical -Message $message)
    if (-not $critical) {
        return $null
    }

    return [ordered]@{
        timestamp = $timestamp.UtcDateTime.ToString("o")
        offsetMsFromRunStart = Get-MillisecondsBetween -Start $RunStart -End $timestamp
        category = $category
        event = $eventName
        label = Get-EventLabel -Entry $Entry
        critical = $critical
        message = $message
    }
}

function Find-Coverage {
    param(
        [DateTimeOffset]$Timestamp,
        [object[]]$Segments,
        [DateTimeOffset]$CaptureStart,
        [DateTimeOffset]$CaptureEnd
    )

    if ($null -eq $CaptureStart -or $null -eq $CaptureEnd) {
        return [ordered]@{
            status = "no-loopback-capture-window"
            segmentIndex = $null
            interruptedSegment = $false
        }
    }
    if ($Timestamp -lt $CaptureStart) {
        return [ordered]@{
            status = "before-capture"
            segmentIndex = $null
            interruptedSegment = $false
        }
    }
    if ($Timestamp -gt $CaptureEnd) {
        return [ordered]@{
            status = "after-capture"
            segmentIndex = $null
            interruptedSegment = $false
        }
    }

    foreach ($segment in $Segments) {
        $started = ConvertTo-DateTimeOffsetOrNull -Value $segment.startedUtc
        $ended = ConvertTo-DateTimeOffsetOrNull -Value $segment.endedUtc
        if ($null -eq $started -or $null -eq $ended) {
            continue
        }
        if ($Timestamp -ge $started -and $Timestamp -le $ended) {
            return [ordered]@{
                status = "covered-by-segment"
                segmentIndex = [int]$segment.segmentIndex
                interruptedSegment = [bool]$segment.interrupted
            }
        }
    }

    return [ordered]@{
        status = "capture-gap"
        segmentIndex = $null
        interruptedSegment = $false
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$harnessReportPath = Resolve-OptionalFile -RepoRoot $repoRoot -Path $HarnessReportFile
$harnessReport = Read-JsonFile -Path $harnessReportPath

$jsonlPath = Resolve-OptionalFile -RepoRoot $repoRoot -Path $JsonlDiagnosticFile
if ([string]::IsNullOrWhiteSpace($jsonlPath) -and $null -ne $harnessReport) {
    $files = Get-JsonProperty -Object $harnessReport -Name "files"
    $jsonlPath = Resolve-OptionalFile -RepoRoot $repoRoot -Path ([string](Get-JsonProperty -Object $files -Name "jsonlDiagnosticFile"))
}
if ([string]::IsNullOrWhiteSpace($jsonlPath)) {
    throw "JsonlDiagnosticFile or HarnessReportFile with files.jsonlDiagnosticFile is required"
}

$loopbackReportPath = Resolve-OptionalFile -RepoRoot $repoRoot -Path $LoopbackReportFile
if ([string]::IsNullOrWhiteSpace($loopbackReportPath)) {
    throw "LoopbackReportFile is required"
}
$loopbackReport = Read-JsonFile -Path $loopbackReportPath

$reportPath = if ([string]::IsNullOrWhiteSpace($ReportFile)) {
    [System.IO.Path]::ChangeExtension($loopbackReportPath, ".alignment.report.json")
} else {
    Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $ReportFile
}

$entries = [System.Collections.Generic.List[object]]::new()
foreach ($line in Get-Content -Path $jsonlPath -Encoding UTF8) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }
    try {
        $entries.Add(($line | ConvertFrom-Json)) | Out-Null
    } catch {
        continue
    }
}
if ($entries.Count -eq 0) {
    throw "No parseable JSONL diagnostic entries found: $jsonlPath"
}

$timestamps = @($entries | ForEach-Object {
    ConvertTo-DateTimeOffsetOrNull -Value (Get-JsonProperty -Object $_ -Name "timestamp")
} | Where-Object { $null -ne $_ } | Sort-Object)
$runStart = if ($timestamps.Count -gt 0) { $timestamps[0] } else { [DateTimeOffset]::UtcNow }
$runEnd = if ($timestamps.Count -gt 0) { $timestamps[-1] } else { $runStart }

$captureStart = ConvertTo-DateTimeOffsetOrNull -Value $loopbackReport.captureStartedUtc
$captureEnd = ConvertTo-DateTimeOffsetOrNull -Value $loopbackReport.captureEndedUtc
$segments = @($loopbackReport.segments)
$timelineEvents = [System.Collections.Generic.List[object]]::new()
foreach ($entry in $entries) {
    $event = ConvertTo-TimelineEvent -Entry $entry -RunStart $runStart
    if ($null -eq $event) {
        continue
    }
    $eventTimestamp = ConvertTo-DateTimeOffsetOrNull -Value $event.timestamp
    $coverage = Find-Coverage -Timestamp $eventTimestamp -Segments $segments -CaptureStart $captureStart -CaptureEnd $captureEnd
    $event.coverageStatus = $coverage.status
    $event.loopbackSegmentIndex = $coverage.segmentIndex
    $event.loopbackSegmentInterrupted = $coverage.interruptedSegment
    $timelineEvents.Add($event) | Out-Null
}

$segmentSummaries = @(
    foreach ($segment in $segments) {
        $started = ConvertTo-DateTimeOffsetOrNull -Value $segment.startedUtc
        $ended = ConvertTo-DateTimeOffsetOrNull -Value $segment.endedUtc
        $segmentEvents = @($timelineEvents | Where-Object { $_.loopbackSegmentIndex -eq [int]$segment.segmentIndex })
        [ordered]@{
            segmentIndex = [int]$segment.segmentIndex
            startedUtc = if ($null -ne $started) { $started.UtcDateTime.ToString("o") } else { "" }
            endedUtc = if ($null -ne $ended) { $ended.UtcDateTime.ToString("o") } else { "" }
            offsetStartMsFromRunStart = if ($null -ne $started) { Get-MillisecondsBetween -Start $runStart -End $started } else { $null }
            offsetEndMsFromRunStart = if ($null -ne $ended) { Get-MillisecondsBetween -Start $runStart -End $ended } else { $null }
            interrupted = [bool]$segment.interrupted
            interruptionReason = [string]$segment.interruptionReason
            interruptionHresult = [string]$segment.interruptionHresult
            transientCandidateCount = [int]$segment.transientCandidateCount
            criticalEventCount = $segmentEvents.Count
            criticalEventLabels = @($segmentEvents | Select-Object -First 20 | ForEach-Object { $_.label })
        }
    }
)

$coverageCounts = [ordered]@{}
foreach ($event in $timelineEvents) {
    $status = [string]$event.coverageStatus
    if (-not $coverageCounts.Contains($status)) {
        $coverageCounts[$status] = 0
    }
    $coverageCounts[$status] += 1
}
$interruptedSegmentEventCount = @($timelineEvents | Where-Object { $_.loopbackSegmentInterrupted }).Count
$captureGapEventCount = @($timelineEvents | Where-Object { $_.coverageStatus -eq "capture-gap" }).Count
$uncoveredEventCount = @($timelineEvents | Where-Object {
    $_.coverageStatus -ne "covered-by-segment"
}).Count

$alignmentReport = [ordered]@{
    schemaVersion = 1
    result = "INCONCLUSIVE"
    evidenceLayer = "jsonl-loopback-time-alignment"
    verificationLayer = "evidence-alignment-only"
    endpointOutputVerified = $false
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    inputs = [ordered]@{
        jsonlDiagnosticFile = $jsonlPath
        harnessReportFile = $harnessReportPath
        loopbackReportFile = $loopbackReportPath
    }
    appTimeline = [ordered]@{
        startedUtc = $runStart.UtcDateTime.ToString("o")
        endedUtc = $runEnd.UtcDateTime.ToString("o")
        durationMs = Get-MillisecondsBetween -Start $runStart -End $runEnd
        parsedEntryCount = $entries.Count
        criticalEventCount = $timelineEvents.Count
    }
    loopbackCapture = [ordered]@{
        result = [string]$loopbackReport.result
        captureStartedUtc = if ($null -ne $captureStart) { $captureStart.UtcDateTime.ToString("o") } else { "" }
        captureEndedUtc = if ($null -ne $captureEnd) { $captureEnd.UtcDateTime.ToString("o") } else { "" }
        offsetStartMsFromRunStart = if ($null -ne $captureStart) { Get-MillisecondsBetween -Start $runStart -End $captureStart } else { $null }
        offsetEndMsFromRunStart = if ($null -ne $captureEnd) { Get-MillisecondsBetween -Start $runStart -End $captureEnd } else { $null }
        captureInterrupted = [bool]$loopbackReport.captureInterrupted
        interruptionReason = [string]$loopbackReport.interruptionReason
        interruptionHresult = [string]$loopbackReport.interruptionHresult
        segmentCount = [int]$loopbackReport.segmentCount
        transientCandidateCount = [int]$loopbackReport.transientCandidateCount
        dropoutCandidateCount = [int]$loopbackReport.dropoutCandidateCount
        tailFadeCandidateObserved = [bool]$loopbackReport.tailFadeCandidateObserved
        trailingSilenceDurationMs = $loopbackReport.trailingSilenceDurationMs
    }
    coverage = [ordered]@{
        countsByStatus = $coverageCounts
        coveredCriticalEventCount = @($timelineEvents | Where-Object { $_.coverageStatus -eq "covered-by-segment" }).Count
        uncoveredCriticalEventCount = $uncoveredEventCount
        interruptedSegmentCriticalEventCount = $interruptedSegmentEventCount
        captureGapCriticalEventCount = $captureGapEventCount
        criticalWindowInterrupted = $interruptedSegmentEventCount -gt 0 -or $captureGapEventCount -gt 0
    }
    segments = @($segmentSummaries)
    timelineEvents = @($timelineEvents)
    interpretation = [ordered]@{
        canClaimNoPop = $false
        limitation = "This report only aligns app diagnostics with loopback capture windows. It does not prove endpoint output is pop-free, and loopback detector silence remains inconclusive when capture is interrupted or evidence is below the physical output layer."
    }
}

$reportDir = Split-Path -Parent $reportPath
if (-not (Test-Path $reportDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $reportDir -Force
}
$alignmentReport | ConvertTo-Json -Depth 12 | Set-Content -Path $reportPath -Encoding UTF8

Write-Output "alignmentReport:$reportPath"
Write-Output "criticalEvents:$($timelineEvents.Count)"
Write-Output "coveredCriticalEvents:$($alignmentReport.coverage.coveredCriticalEventCount)"
Write-Output "uncoveredCriticalEvents:$($alignmentReport.coverage.uncoveredCriticalEventCount)"
Write-Output "criticalWindowInterrupted:$($alignmentReport.coverage.criticalWindowInterrupted)"
