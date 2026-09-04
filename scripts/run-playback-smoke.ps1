param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [int]$QuitAfterMs = 8000,
    [int]$SeekAfterMs = -1,
    [long]$SeekToMs = -1,
    [int]$SecondSeekAfterMs = -1,
    [long]$SecondSeekToMs = -1,
    [switch]$SeekPauseResume,
    [string]$SeekSequence = "",
    [int]$PauseAfterMs = -1,
    [int]$ResumeAfterMs = -1,
    [int]$StopAfterMs = -1,
    [int]$RefreshOutputAfterMs = -1,
    [int]$RefreshOutputCount = 1,
    [int]$RefreshOutputIntervalMs = 0,
    [switch]$ListOutputDevices,
    [int]$OutputDeviceIndex = [int]::MinValue,
    [int]$AsioOutputIndex = [int]::MinValue,
    [switch]$ExclusiveMode,
    [int]$SwitchOutputAfterMs = -1,
    [int]$SwitchOutputToIndex = [int]::MinValue,
    [int]$RepeatOutputSwitch = 1,
    [int]$SwitchIntervalMs = 0,
    [Alias("HoldSeconds")]
    [ValidateRange(0, 86400)]
    [int]$ManualObservationWindowSeconds = 0,
    [string]$ManualObservationNote = "",
    [AllowEmptyString()]
    [ValidateSet("", "PopHeard", "NoPop", "Unclear")]
    [string]$ManualEndpointResult = "",
    [AllowEmptyString()]
    [ValidateSet("", "SwitchInstant", "Recovery", "PlaybackStart", "Unknown")]
    [string]$ManualEndpointTiming = "",
    [string]$ReportFile = "",
    [string]$HarnessReportFile = "",
    [int]$SwitchSourceAfterMs = -1,
    [string]$SwitchSource = "",
    [int]$GracePeriodMs = 5000,
    [switch]$RequirePlaying,
    [switch]$RequirePauseResume,
    [switch]$RequireStop,
    [switch]$RequireStopFadeOut,
    [switch]$RequireFinished,
    [ValidateRange(0, 86400000)]
    [int]$RequireFinishedBufferMs = 5000,
    [switch]$RejectPlaybackErrors,
    [switch]$RequireSeekCompletion,
    [int]$RequireSeekCompletionCount = 0,
    [int]$RequireSeekResumeProfileCount = 0,
    [int]$RequireLoadCount = 0,
    [long]$ExpectedSeekTargetMs = -1,
    [int]$SeekToleranceMs = 1500,
    [long]$MinReachedPositionMs = -1,
    [switch]$RequireAudibleLevels,
    [double]$MinAudiblePeak = 0.01,
    [switch]$RequireExactBitDepthMatch,
    [switch]$RequireNoiseShaping,
    [string]$RequireLogPattern = "",
    [string]$RequireErrorPattern = "",
    [int]$RequireConservativeRebuildCount = 0,
    [int]$RequireFirstDataBlockAfterConfigureCount = 0,
    [int]$RequireFirstBlockGuardCount = 0,
    [int]$RequireActiveSwitchBoundaryEnvelopeCount = 0,
    [int]$RequireActiveSwitchPreflightCount = 0,
    [int]$RequireActiveSwitchPreFadeCount = 0,
    [switch]$RequireSourceSwitchClean,
    [string]$RefreshOutputTrigger = "",
    [string]$HotReconfigureCompatibilityOverride = "",
    [string]$LogPath = "",
    [string]$FfmpegPathOverride = "",
    [string]$FfprobePathOverride = "",
    [ValidateRange(1, 100000)]
    [int]$KeepRuns = 20,
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "harness-common.ps1")

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
. (Join-Path $PSScriptRoot "cleanup-test-artifacts.ps1")
. (Join-Path $PSScriptRoot "playback-smoke-evidence.ps1")
. (Join-Path $PSScriptRoot "playback-smoke-runner.ps1")
. (Join-Path $PSScriptRoot "playback-smoke-assertions.ps1")

# ── Path setup, validation, process launch ──
$runner = Invoke-SmokeRunner -BoundParameters $PSBoundParameters `
    -Source $Source `
    -BuildDir $BuildDir `
    -Configuration $Configuration `
    -QuitAfterMs $QuitAfterMs `
    -SeekAfterMs $SeekAfterMs `
    -SeekToMs $SeekToMs `
    -SecondSeekAfterMs $SecondSeekAfterMs `
    -SecondSeekToMs $SecondSeekToMs `
    -SeekSequence $SeekSequence `
    -PauseAfterMs $PauseAfterMs `
    -ResumeAfterMs $ResumeAfterMs `
    -StopAfterMs $StopAfterMs `
    -RefreshOutputAfterMs $RefreshOutputAfterMs `
    -RefreshOutputCount $RefreshOutputCount `
    -RefreshOutputIntervalMs $RefreshOutputIntervalMs `
    -ListOutputDevices:$ListOutputDevices `
    -OutputDeviceIndex $OutputDeviceIndex `
    -AsioOutputIndex $AsioOutputIndex `
    -ExclusiveMode:$ExclusiveMode `
    -SwitchOutputAfterMs $SwitchOutputAfterMs `
    -SwitchOutputToIndex $SwitchOutputToIndex `
    -RepeatOutputSwitch $RepeatOutputSwitch `
    -SwitchIntervalMs $SwitchIntervalMs `
    -ManualObservationWindowSeconds $ManualObservationWindowSeconds `
    -ManualObservationNote $ManualObservationNote `
    -ManualEndpointResult $ManualEndpointResult `
    -ManualEndpointTiming $ManualEndpointTiming `
    -ReportFile $ReportFile `
    -HarnessReportFile $HarnessReportFile `
    -SwitchSourceAfterMs $SwitchSourceAfterMs `
    -SwitchSource $SwitchSource `
    -GracePeriodMs $GracePeriodMs `
    -SeekPauseResume:$SeekPauseResume `
    -RequirePlaying:$RequirePlaying `
    -RequirePauseResume:$RequirePauseResume `
    -RequireStop:$RequireStop `
    -RequireStopFadeOut:$RequireStopFadeOut `
    -RejectPlaybackErrors:$RejectPlaybackErrors `
    -RequireErrorPattern $RequireErrorPattern `
    -RequireAudibleLevels:$RequireAudibleLevels `
    -RequireSourceSwitchClean:$RequireSourceSwitchClean `
    -RequireFinished:$RequireFinished `
    -RequireFinishedBufferMs $RequireFinishedBufferMs `
    -RefreshOutputTrigger $RefreshOutputTrigger `
    -HotReconfigureCompatibilityOverride $HotReconfigureCompatibilityOverride `
    -LogPath $LogPath `
    -FfmpegPathOverride $FfmpegPathOverride `
    -FfprobePathOverride $FfprobePathOverride

$runLogPath = $runner.runLogPath
$runReportPath = $runner.runReportPath
$runHarnessReportPath = $runner.runHarnessReportPath
$failureReasons = $runner.failureReasons
$inconclusiveReasons = $runner.inconclusiveReasons
$warnings = $runner.warnings
$manualObservation = $runner.manualObservation
$requestedActions = $runner.requestedActions
$processCleanup = $runner.processCleanup
$process = $runner.process

# ── Evidence collection ──
$latestLog = Get-Item -Path $runLogPath
Write-Output "log:$($latestLog.FullName)"
Write-Output "report:$runReportPath"
$diagnosticReport = Get-Content -Path $runReportPath -Encoding UTF8 -Raw | ConvertFrom-Json

$logLines = Get-Content -Path $runLogPath -Encoding UTF8
$smokeEvidence = Read-SmokeLogEvidence `
    -LogLines $logLines `
    -DiagnosticReport $diagnosticReport `
    -AsioOutputIndex $AsioOutputIndex `
    -RequireErrorPattern $RequireErrorPattern `
    -RejectPlaybackErrors $RejectPlaybackErrors `
    -FailureReasons $failureReasons `
    -InconclusiveReasons $inconclusiveReasons `
    -Warnings $warnings

# ── Assertions / result normalization ──
$assertionResult = Invoke-SmokeAssertions `
    -LogLines $logLines `
    -DiagnosticReport $diagnosticReport `
    -SmokeEvidence $smokeEvidence `
    -ExitCode $process.ExitCode `
    -AsioOutputIndex $AsioOutputIndex `
    -RequirePlaying:$RequirePlaying `
    -RequirePauseResume:$RequirePauseResume `
    -RequireStop:$RequireStop `
    -RequireStopFadeOut:$RequireStopFadeOut `
    -RequireFinished:$RequireFinished `
    -RejectPlaybackErrors:$RejectPlaybackErrors `
    -RequireErrorPattern $RequireErrorPattern `
    -RequireLogPattern $RequireLogPattern `
    -RequireConservativeRebuildCount $RequireConservativeRebuildCount `
    -RequireFirstDataBlockAfterConfigureCount $RequireFirstDataBlockAfterConfigureCount `
    -RequireFirstBlockGuardCount $RequireFirstBlockGuardCount `
    -RequireActiveSwitchBoundaryEnvelopeCount $RequireActiveSwitchBoundaryEnvelopeCount `
    -RequireActiveSwitchPreflightCount $RequireActiveSwitchPreflightCount `
    -RequireActiveSwitchPreFadeCount $RequireActiveSwitchPreFadeCount `
    -RequireSourceSwitchClean:$RequireSourceSwitchClean `
    -RequireSeekCompletion:$RequireSeekCompletion `
    -RequireSeekCompletionCount $RequireSeekCompletionCount `
    -RequireSeekResumeProfileCount $RequireSeekResumeProfileCount `
    -RequireLoadCount $RequireLoadCount `
    -ExpectedSeekTargetMs $ExpectedSeekTargetMs `
    -SeekToleranceMs $SeekToleranceMs `
    -MinReachedPositionMs $MinReachedPositionMs `
    -RequireAudibleLevels:$RequireAudibleLevels `
    -MinAudiblePeak $MinAudiblePeak `
    -RequireExactBitDepthMatch:$RequireExactBitDepthMatch `
    -RequireNoiseShaping:$RequireNoiseShaping `
    -FailureReasons $failureReasons `
    -InconclusiveReasons $inconclusiveReasons `
    -Warnings $warnings

$assertionsPassed = $assertionResult.assertionsPassed
$assertionFailure = $assertionResult.assertionFailure
$harnessResult = $assertionResult.harnessResult

# ── Harness report ──
Write-SmokeHarnessReport `
    -LatestLog $latestLog `
    -RunReportPath $runReportPath `
    -RunHarnessReportPath $runHarnessReportPath `
    -DiagnosticReport $diagnosticReport `
    -SmokeEvidence $smokeEvidence `
    -AsioOutputIndex $AsioOutputIndex `
    -ProcessCleanup $processCleanup `
    -ManualObservation $manualObservation `
    -RequestedActions $requestedActions `
    -AssertionsPassed $assertionsPassed `
    -AssertionFailure $assertionFailure `
    -HarnessResult $harnessResult `
    -ProcessExitCode $process.ExitCode `
    -FailureReasons $failureReasons `
    -InconclusiveReasons $inconclusiveReasons `
    -Warnings $warnings

# ── Diagnostic output ──
Write-SmokeDiagnosticOutput `
    -DiagnosticReport $diagnosticReport `
    -SmokeEvidence $smokeEvidence `
    -AsioOutputIndex $AsioOutputIndex `
    -ProcessCleanup $processCleanup `
    -ManualObservation $manualObservation `
    -RunHarnessReportPath $runHarnessReportPath `
    -HarnessResult $harnessResult `
    -AssertionsPassed $assertionsPassed `
    -AssertionFailure $assertionFailure `
    -ProcessExitCode $process.ExitCode
# ── Cleanup / archival ──
$cleanupSummary = Invoke-TestArtifactRetention `
    -RepoRoot $repoRoot `
    -BuildDir $BuildDir `
    -KeepRuns $KeepRuns `
    -PreserveRunTokens @(
        if ([System.IO.Path]::GetFileName($runLogPath) -match '^player-smoke-(?<run>.+?)\.log$') {
            $Matches.run
        }
    ) `
    -NoCleanup:$NoCleanup
Write-Output "cleanup:removedRuns=$($cleanupSummary.removedRuns) removedFiles=$($cleanupSummary.removedFiles) keptRuns=$($cleanupSummary.keptRuns)"
Write-Output "archive:archivedRuns=$($cleanupSummary.archivedRuns) archivedFiles=$($cleanupSummary.archivedFiles) manifest=$($cleanupSummary.archiveManifest)"
foreach ($archiveError in @($cleanupSummary.archiveErrors)) {
    Write-Warning "archiveError:$archiveError"
}

if ($harnessResult -eq "FAIL") {
    throw "Smoke harness result FAIL: $([string]::Join('; ', $failureReasons.ToArray()))"
}
