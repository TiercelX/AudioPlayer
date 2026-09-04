param(
    [string]$BuildDir = "",
    [int]$Recent = 20,
    [int]$SeekResumeBoundaryWindowMs = 250,
    [string]$LogPath = "",
    [string]$ReportFile = "",
    [switch]$RawRecent,
    [switch]$RulesOnly
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common-paths.ps1")

function Resolve-PlayerLog {
    param(
        [string]$RepoRoot,
        [string]$BuildDir,
        [string]$LogPath
    )

    $BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir

    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        $candidate = if ([System.IO.Path]::IsPathRooted($LogPath)) {
            $LogPath
        } else {
            Join-Path $RepoRoot $LogPath
        }
        $resolved = [System.IO.Path]::GetFullPath($candidate)
        if (-not (Test-Path $resolved -PathType Leaf)) {
            throw "Log file not found: $resolved"
        }
        return (Get-Item -Path $resolved)
    }

    $logDir = Resolve-AudioPlayerLogDir -RepoRoot $RepoRoot -BuildDir $BuildDir
    if (-not (Test-Path $logDir -PathType Container)) {
        throw "Log directory not found: $logDir"
    }

    $latestLog = Get-ChildItem -Path $logDir -Filter "player-*.log" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latestLog) {
        throw "No player log files found in $logDir"
    }
    return $latestLog
}

function ConvertFrom-KeyValuePayload {
    param(
        [string]$Line,
        [string]$Marker
    )

    $markerIndex = $Line.IndexOf($Marker)
    if ($markerIndex -lt 0) {
        return $null
    }

    $payload = $Line.Substring($markerIndex + $Marker.Length)
    $matches = [regex]::Matches($payload, '(?<key>[A-Za-z][A-Za-z0-9]*)=')
    if ($matches.Count -eq 0) {
        return $null
    }

    $fields = [ordered]@{
        rawLine = $Line
    }
    for ($i = 0; $i -lt $matches.Count; ++$i) {
        $key = $matches[$i].Groups["key"].Value
        $valueStart = $matches[$i].Index + $matches[$i].Length
        $valueEnd = if ($i + 1 -lt $matches.Count) {
            $matches[$i + 1].Index
        } else {
            $payload.Length
        }
        $fields[$key] = $payload.Substring($valueStart, $valueEnd - $valueStart).Trim()
    }
    return $fields
}

function Get-Field {
    param(
        [hashtable]$Item,
        [string]$Name,
        [string]$Default = "none"
    )

    if ($null -ne $Item -and $Item.Contains($Name) -and -not [string]::IsNullOrWhiteSpace($Item[$Name])) {
        return [string]$Item[$Name]
    }
    return $Default
}

function Get-Flag {
    param(
        [hashtable]$Item,
        [string]$Name
    )

    $value = (Get-Field -Item $Item -Name $Name -Default "0").ToLowerInvariant()
    return $value -eq "1" -or $value -eq "true"
}

function Get-IntField {
    param(
        [hashtable]$Item,
        [string]$Name,
        [int]$Default = 0
    )

    $parsed = 0
    if ([int]::TryParse((Get-Field -Item $Item -Name $Name -Default ""), [ref]$parsed)) {
        return $parsed
    }
    return $Default
}

function Test-SeekResumeProfile {
    param([string]$Value)

    return $Value -eq "SeekResume" -or $Value -eq "SeekRestart"
}

function Test-KnownSyntheticFixture {
    param([string]$Source)

    $name = [System.IO.Path]::GetFileName($Source).ToLowerInvariant()
    return $name -eq "smoke.wav" -or
        $name -eq "smoke.flac" -or
        $name -eq "smoke.mp3" -or
        $name -eq "smoke.aac" -or
        $name -eq "smoke.m4a" -or
        $name -eq "smoke-alac.m4a" -or
        $name.StartsWith("sine-") -or
        $name.StartsWith("silence-") -or
        $name.StartsWith("ab-sine-") -or
        $name.StartsWith("ab-pink-noise-")
}

function Test-CompressedContentSample {
    param([hashtable]$Artifact)

    $source = Get-Field -Item $Artifact -Name "source" -Default ""
    if ([string]::IsNullOrWhiteSpace($source) -or (Test-KnownSyntheticFixture -Source $source)) {
        return $false
    }

    $name = [System.IO.Path]::GetFileName($source).ToLowerInvariant()
    if ($name -eq "real-alac-sample.m4a") {
        return $true
    }

    $lowerSource = $source.ToLowerInvariant()
    $compressedExtension = $name.EndsWith(".m4a") -or
        $name.EndsWith(".aac") -or
        $name.EndsWith(".mp3") -or
        $name.EndsWith(".flac") -or
        $name.EndsWith(".eb3") -or
        $name.EndsWith(".mlp")
    return $compressedExtension -and
        ($lowerSource.Contains("/media/") -or $lowerSource.Contains("\media\"))
}

function Test-ContentTransientDetector {
    param([string]$Detector)

    return $Detector -eq "transient-spike" -or
        $Detector -eq "short-burst" -or
        $Detector -eq "crackle-texture" -or
        $Detector -eq "sample-jump"
}

function Test-HardArtifactEvidence {
    param([hashtable]$Artifact)

    $detector = Get-Field -Item $Artifact -Name "detector"
    $severity = Get-Field -Item $Artifact -Name "severity"
    $jump = Get-DoubleField -Item $Artifact -Name "jump"
    return $detector -eq "invalid-sample" -or
        $detector -eq "out-of-range-sample" -or
        $detector -eq "block-boundary-discontinuity" -or
        $detector -eq "silence-hard-switch" -or
        ($detector -eq "sample-jump" -and ($severity -eq "critical" -or $jump -ge 1.50))
}

function Get-DoubleField {
    param(
        [hashtable]$Item,
        [string]$Name,
        [double]$Default = 0.0
    )

    $parsed = 0.0
    $style = [System.Globalization.NumberStyles]::Float
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    if ([double]::TryParse((Get-Field -Item $Item -Name $Name -Default ""), $style, $culture, [ref]$parsed)) {
        return $parsed
    }
    return $Default
}

function Format-DoubleValue {
    param([double]$Value)

    return $Value.ToString("0.0000", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-MaxDoubleFieldText {
    param(
        [object[]]$Items,
        [string]$Name
    )

    $values = @($Items | ForEach-Object {
        Get-DoubleField -Item $_ -Name $Name -Default ([double]::NaN)
    } | Where-Object { -not [double]::IsNaN($_) })
    if ($values.Count -eq 0) {
        return "none"
    }

    return (Format-DoubleValue -Value (($values | Measure-Object -Maximum).Maximum))
}

function Get-SeekResumeOffsetMsAfterResume {
    param(
        [hashtable]$Artifact,
        [hashtable]$SessionStartPositions
    )

    if ($null -eq $SessionStartPositions) {
        return $null
    }

    $session = Get-IntField -Item $Artifact -Name "session" -Default 0
    if ($session -le 0 -or -not $SessionStartPositions.ContainsKey($session)) {
        return $null
    }

    $positionMs = Get-IntField -Item $Artifact -Name "positionMs" -Default -1
    if ($positionMs -lt 0) {
        return $null
    }

    return $positionMs - [int]$SessionStartPositions[$session]
}

function Get-ControlEventBucket {
    param([hashtable]$Item)

    $event = Get-Field -Item $Item -Name "recentControlEvent"
    $separator = $event.LastIndexOf("@")
    if ($separator -gt 0) {
        return $event.Substring(0, $separator)
    }
    return $event
}

function Get-StartupProfile {
    param([hashtable]$Item)

    $profile = Get-Field -Item $Item -Name "pipelineStartProfile"
    if ($profile -eq "none") {
        $profile = Get-Field -Item $Item -Name "startupProfile"
    }
    return $profile
}

function Get-ArtifactPath {
    param([hashtable]$Item)

    $path = Get-Field -Item $Item -Name "artifactPath"
    if ($path -ne "none") {
        return $path
    }

    return Get-StartupProfile -Item $Item
}

function Get-CountTable {
    param(
        [object[]]$Items,
        [scriptblock]$KeySelector
    )

    $counts = @{}
    foreach ($item in $Items) {
        $key = & $KeySelector $item
        if ([string]::IsNullOrWhiteSpace($key)) {
            $key = "none"
        }
        if (-not $counts.ContainsKey($key)) {
            $counts[$key] = 0
        }
        $counts[$key] += 1
    }
    return $counts
}

function ConvertTo-CountRows {
    param([hashtable]$Counts)

    $rows = [System.Collections.Generic.List[object]]::new()
    $Counts.GetEnumerator() |
        Sort-Object @{ Expression = "Value"; Descending = $true }, @{ Expression = "Name"; Ascending = $true } |
        ForEach-Object {
            $rows.Add([ordered]@{
                name = [string]$_.Name
                count = [int]$_.Value
            }) | Out-Null
        }
    return ,($rows.ToArray())
}

function Write-CountTable {
    param(
        [string]$Title,
        [hashtable]$Counts
    )

    Write-Output ""
    Write-Output $Title
    if ($Counts.Count -eq 0) {
        Write-Output "  none"
        return
    }

    $Counts.GetEnumerator() |
        Sort-Object @{ Expression = "Value"; Descending = $true }, @{ Expression = "Name"; Ascending = $true } |
        ForEach-Object {
            Write-Output ("  {0,5}  {1}" -f $_.Value, $_.Name)
        }
}

function Get-PriorityHintText {
    param([hashtable]$ClassCounts)

    if ($ClassCounts.ContainsKey("active-switch-rebuild-boundary") -or
        $ClassCounts.ContainsKey("active-switch-rebuild-path")) {
        return "Start with the ActiveSwitchRebuild startup boundary and surrounding activeOutputSwitch logs."
    }
    if ($ClassCounts.ContainsKey("error-recovery-boundary") -or $ClassCounts.ContainsKey("error-recovery-path")) {
        return "Start with the ErrorRecovery startup boundary and outputRecovery logs."
    }
    if ($ClassCounts.ContainsKey("seek-resume-boundary")) {
        return "Start with the SeekResume startup boundary window and nearby stale/discontinuity evidence."
    }
    if ($ClassCounts.ContainsKey("seek-resume-content-transient-likely")) {
        return "Treat later SeekResume hits on compressed music as content-transient leads unless manual audibility or boundary evidence says otherwise."
    }
    if ($ClassCounts.ContainsKey("seek-resume-detector-inconclusive")) {
        return "Inspect SeekResume offsets, detector types, and whether the source is a synthetic fixture or compressed music."
    }
    if ($ClassCounts.ContainsKey("normal-start-boundary") -or $ClassCounts.ContainsKey("normal-start-path")) {
        return "Start with the NormalStart startup boundary and firstDataBlockAfterConfigure logs."
    }
    if ($ClassCounts.ContainsKey("boundary-discontinuity")) {
        return "Start with the render boundary samples and nearby control events."
    }
    if ($ClassCounts.ContainsKey("sustained-output-chain-anomaly")) {
        return "Start with the output write loop, padding/available-frame cadence, and render continuity."
    }
    if ($ClassCounts.ContainsKey("source-or-decoder-anomaly")) {
        return "Start with source probing, decoder output format, and PCM data validity."
    }
    return "No strong artifact class. Inspect the raw recent artifact lines and nearby output events."
}

function Get-ArtifactClass {
    param(
        [hashtable]$Artifact,
        [hashtable]$SessionStartPositions = $null,
        [int]$BoundaryWindowMs = 250
    )

    $detector = Get-Field -Item $Artifact -Name "detector"
    $profile = Get-StartupProfile -Item $Artifact
    $path = Get-ArtifactPath -Item $Artifact
    $control = Get-ControlEventBucket -Item $Artifact
    $density = Get-DoubleField -Item $Artifact -Name "artifactDensityPerMinute"
    $recovery = (Get-Flag -Item $Artifact -Name "recovery") -or
        (Get-Flag -Item $Artifact -Name "recoveryPending") -or
        (Get-IntField -Item $Artifact -Name "recoveryAttempt") -gt 0 -or
        $control.StartsWith("output-recovery")
    $firstDataBlock = Get-Flag -Item $Artifact -Name "firstDataBlockAfterConfigure"
    $warmupOrSilence = (Get-Flag -Item $Artifact -Name "warmup") -or (Get-Flag -Item $Artifact -Name "silenceFill")
    $boundaryDetector = $detector -eq "block-boundary-discontinuity" -or $detector -eq "silence-hard-switch"
    $sourceDetector = $detector -eq "invalid-sample" -or $detector -eq "out-of-range-sample" -or
        $detector -eq "transient-spike" -or $detector -eq "short-burst" -or
        $detector -eq "crackle-texture"
    $suppressed = Get-IntField -Item $Artifact -Name "suppressedDuplicateArtifacts"
    $startupBoundary = $firstDataBlock -or $warmupOrSilence -or $boundaryDetector
    $trackedPath = "none"

    if ($profile -eq "ActiveSwitchRebuild" -or $path -eq "ActiveSwitchRebuild") {
        $trackedPath = "ActiveSwitchRebuild"
    } elseif ($profile -eq "ErrorRecovery" -or $path -eq "ErrorRecovery" -or $recovery) {
        $trackedPath = "ErrorRecovery"
    } elseif ($profile -eq "SeekResume" -or $path -eq "SeekResume") {
        $trackedPath = "SeekResume"
    } elseif ($profile -eq "SeekRestart" -or $path -eq "SeekRestart") {
        $trackedPath = "SeekRestart"
    } elseif ($profile -eq "NormalStart" -or $path -eq "NormalStart") {
        $trackedPath = "NormalStart"
    } elseif ($profile -ne "none") {
        $trackedPath = $profile
    }

    if ($trackedPath -eq "SeekResume" -or $trackedPath -eq "SeekRestart") {
        $offsetMs = Get-SeekResumeOffsetMsAfterResume -Artifact $Artifact -SessionStartPositions $SessionStartPositions
        $hardEvidence = Test-HardArtifactEvidence -Artifact $Artifact
        if ($null -ne $offsetMs -and $offsetMs -ge 0 -and $offsetMs -le $BoundaryWindowMs) {
            return "seek-resume-boundary"
        }
        if ((Test-CompressedContentSample -Artifact $Artifact) -and
            -not $hardEvidence -and
            (Test-ContentTransientDetector -Detector $detector)) {
            return "seek-resume-content-transient-likely"
        }
        return "seek-resume-detector-inconclusive"
    }

    if ($startupBoundary) {
        if ($trackedPath -eq "ActiveSwitchRebuild") {
            return "active-switch-rebuild-boundary"
        }
        if ($trackedPath -eq "ErrorRecovery") {
            return "error-recovery-boundary"
        }
        if ($trackedPath -eq "NormalStart") {
            return "normal-start-boundary"
        }
    }

    if ($boundaryDetector) {
        return "boundary-discontinuity"
    }
    if ($density -ge 6.0 -or $suppressed -gt 0) {
        return "sustained-output-chain-anomaly"
    }
    if ($sourceDetector -or (($detector -eq "sample-jump") -and -not $firstDataBlock)) {
        return "source-or-decoder-anomaly"
    }
    if ($trackedPath -eq "ActiveSwitchRebuild") {
        return "active-switch-rebuild-path"
    }
    if ($trackedPath -eq "ErrorRecovery") {
        return "error-recovery-path"
    }
    if ($trackedPath -eq "NormalStart") {
        return "normal-start-path"
    }
    if ($trackedPath -ne "none") {
        return "profile-path-anomaly"
    }
    return "unclassified"
}

function Write-AttributionRules {
    Write-Output "Attribution rules:"
    Write-Output "  active-switch-rebuild-boundary: pipelineStartProfile/artifactPath is ActiveSwitchRebuild and the artifact lands on firstDataBlockAfterConfigure, silenceFill/warmup, or a boundary detector."
    Write-Output "  error-recovery-boundary: pipelineStartProfile/artifactPath is ErrorRecovery, or recovery context is active, and the artifact lands on firstDataBlockAfterConfigure, silenceFill/warmup, or a boundary detector."
    Write-Output "  seek-resume-boundary: pipelineStartProfile/artifactPath is SeekResume or SeekRestart and the artifact lands within the immediate seek-resume boundary window."
    Write-Output "  seek-resume-content-transient-likely: compressed real content has source-shaped transient detectors outside the seek-resume boundary window, without hard discontinuity/stale evidence."
    Write-Output "  seek-resume-detector-inconclusive: SeekResume/SeekRestart artifact lacks enough boundary-window or content-transient evidence for a hard classification."
    Write-Output "  normal-start-boundary: pipelineStartProfile/artifactPath is NormalStart and the artifact lands on firstDataBlockAfterConfigure, silenceFill/warmup, or a boundary detector."
    Write-Output "  active-switch-boundary-pop-candidate: activeSwitchBoundaryPopCandidate lines are boundary-envelope leads for endpoint/manual pop investigation, not proof of endpoint output."
    Write-Output "  boundary-discontinuity: detector is block-boundary-discontinuity or silence-hard-switch outside the tracked startup profiles."
    Write-Output "  source-or-decoder-anomaly: invalid/out-of-range/transient/sample-jump/short-burst/crackle-texture appears away from the tracked startup boundary."
    Write-Output "  sustained-output-chain-anomaly: artifactDensityPerMinute is high or suppressedDuplicateArtifacts is nonzero."
}

function Write-PriorityHint {
    param([hashtable]$ClassCounts)

    Write-Output ""
    Write-Output "Priority hint:"
    Write-Output ("  {0}" -f (Get-PriorityHintText -ClassCounts $ClassCounts))
}

function Resolve-AnalyzerReportPath {
    param(
        [string]$RepoRoot,
        [string]$ReportFile
    )

    if ([string]::IsNullOrWhiteSpace($ReportFile)) {
        return ""
    }
    return Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $ReportFile
}

function Select-ArtifactSummary {
    param(
        [hashtable]$Artifact,
        [hashtable]$SessionStartPositions,
        [int]$BoundaryWindowMs
    )

    $seekOffset = Get-SeekResumeOffsetMsAfterResume -Artifact $Artifact -SessionStartPositions $SessionStartPositions
    return [ordered]@{
        timestamp = Get-Field -Item $Artifact -Name "timestamp" -Default ""
        detector = Get-Field -Item $Artifact -Name "detector"
        severity = Get-Field -Item $Artifact -Name "severity"
        class = Get-ArtifactClass -Artifact $Artifact -SessionStartPositions $SessionStartPositions -BoundaryWindowMs $BoundaryWindowMs
        pipelineStartProfile = Get-StartupProfile -Item $Artifact
        artifactPath = Get-ArtifactPath -Item $Artifact
        recentControlEvent = Get-ControlEventBucket -Item $Artifact
        playbackState = Get-Field -Item $Artifact -Name "playbackState"
        positionMs = Get-Field -Item $Artifact -Name "positionMs"
        seekOffsetMsAfterResume = $seekOffset
        jump = Get-Field -Item $Artifact -Name "jump"
        peak = Get-Field -Item $Artifact -Name "peak"
        writeFrames = Get-Field -Item $Artifact -Name "writeFrames"
        wasapiPaddingFrames = Get-Field -Item $Artifact -Name "wasapiPaddingFrames"
        wasapiAvailableFrames = Get-Field -Item $Artifact -Name "wasapiAvailableFrames"
        rawLine = Get-Field -Item $Artifact -Name "rawLine" -Default ""
    }
}

function Write-AnalyzerJsonReport {
    param(
        [string]$Path,
        [object]$Report
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $reportDir = Split-Path -Parent $Path
    if (-not (Test-Path $reportDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $reportDir -Force
    }
    $Report | ConvertTo-Json -Depth 12 | Set-Content -Path $Path -Encoding UTF8
}

if ($RulesOnly) {
    Write-AttributionRules
    return
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$log = Resolve-PlayerLog -RepoRoot $repoRoot -BuildDir $BuildDir -LogPath $LogPath
$artifactLines = Select-String -Path $log.FullName -Pattern "\[anomaly\].*audioArtifact " |
    ForEach-Object { $_.Line }
$artifacts = @($artifactLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "audioArtifact "
} | Where-Object { $null -ne $_ })

$startupLines = Select-String -Path $log.FullName -Pattern "startPipeline output .*pipelineStartProfile=" |
    ForEach-Object { $_.Line }
$startupEvents = @($startupLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "startPipeline output "
} | Where-Object { $null -ne $_ })
$activeSwitchPolicyLines = Select-String -Path $log.FullName -Pattern "activeSwitchBoundaryPolicy name=" |
    ForEach-Object { $_.Line }
$activeSwitchPolicyEvents = @($activeSwitchPolicyLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "activeSwitchBoundaryPolicy "
} | Where-Object { $null -ne $_ })
$activeSwitchPreflightLines = Select-String -Path $log.FullName -Pattern "activeSwitchPreflight " |
    ForEach-Object { $_.Line }
$activeSwitchPreflightEvents = @($activeSwitchPreflightLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "activeSwitchPreflight "
} | Where-Object { $null -ne $_ })
$activeSwitchPreFadeLines = Select-String -Path $log.FullName -Pattern "activeSwitchPreFade " |
    ForEach-Object { $_.Line }
$activeSwitchPreFadeEvents = @($activeSwitchPreFadeLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "activeSwitchPreFade "
} | Where-Object { $null -ne $_ })
$activeSwitchBoundaryPopCandidateLines = Select-String -Path $log.FullName -Pattern "activeSwitchBoundaryPopCandidate " |
    ForEach-Object { $_.Line }
$activeSwitchBoundaryPopCandidateEvents = @($activeSwitchBoundaryPopCandidateLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "activeSwitchBoundaryPopCandidate "
} | Where-Object { $null -ne $_ })
$renderMirrorConclusionLines = Select-String -Path $log.FullName -Pattern "renderMirrorConclusion " |
    ForEach-Object { $_.Line }
$renderMirrorConclusions = @($renderMirrorConclusionLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "renderMirrorConclusion "
} | Where-Object { $null -ne $_ })
$renderMirrorStartLines = Select-String -Path $log.FullName -Pattern "renderMirror start " |
    ForEach-Object { $_.Line }
$renderMirrorStarts = @($renderMirrorStartLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "renderMirror start "
} | Where-Object { $null -ne $_ })
$seekResumeFirstSubmittedBlockLines = Select-String -Path $log.FullName -Pattern "seekResumeFirstSubmittedBlock " |
    ForEach-Object { $_.Line }
$seekResumeFirstSubmittedBlocks = @($seekResumeFirstSubmittedBlockLines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "seekResumeFirstSubmittedBlock "
} | Where-Object { $null -ne $_ })
$seekResumeRenderMirrorFirst50Lines = Select-String -Path $log.FullName -Pattern "renderMirrorFirst50msAfterSeek " |
    ForEach-Object { $_.Line }
$seekResumeRenderMirrorFirst50 = @($seekResumeRenderMirrorFirst50Lines | ForEach-Object {
    ConvertFrom-KeyValuePayload -Line $_ -Marker "renderMirrorFirst50msAfterSeek "
} | Where-Object { $null -ne $_ })
$seekResumeStartPositionsBySession = @{}
foreach ($start in $renderMirrorStarts) {
    $profile = Get-Field -Item $start -Name "startupProfile"
    if (-not (Test-SeekResumeProfile -Value $profile)) {
        continue
    }

    $session = Get-IntField -Item $start -Name "session" -Default 0
    $positionMs = Get-IntField -Item $start -Name "positionMs" -Default -1
    if ($session -gt 0 -and $positionMs -ge 0) {
        $seekResumeStartPositionsBySession[$session] = $positionMs
    }
}

$seekResumeArtifactsForReport = @($artifacts | Where-Object {
    (Test-SeekResumeProfile -Value (Get-StartupProfile -Item $_)) -or
        (Test-SeekResumeProfile -Value (Get-ArtifactPath -Item $_))
})
$seekResumeOffsetsForReport = @($seekResumeArtifactsForReport | ForEach-Object {
    Get-SeekResumeOffsetMsAfterResume -Artifact $_ -SessionStartPositions $seekResumeStartPositionsBySession
} | Where-Object { $null -ne $_ })
$seekResumeFirstOffsetForReport = if ($seekResumeOffsetsForReport.Count -gt 0) {
    ($seekResumeOffsetsForReport | Measure-Object -Minimum).Minimum
} else {
    $null
}
$seekResumeClassCountsForReport = Get-CountTable -Items $seekResumeArtifactsForReport -KeySelector {
    param($item)
    Get-ArtifactClass -Artifact $item -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs
}
$activeSwitchPositiveCandidateEventsForReport = @($activeSwitchBoundaryPopCandidateEvents | Where-Object {
    Get-Flag -Item $_ -Name "candidate"
})
$activeSwitchFallbackToSilenceCountForReport = @($activeSwitchBoundaryPopCandidateEvents | Where-Object {
    Get-Flag -Item $_ -Name "fallbackToSilence"
}).Count
$activeSwitchMetricEventsForReport = @(
    if ($activeSwitchPositiveCandidateEventsForReport.Count -gt 0) {
        $activeSwitchPositiveCandidateEventsForReport
    } else {
        $activeSwitchBoundaryPopCandidateEvents
    }
)
$activeSwitchTriggerReasonCountsForReport = Get-CountTable -Items $activeSwitchBoundaryPopCandidateEvents -KeySelector {
    param($item)
    "{0}/{1}" -f
        (Get-Field -Item $item -Name "activeSwitchTrigger"),
        (Get-Field -Item $item -Name "activeSwitchReason")
}
$detectorCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "detector" }
$severityCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "severity" }
$controlCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-ControlEventBucket -Item $item }
$profileCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-StartupProfile -Item $item }
$pathCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-ArtifactPath -Item $item }
$activeSwitchTriggerCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "activeSwitchTrigger" }
$stateCountsForReport = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "playbackState" }
$classCountsForReport = Get-CountTable -Items $artifacts -KeySelector {
    param($item)
    Get-ArtifactClass -Artifact $item -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs
}
$recentArtifactsForReport = @($artifacts | Select-Object -Last $Recent | ForEach-Object {
    Select-ArtifactSummary -Artifact $_ -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs
})
$analyzerReportPath = Resolve-AnalyzerReportPath -RepoRoot $repoRoot -ReportFile $ReportFile
$analyzerReport = [ordered]@{
    createdAt = (Get-Date).ToUniversalTime().ToString("o")
    logFile = $log.FullName
    seekResumeBoundaryWindowMs = $SeekResumeBoundaryWindowMs
    evidenceLayer = "log-driven-internal-and-submitted-pcm"
    endpointOutputVerified = $false
    limitation = "Analyzer output is based on logs, internal PCM, and submitted PCM evidence. It cannot prove speaker/headphone endpoint output is pop/click free."
    counts = [ordered]@{
        audioArtifactTotal = $artifacts.Count
        startupProfileEventCount = $startupEvents.Count
        renderMirrorConclusionCount = $renderMirrorConclusions.Count
        seekResumeArtifactCount = $seekResumeArtifactsForReport.Count
        activeSwitchBoundaryPopCandidateTotal = $activeSwitchBoundaryPopCandidateEvents.Count
        activeSwitchBoundaryPopCandidatePositiveCount = $activeSwitchPositiveCandidateEventsForReport.Count
        activeSwitchBoundaryFallbackToSilenceCount = $activeSwitchFallbackToSilenceCountForReport
        activeSwitchPreflightCount = $activeSwitchPreflightEvents.Count
        activeSwitchPreFadeCount = $activeSwitchPreFadeEvents.Count
    }
    activeSwitchBoundaryPopCandidateMetrics = [ordered]@{
        maxPreviousToBridgeStep = Get-MaxDoubleFieldText -Items $activeSwitchMetricEventsForReport -Name "previousToBridgeEnvelopeStep"
        maxBridgeToFirstStep = Get-MaxDoubleFieldText -Items $activeSwitchMetricEventsForReport -Name "bridgeToFirstEnvelopeStep"
        maxFirstBlockFadeEndpoint = Get-MaxDoubleFieldText -Items $activeSwitchMetricEventsForReport -Name "firstBlockEndFadeGain"
        triggerReasonDistribution = ConvertTo-CountRows -Counts $activeSwitchTriggerReasonCountsForReport
    }
    seekResume = [ordered]@{
        firstArtifactOffsetMsAfterResume = $seekResumeFirstOffsetForReport
        classDistribution = ConvertTo-CountRows -Counts $seekResumeClassCountsForReport
    }
    distributions = [ordered]@{
        detector = ConvertTo-CountRows -Counts $detectorCountsForReport
        severity = ConvertTo-CountRows -Counts $severityCountsForReport
        recentControlEvent = ConvertTo-CountRows -Counts $controlCountsForReport
        pipelineStartProfile = ConvertTo-CountRows -Counts $profileCountsForReport
        artifactPath = ConvertTo-CountRows -Counts $pathCountsForReport
        activeSwitchTrigger = ConvertTo-CountRows -Counts $activeSwitchTriggerCountsForReport
        playbackState = ConvertTo-CountRows -Counts $stateCountsForReport
        attributionClass = ConvertTo-CountRows -Counts $classCountsForReport
    }
    recent = [ordered]@{
        startupProfiles = @($startupEvents | Select-Object -Last 5)
        renderMirrorConclusions = @($renderMirrorConclusions | Select-Object -Last 5)
        seekResumeFirstSubmittedBlocks = @($seekResumeFirstSubmittedBlocks | Select-Object -Last 5)
        seekResumeRenderMirrorFirst50 = @($seekResumeRenderMirrorFirst50 | Select-Object -Last 5)
        activeSwitchBoundaryPolicies = @($activeSwitchPolicyEvents | Select-Object -Last 5)
        activeSwitchBoundaryPopCandidates = @($activeSwitchBoundaryPopCandidateEvents | Select-Object -Last 5)
        activeSwitchPreflights = @($activeSwitchPreflightEvents | Select-Object -Last 5)
        activeSwitchPreFades = @($activeSwitchPreFadeEvents | Select-Object -Last 5)
        audioArtifacts = $recentArtifactsForReport
    }
    priorityHint = Get-PriorityHintText -ClassCounts $classCountsForReport
}
Write-AnalyzerJsonReport -Path $analyzerReportPath -Report $analyzerReport

Write-Output "log:$($log.FullName)"
if (-not [string]::IsNullOrWhiteSpace($analyzerReportPath)) {
    Write-Output "analyzerReport:$analyzerReportPath"
}
Write-AttributionRules

Write-Output ""
Write-Output "Recent startup profiles:"
if ($startupEvents.Count -eq 0) {
    Write-Output "  none"
} else {
    $startupEvents | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  profile={0} startMuted={1} startupSilence={2} warmupDiscard={3} startupSilenceMs={4} device={5}" -f
            (Get-StartupProfile -Item $_),
            (Get-Field -Item $_ -Name "startMutedForFadeIn"),
            (Get-Field -Item $_ -Name "startupSilence"),
            (Get-Field -Item $_ -Name "warmupDiscard"),
            (Get-Field -Item $_ -Name "startupSilenceMs"),
            (Get-Field -Item $_ -Name "deviceId"))
    }
}

Write-Output ""
Write-Output "Recent render-mirror submitted PCM conclusions:"
if ($renderMirrorConclusions.Count -eq 0) {
    Write-Output "  none"
} else {
    $renderMirrorConclusions | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  session={0} artifactDetected={1} artifactCount={2} result={3} frames={4} raw={5} metadata={6}" -f
            (Get-Field -Item $_ -Name "session"),
            (Get-Field -Item $_ -Name "artifactDetected"),
            (Get-Field -Item $_ -Name "artifactCount"),
            (Get-Field -Item $_ -Name "result"),
            (Get-Field -Item $_ -Name "frames"),
            (Get-Field -Item $_ -Name "raw"),
            (Get-Field -Item $_ -Name "metadata"))
    }
}

Write-Output ""
Write-Output "Seek-resume artifact judgment:"
$seekResumeArtifacts = @($artifacts | Where-Object {
    (Test-SeekResumeProfile -Value (Get-StartupProfile -Item $_)) -or
        (Test-SeekResumeProfile -Value (Get-ArtifactPath -Item $_))
})
if ($seekResumeArtifacts.Count -eq 0) {
    Write-Output "  none"
} else {
    $offsets = @($seekResumeArtifacts | ForEach-Object {
        Get-SeekResumeOffsetMsAfterResume -Artifact $_ -SessionStartPositions $seekResumeStartPositionsBySession
    } | Where-Object { $null -ne $_ })
    $firstOffset = if ($offsets.Count -gt 0) { ($offsets | Measure-Object -Minimum).Minimum } else { "unknown" }
    $seekClassCounts = Get-CountTable -Items $seekResumeArtifacts -KeySelector {
        param($item)
        Get-ArtifactClass -Artifact $item -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs
    }
    Write-Output ("  boundaryWindowMs={0} firstArtifactOffsetMsAfterResume={1}" -f $SeekResumeBoundaryWindowMs, $firstOffset)
    $seekClassCounts.GetEnumerator() |
        Sort-Object @{ Expression = "Value"; Descending = $true }, @{ Expression = "Name"; Ascending = $true } |
        ForEach-Object {
            Write-Output ("  {0,5}  {1}" -f $_.Value, $_.Name)
        }
    Write-Output "  note=For compressed music, only boundary-window candidates plus hard stale/discontinuity evidence should drive a seek-resume bug conclusion."
}

Write-Output ""
Write-Output "Seek-resume first submitted block:"
if ($seekResumeFirstSubmittedBlocks.Count -eq 0) {
    Write-Output "  none"
} else {
    $seekResumeFirstSubmittedBlocks | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  session={0} peak={1} startSample={2} endSample={3} fadeApplied={4} minGain={5} maxGain={6} writeFrames={7}" -f
            (Get-Field -Item $_ -Name "session"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockPeak"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockStartSample"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockEndSample"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockFadeApplied"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockMinGain"),
            (Get-Field -Item $_ -Name "firstSubmittedBlockMaxGain"),
            (Get-Field -Item $_ -Name "writeFrames"))
    }
}

Write-Output ""
Write-Output "Seek-resume render mirror first 50 ms:"
if ($seekResumeRenderMirrorFirst50.Count -eq 0) {
    Write-Output "  none"
} else {
    $seekResumeRenderMirrorFirst50 | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  session={0} artifactDetected={1} peak={2} jump={3} startSample={4} endSample={5} startupSilenceFrames={6} warmupFrames={7} realPcmFrames={8}" -f
            (Get-Field -Item $_ -Name "session"),
            (Get-Field -Item $_ -Name "artifactDetected"),
            (Get-Field -Item $_ -Name "peak"),
            (Get-Field -Item $_ -Name "jump"),
            (Get-Field -Item $_ -Name "startSample"),
            (Get-Field -Item $_ -Name "endSample"),
            (Get-Field -Item $_ -Name "startupSilenceFrames"),
            (Get-Field -Item $_ -Name "warmupFrames"),
            (Get-Field -Item $_ -Name "realPcmFrames"))
    }
}

Write-Output ""
Write-Output "Recent active switch boundary policies:"
if ($activeSwitchPolicyEvents.Count -eq 0) {
    Write-Output "  none"
} else {
    $activeSwitchPolicyEvents | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  name={0} currentRate={1} targetRate={2} startupSilenceMs={3} pcmFadeInMs={4} firstBlockMaxFadeGain={5}" -f
            (Get-Field -Item $_ -Name "name"),
            (Get-Field -Item $_ -Name "currentRate"),
            (Get-Field -Item $_ -Name "targetRate"),
            (Get-Field -Item $_ -Name "startupSilenceMs"),
            (Get-Field -Item $_ -Name "pcmFadeInMs"),
            (Get-Field -Item $_ -Name "firstBlockMaxFadeGain"))
    }
}

Write-Output ""
Write-Output "Active switch boundary pop candidates:"
if ($activeSwitchBoundaryPopCandidateEvents.Count -eq 0) {
    Write-Output "  none"
} else {
    $positiveCandidateEvents = @($activeSwitchBoundaryPopCandidateEvents | Where-Object {
        Get-Flag -Item $_ -Name "candidate"
    })
    $fallbackToSilenceCount = @($activeSwitchBoundaryPopCandidateEvents | Where-Object {
        Get-Flag -Item $_ -Name "fallbackToSilence"
    }).Count
    $metricEvents = @(
        if ($positiveCandidateEvents.Count -gt 0) {
            $positiveCandidateEvents
        } else {
            $activeSwitchBoundaryPopCandidateEvents
        }
    )
    Write-Output ("  total={0} candidateCount={1} fallbackToSilenceCount={2} maxPreviousToBridgeStep={3} maxBridgeToFirstStep={4} maxFirstBlockFadeEndpoint={5}" -f
        $activeSwitchBoundaryPopCandidateEvents.Count,
        $positiveCandidateEvents.Count,
        $fallbackToSilenceCount,
        (Get-MaxDoubleFieldText -Items $metricEvents -Name "previousToBridgeEnvelopeStep"),
        (Get-MaxDoubleFieldText -Items $metricEvents -Name "bridgeToFirstEnvelopeStep"),
        (Get-MaxDoubleFieldText -Items $metricEvents -Name "firstBlockEndFadeGain"))

    Write-Output "  trigger/reason distribution:"
    $triggerReasonCounts = Get-CountTable -Items $activeSwitchBoundaryPopCandidateEvents -KeySelector {
        param($item)
        "{0}/{1}" -f
            (Get-Field -Item $item -Name "activeSwitchTrigger"),
            (Get-Field -Item $item -Name "activeSwitchReason")
    }
    $triggerReasonCounts.GetEnumerator() |
        Sort-Object @{ Expression = "Value"; Descending = $true }, @{ Expression = "Name"; Ascending = $true } |
        ForEach-Object {
            Write-Output ("  {0,5}  {1}" -f $_.Value, $_.Name)
        }

    Write-Output "  recent:"
    $activeSwitchBoundaryPopCandidateEvents | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  candidate={0} trigger={1} reason={2} previousToBridgeStep={3} bridgeToFirstStep={4} fallbackToSilence={5} firstBlockFadeEndpoint={6}" -f
            (Get-Field -Item $_ -Name "candidate"),
            (Get-Field -Item $_ -Name "activeSwitchTrigger"),
            (Get-Field -Item $_ -Name "activeSwitchReason"),
            (Get-Field -Item $_ -Name "previousToBridgeEnvelopeStep"),
            (Get-Field -Item $_ -Name "bridgeToFirstEnvelopeStep"),
            (Get-Field -Item $_ -Name "fallbackToSilence"),
            (Get-Field -Item $_ -Name "firstBlockEndFadeGain"))
    }
}

Write-Output ""
Write-Output "Recent active switch preflight decisions:"
if ($activeSwitchPreflightEvents.Count -eq 0) {
    Write-Output "  none"
} else {
    $activeSwitchPreflightEvents | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  decision={0} trigger={1} reason={2} entryPhase={3} phase={4} hot={5} formatChanged={6} outputSuspended={7} freshBuffer={8} currentRate={9} targetRate={10} bufferedBytes={11} elapsedMs={12}" -f
            (Get-Field -Item $_ -Name "decision"),
            (Get-Field -Item $_ -Name "trigger"),
            (Get-Field -Item $_ -Name "reason"),
            (Get-Field -Item $_ -Name "entryPhase"),
            (Get-Field -Item $_ -Name "phase"),
            (Get-Field -Item $_ -Name "hotReconfigureEligible"),
            (Get-Field -Item $_ -Name "formatChanged"),
            (Get-Field -Item $_ -Name "outputSuspended"),
            (Get-Field -Item $_ -Name "freshBuffer"),
            (Get-Field -Item $_ -Name "currentRate"),
            (Get-Field -Item $_ -Name "targetRate"),
            (Get-Field -Item $_ -Name "bufferedBytes"),
            (Get-Field -Item $_ -Name "elapsedMs"))
    }
}

Write-Output ""
Write-Output "Recent active switch pre-fades:"
if ($activeSwitchPreFadeEvents.Count -eq 0) {
    Write-Output "  none"
} else {
    $activeSwitchPreFadeEvents | Select-Object -Last 5 | ForEach-Object {
        Write-Output ("  timing={0} trigger={1} phase={2} reason={3} session={4} positionMs={5} elapsedMs={6}" -f
            (Get-Field -Item $_ -Name "timing"),
            (Get-Field -Item $_ -Name "trigger"),
            (Get-Field -Item $_ -Name "phase"),
            (Get-Field -Item $_ -Name "reason"),
            (Get-Field -Item $_ -Name "session"),
            (Get-Field -Item $_ -Name "positionMs"),
            (Get-Field -Item $_ -Name "elapsedMs"))
    }
}

Write-Output ""
Write-Output ("audioArtifact total:{0}" -f $artifacts.Count)
if ($artifacts.Count -eq 0) {
    Write-Output "No internal PCM audioArtifact entries found."
    Write-Output "Note: this does not prove acoustic output continuity; endpoint reset, spatial-audio, or format-switch transients require manual listening or loopback capture."
    return
}

$recentArtifacts = @($artifacts | Select-Object -Last $Recent)
Write-Output ""
Write-Output ("Recent audioArtifact entries (last {0}):" -f $recentArtifacts.Count)
if ($RawRecent) {
    $recentArtifacts | ForEach-Object { Write-Output $_["rawLine"] }
} else {
    foreach ($artifact in $recentArtifacts) {
        $class = Get-ArtifactClass -Artifact $artifact -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs
        $seekOffset = Get-SeekResumeOffsetMsAfterResume -Artifact $artifact -SessionStartPositions $seekResumeStartPositionsBySession
        $seekOffsetText = if ($null -eq $seekOffset) { "unknown" } else { [string]$seekOffset }
        Write-Output ("  {0} detector={1} severity={2} class={3} profile={4} path={5} event={6} state={7} posMs={8} seekOffsetMs={9} jump={10} peak={11} write={12} padding={13} available={14}" -f
            (Get-Field -Item $artifact -Name "timestamp"),
            (Get-Field -Item $artifact -Name "detector"),
            (Get-Field -Item $artifact -Name "severity"),
            $class,
            (Get-StartupProfile -Item $artifact),
            (Get-ArtifactPath -Item $artifact),
            (Get-ControlEventBucket -Item $artifact),
            (Get-Field -Item $artifact -Name "playbackState"),
            (Get-Field -Item $artifact -Name "positionMs"),
            $seekOffsetText,
            (Get-Field -Item $artifact -Name "jump"),
            (Get-Field -Item $artifact -Name "peak"),
            (Get-Field -Item $artifact -Name "writeFrames"),
            (Get-Field -Item $artifact -Name "wasapiPaddingFrames"),
            (Get-Field -Item $artifact -Name "wasapiAvailableFrames"))
    }
}

$detectorCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "detector" }
$severityCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "severity" }
$controlCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-ControlEventBucket -Item $item }
$profileCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-StartupProfile -Item $item }
$pathCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-ArtifactPath -Item $item }
$activeSwitchTriggerCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "activeSwitchTrigger" }
$stateCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-Field -Item $item -Name "playbackState" }
$classCounts = Get-CountTable -Items $artifacts -KeySelector { param($item) Get-ArtifactClass -Artifact $item -SessionStartPositions $seekResumeStartPositionsBySession -BoundaryWindowMs $SeekResumeBoundaryWindowMs }

Write-CountTable -Title "Detector distribution:" -Counts $detectorCounts
Write-CountTable -Title "Severity distribution:" -Counts $severityCounts
Write-CountTable -Title "recentControlEvent distribution:" -Counts $controlCounts
Write-CountTable -Title "pipelineStartProfile distribution:" -Counts $profileCounts
Write-CountTable -Title "artifactPath distribution:" -Counts $pathCounts
Write-CountTable -Title "activeSwitchTrigger distribution:" -Counts $activeSwitchTriggerCounts
Write-CountTable -Title "playbackState distribution:" -Counts $stateCounts
Write-CountTable -Title "Attribution class distribution:" -Counts $classCounts

Write-PriorityHint -ClassCounts $classCounts
