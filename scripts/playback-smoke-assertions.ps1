# Assertion/validation and result normalization for smoke tests.
# Dot-source this file from scripts that need post-run assertions after
# smoke evidence has been collected.
#
# Depends on harness-common.ps1 for: Assert-LogRegexContains,
# Normalize-SmokeResult, Get-LogKeyValue, Get-MaxLogDoubleValue,
# Write-HarnessReport.

function Invoke-SmokeAssertions {
    param(
        [string[]]$LogLines,
        [object]$DiagnosticReport,
        [object]$SmokeEvidence,
        [int]$ExitCode,
        [int]$AsioOutputIndex = [int]::MinValue,
        [switch]$RequirePlaying,
        [switch]$RequirePauseResume,
        [switch]$RequireStop,
        [switch]$RequireStopFadeOut,
        [switch]$RequireFinished,
        [switch]$RejectPlaybackErrors,
        [string]$RequireErrorPattern = "",
        [string]$RequireLogPattern = "",
        [int]$RequireConservativeRebuildCount = 0,
        [int]$RequireFirstDataBlockAfterConfigureCount = 0,
        [int]$RequireFirstBlockGuardCount = 0,
        [int]$RequireActiveSwitchBoundaryEnvelopeCount = 0,
        [int]$RequireActiveSwitchPreflightCount = 0,
        [int]$RequireActiveSwitchPreFadeCount = 0,
        [switch]$RequireSourceSwitchClean,
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
        [System.Collections.Generic.List[string]]$FailureReasons,
        [System.Collections.Generic.List[string]]$InconclusiveReasons,
        [System.Collections.Generic.List[string]]$Warnings
    )

    $appReportResult = $SmokeEvidence.appReportResult

    if ($ExitCode -ne 0) {
        Add-UniqueString -List $FailureReasons -Value "process-exit-code:$ExitCode"
    }
    switch ($appReportResult) {
        "PASS" {}
        "FAIL" {
            if ($SmokeEvidence.seekResumeDetectorOnlyCaution) {
                Add-UniqueString -List $Warnings -Value "app-report-fail-normalized:seek-resume-compressed-content-detector-only"
                Add-UniqueString -List $InconclusiveReasons -Value "seek-resume-compressed-content-detector-only"
            } elseif ($SmokeEvidence.expectedAppReportFailure) {
                Add-UniqueString -List $Warnings -Value "app-report-fail-normalized:expected-error"
            } else {
                Add-UniqueString -List $FailureReasons -Value "app-report-fail"
            }
        }
        "WARN" {
            Add-UniqueString -List $Warnings -Value "app-report-warn"
            Add-UniqueString -List $InconclusiveReasons -Value "app-report-warn-normalized"
        }
        "INCONCLUSIVE" {
            Add-UniqueString -List $InconclusiveReasons -Value "app-report-inconclusive"
        }
        default {
            Add-UniqueString -List $InconclusiveReasons -Value "app-report-result-missing-or-unknown"
        }
    }

    $assertionsPassed = $false
    $assertionFailure = ""
    try {
        if ($RequirePlaying) {
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*state=Playing' -Description 'playback start'
            if ($AsioOutputIndex -ne [int]::MinValue -and -not $SmokeEvidence.asioFirstBufferSwitchObserved) {
                throw "ASIO playback start did not reach first buffer switch"
            }
        }

        if ($RequirePauseResume) {
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*action=pause' -Description 'pause action'
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*state=Paused' -Description 'pause state'
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*action=resume' -Description 'resume action'
        }

        if ($RequireStop) {
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*action=stop' -Description 'stop action'
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*state=Stopped' -Description 'stop completion'
        }

        if ($RequireStopFadeOut -and -not $SmokeEvidence.stopFadeOutCompleted) {
            throw "Playback log missing completed stop fade-out to zero gain"
        }

        if ($RequireFinished) {
            Assert-LogRegexContains -Lines $LogLines -Pattern '\[automation\].*finished$' -Description 'finished signal'
        }

        if ($RejectPlaybackErrors) {
            if ($LogLines | Select-String -Pattern '\[automation\].*error message=') {
                throw "Playback log contains automation error"
            }
        }

        if (-not [string]::IsNullOrWhiteSpace($RequireErrorPattern)) {
            Assert-LogRegexContains -Lines $LogLines -Pattern "\[automation\].*error message=.*$RequireErrorPattern" -Description "required error pattern"
        }

        if ($AsioOutputIndex -ne [int]::MinValue) {
            if (-not $SmokeEvidence.asioSelectionConfirmed) {
                throw "ASIO output index $AsioOutputIndex was requested, but the app did not confirm ASIO output selection"
            }
            if ($null -eq $SmokeEvidence.asioSelectedIndex -or $SmokeEvidence.asioSelectedIndex -ne $AsioOutputIndex) {
                throw "ASIO output selection index $($SmokeEvidence.asioSelectedIndex) did not match requested index $AsioOutputIndex"
            }
            if ([string]::IsNullOrWhiteSpace($SmokeEvidence.loadedBackend) -or $SmokeEvidence.loadedBackend -notlike "*ASIO*") {
                throw "ASIO output selection was requested, but loaded backend was '$($SmokeEvidence.loadedBackend)'"
            }
        }

        if (-not [string]::IsNullOrWhiteSpace($RequireLogPattern)) {
            Assert-LogRegexContains -Lines $LogLines -Pattern $RequireLogPattern -Description "required log pattern"
        }

        if ($RequireConservativeRebuildCount -gt 0) {
            if ($SmokeEvidence.waitingForInvalidationMatches.Count -ne $RequireConservativeRebuildCount) {
                throw "WaitingForInvalidation count $($SmokeEvidence.waitingForInvalidationMatches.Count) did not match expected count $RequireConservativeRebuildCount"
            }
            if ($SmokeEvidence.absorbedOutputErrorMatches.Count -ne $RequireConservativeRebuildCount) {
                throw "absorbed-output-error count $($SmokeEvidence.absorbedOutputErrorMatches.Count) did not match expected count $RequireConservativeRebuildCount"
            }
            if ($SmokeEvidence.conservativeRebuildMatches.Count -ne $RequireConservativeRebuildCount) {
                throw "conservative-rebuild count $($SmokeEvidence.conservativeRebuildMatches.Count) did not match expected count $RequireConservativeRebuildCount"
            }
            if ($SmokeEvidence.activeSwitchRebuildPipelineMatches.Count -ne $RequireConservativeRebuildCount) {
                throw "ActiveSwitchRebuild pipeline count $($SmokeEvidence.activeSwitchRebuildPipelineMatches.Count) did not match expected count $RequireConservativeRebuildCount"
            }
        }

        if ($RequireFirstDataBlockAfterConfigureCount -gt 0 -and $SmokeEvidence.firstDataBlockAfterConfigureMatches.Count -ne $RequireFirstDataBlockAfterConfigureCount) {
            throw "firstDataBlockAfterConfigure count $($SmokeEvidence.firstDataBlockAfterConfigureMatches.Count) did not match expected count $RequireFirstDataBlockAfterConfigureCount"
        }

        if ($RequireFirstBlockGuardCount -gt 0 -and $SmokeEvidence.firstBlockGuardMatches.Count -ne $RequireFirstBlockGuardCount) {
            throw "activeSwitchFirstBlockEntryGuard count $($SmokeEvidence.firstBlockGuardMatches.Count) did not match expected count $RequireFirstBlockGuardCount"
        }

        if ($RequireActiveSwitchBoundaryEnvelopeCount -gt 0 -and $SmokeEvidence.activeSwitchBoundaryEnvelopeMatches.Count -ne $RequireActiveSwitchBoundaryEnvelopeCount) {
            throw "activeSwitchBoundaryEnvelope count $($SmokeEvidence.activeSwitchBoundaryEnvelopeMatches.Count) did not match expected count $RequireActiveSwitchBoundaryEnvelopeCount"
        }

        if ($RequireActiveSwitchPreflightCount -gt 0 -and $SmokeEvidence.activeSwitchPreflightMatches.Count -ne $RequireActiveSwitchPreflightCount) {
            throw "activeSwitchPreflight count $($SmokeEvidence.activeSwitchPreflightMatches.Count) did not match expected count $RequireActiveSwitchPreflightCount"
        }

        if ($RequireActiveSwitchPreFadeCount -gt 0 -and $SmokeEvidence.activeSwitchPreFadeMatches.Count -ne $RequireActiveSwitchPreFadeCount) {
            throw "activeSwitchPreFade count $($SmokeEvidence.activeSwitchPreFadeMatches.Count) did not match expected count $RequireActiveSwitchPreFadeCount"
        }

        if ($RequireSourceSwitchClean) {
            if (-not $DiagnosticReport.sourceSwitchRequested) {
                throw "Diagnostic report did not detect a source switch"
            }
            if (-not $DiagnosticReport.sourceSwitchClean) {
                throw "Source switch contamination check failed oldPcmLeakDetected=$($DiagnosticReport.oldPcmLeakDetected) staleSessionWriteDetected=$($DiagnosticReport.staleSessionWriteDetected) staleBufferReadDetected=$($DiagnosticReport.staleBufferReadDetected)"
            }
        }

        if ($RequireSeekCompletion) {
            if ($SmokeEvidence.seekCompletionMatches.Count -le 0) {
                throw "Playback log missing seekCompleted event"
            }
        }

        if ($RequireSeekCompletionCount -gt 0 -and $SmokeEvidence.seekCompletionMatches.Count -ne $RequireSeekCompletionCount) {
            throw "Playback log seekCompleted count $($SmokeEvidence.seekCompletionMatches.Count) did not match expected count $RequireSeekCompletionCount"
        }

        if ($RequireSeekResumeProfileCount -gt 0 -and $SmokeEvidence.seekResumePipelineMatches.Count -ne $RequireSeekResumeProfileCount) {
            throw "SeekResume pipeline count $($SmokeEvidence.seekResumePipelineMatches.Count) did not match expected count $RequireSeekResumeProfileCount"
        }

        if ($RequireSeekCompletion -or $RequireSeekCompletionCount -gt 0) {
            $lastSeekCompletion = $SmokeEvidence.seekCompletionMatches[-1]
            if ($ExpectedSeekTargetMs -ge 0 -and $lastSeekCompletion.Target -ne $ExpectedSeekTargetMs) {
                throw "Playback seekCompleted target $($lastSeekCompletion.Target) ms did not match expected target $ExpectedSeekTargetMs ms"
            }
            if ($ExpectedSeekTargetMs -ge 0 -and [Math]::Abs($lastSeekCompletion.Actual - $ExpectedSeekTargetMs) -gt $SeekToleranceMs) {
                throw "Playback seekCompleted actual $($lastSeekCompletion.Actual) ms exceeded tolerance $SeekToleranceMs ms for target $ExpectedSeekTargetMs ms"
            }

            Write-Output "seekCompletedTarget:$($lastSeekCompletion.Target)"
            Write-Output "seekCompletedActual:$($lastSeekCompletion.Actual)"
        }

        if ($RequireLoadCount -gt 0 -and $SmokeEvidence.loadMatches.Count -ne $RequireLoadCount) {
            throw "Playback log load count $($SmokeEvidence.loadMatches.Count) did not match expected count $RequireLoadCount"
        }

        if ($MinReachedPositionMs -ge 0 -and $SmokeEvidence.maxPositionMs -lt $MinReachedPositionMs) {
            throw "Playback log max position $($SmokeEvidence.maxPositionMs) ms did not reach required minimum $MinReachedPositionMs ms"
        }

        if ($RequireAudibleLevels -and $SmokeEvidence.maxAudioPeak -lt $MinAudiblePeak) {
            throw "Playback log max audio peak $($SmokeEvidence.maxAudioPeak) did not reach required minimum $MinAudiblePeak"
        }

        if ($RequireExactBitDepthMatch) {
            $actualMatch = $SmokeEvidence.bitDepthMatch
            if ([string]::IsNullOrWhiteSpace($actualMatch)) {
                throw "bitDepthMatch not found in log (expected 'exact')"
            }
            if ($actualMatch -ne "exact") {
                throw "bitDepthMatch=expected(exact) actual=$actualMatch"
            }
        }

        if ($RequireNoiseShaping) {
            if (-not $SmokeEvidence.noiseShapingEnabled) {
                throw "noise shaping not activated (expected 'noiseShaping enabled' in log)"
            }
        }

        $assertionsPassed = $true
    } catch {
        $assertionFailure = $_.Exception.Message
        Add-UniqueString -List $FailureReasons -Value "assertion-failed:$assertionFailure"
    }

    $harnessResult = Normalize-SmokeResult -AppReportResult $appReportResult `
        -FailureReasons $FailureReasons `
        -InconclusiveReasons $InconclusiveReasons `
        -ExpectedAppReportFailure $SmokeEvidence.expectedAppReportFailure

    return [ordered]@{
        assertionsPassed = $assertionsPassed
        assertionFailure = $assertionFailure
        harnessResult = $harnessResult
    }
}

function Write-SmokeHarnessReport {
    param(
        [System.IO.FileInfo]$LatestLog,
        [string]$RunReportPath,
        [string]$RunHarnessReportPath,
        [object]$DiagnosticReport,
        [object]$SmokeEvidence,
        [int]$AsioOutputIndex = [int]::MinValue,
        [object]$ProcessCleanup,
        [object]$ManualObservation,
        [object]$RequestedActions,
        [bool]$AssertionsPassed,
        [string]$AssertionFailure = "",
        [string]$HarnessResult,
        [int]$ProcessExitCode,
        [System.Collections.Generic.List[string]]$FailureReasons,
        [System.Collections.Generic.List[string]]$InconclusiveReasons,
        [System.Collections.Generic.List[string]]$Warnings
    )

    $files = [ordered]@{
        textLogFile = $LatestLog.FullName
        jsonlDiagnosticFile = $DiagnosticReport.jsonlDiagnosticFile
        appReportFile = $RunReportPath
        harnessReportFile = $RunHarnessReportPath
    }
    $observedActions = [ordered]@{
        playbackStarted = $DiagnosticReport.playbackStarted
        loadedBackend = $SmokeEvidence.loadedBackend
        asioOutputSelectionRequested = ($AsioOutputIndex -ne [int]::MinValue)
        asioOutputSelectionConfirmed = $SmokeEvidence.asioSelectionConfirmed
        asioSelectedIndex = $SmokeEvidence.asioSelectedIndex
        asioSelectedId = $SmokeEvidence.asioSelectedId
        asioSelectedDescription = $SmokeEvidence.asioSelectedDescription
        asioOutputDeviceCount = $SmokeEvidence.asioOutputDeviceCount
        asioConfigureOutputObserved = $SmokeEvidence.asioConfigureOutputObserved
        asioConfigureDriverId = Get-LogKeyValue -Line $SmokeEvidence.asioConfigureOutputLine -Name "driverId"
        asioConfigureRate = Get-LogIntValueOrNull -Line $SmokeEvidence.asioConfigureOutputLine -Name "rate"
        asioActiveSampleRate = Get-LogDoubleValueOrNull -Line $SmokeEvidence.asioSampleRateActiveLine -Name "actual"
        asioPreferredBufferSize = Get-LogIntValueOrNull -Line $SmokeEvidence.asioBufferSizeLine -Name "preferred"
        asioPrepareOutputObserved = $SmokeEvidence.asioPrepareOutputObserved
        asioAudioStateActiveObserved = $SmokeEvidence.asioAudioStateActiveObserved
        asioFirstBufferSwitchObserved = $SmokeEvidence.asioFirstBufferSwitchObserved
        asioBackendStartVerified = $SmokeEvidence.asioBackendStartVerified
        seekRequested = $DiagnosticReport.seekRequested
        seekCompletedCount = $DiagnosticReport.seekCompletedCount
        seekResumePipelineCount = $DiagnosticReport.seekResumePipelineCount
        seekRequestTimeMs = $DiagnosticReport.seekRequestTimeMs
        pipelineStartTimeMs = $DiagnosticReport.pipelineStartTimeMs
        firstDecodedPcmAfterSeekMs = $DiagnosticReport.firstDecodedPcmAfterSeekMs
        firstSubmittedPcmAfterSeekMs = $DiagnosticReport.firstSubmittedPcmAfterSeekMs
        firstAudibleOrFadeOpenMs = $DiagnosticReport.firstAudibleOrFadeOpenMs
        seekResumeLatencyMs = $DiagnosticReport.seekResumeLatencyMs
        seekResumeStartupSilenceMs = $DiagnosticReport.seekResumeStartupSilenceMs
        seekResumeWarmupDiscardMs = $DiagnosticReport.seekResumeWarmupDiscardMs
        seekResumeFadeInMs = $DiagnosticReport.seekResumeFadeInMs
        firstSubmittedBlockPeak = $DiagnosticReport.firstSubmittedBlockPeak
        firstSubmittedBlockStartSample = $DiagnosticReport.firstSubmittedBlockStartSample
        firstSubmittedBlockEndSample = $DiagnosticReport.firstSubmittedBlockEndSample
        firstSubmittedBlockFadeApplied = $DiagnosticReport.firstSubmittedBlockFadeApplied
        firstSubmittedBlockMinGain = $DiagnosticReport.firstSubmittedBlockMinGain
        firstSubmittedBlockMaxGain = $DiagnosticReport.firstSubmittedBlockMaxGain
        first50msSubmittedPcmJumpAfterSeek = $DiagnosticReport.first50msSubmittedPcmJumpAfterSeek
        renderMirrorFirst50msAfterSeekObserved = $DiagnosticReport.renderMirrorFirst50msAfterSeekObserved
        renderMirrorFirst50msAfterSeekPeak = $DiagnosticReport.renderMirrorFirst50msAfterSeekPeak
        renderMirrorFirst50msAfterSeekArtifactDetected = $DiagnosticReport.renderMirrorFirst50msAfterSeekArtifactDetected
        realtimeDecodeEnabled = $DiagnosticReport.realtimeDecodeEnabled
        seekResumeMirrorObserved = $DiagnosticReport.seekResumeMirrorObserved
        seekResumeMirrorClean = $DiagnosticReport.seekResumeMirrorClean
        seekResumeMirrorArtifactDetected = $DiagnosticReport.seekResumeMirrorArtifactDetected
        artifactWindow = $DiagnosticReport.artifactWindow
        artifactClassification = $DiagnosticReport.artifactClassification
        firstArtifactOffsetMsAfterResume = $DiagnosticReport.firstArtifactOffsetMsAfterResume
        seekResumeBoundaryWindowMs = $DiagnosticReport.seekResumeBoundaryWindowMs
        compressedContentSample = $DiagnosticReport.compressedContentSample
        seekResumeBoundaryArtifactDetected = $DiagnosticReport.seekResumeBoundaryArtifactDetected
        seekResumeBoundaryArtifactCount = $DiagnosticReport.seekResumeBoundaryArtifactCount
        seekResumeFullSegmentArtifactCount = $DiagnosticReport.seekResumeFullSegmentArtifactCount
        seekResumeContentTransientLikely = $DiagnosticReport.seekResumeContentTransientLikely
        seekResumeContentTransientLikelyCount = $DiagnosticReport.seekResumeContentTransientLikelyCount
        seekResumeDetectorInconclusiveCount = $DiagnosticReport.seekResumeDetectorInconclusiveCount
        seekResumeHardArtifactEvidenceDetected = $DiagnosticReport.seekResumeHardArtifactEvidenceDetected
        seekResumeDetectorOnlyCaution = $DiagnosticReport.seekResumeDetectorOnlyCaution
        outputSwitchRequested = $DiagnosticReport.outputSwitchRequested
        outputSwitchCompleted = $DiagnosticReport.outputSwitchCompleted
        playbackContinuedAfterSwitch = $DiagnosticReport.playbackContinuedAfterSwitch
        activeOutputSwitchDetected = $DiagnosticReport.activeOutputSwitchDetected
        activeOutputSwitchStartedCount = $DiagnosticReport.activeOutputSwitchStartedCount
        activeOutputSwitchCompletedCount = $DiagnosticReport.activeOutputSwitchCompletedCount
        sameOutputInvalidationDetected = $DiagnosticReport.sameOutputInvalidationDetected
        sameOutputInvalidationCount = $DiagnosticReport.sameOutputInvalidationCount
        sameOutputInvalidationAbsorbedEventCount = $DiagnosticReport.sameOutputInvalidationAbsorbedEventCount
        wasapiErrorRecoveryDetected = $DiagnosticReport.wasapiErrorRecoveryDetected
        wasapiErrorRecoveryScheduledCount = $DiagnosticReport.wasapiErrorRecoveryScheduledCount
        wasapiErrorRecoveryStartCount = $DiagnosticReport.wasapiErrorRecoveryStartCount
        sourceSwitchRequested = $DiagnosticReport.sourceSwitchRequested
        sourceSwitchClean = $DiagnosticReport.sourceSwitchClean
        expectedErrorObserved = $SmokeEvidence.expectedErrorObserved
        loadCount = $SmokeEvidence.loadMatches.Count
        seekCompletionCount = $SmokeEvidence.seekCompletionMatches.Count
        waitingForInvalidationCount = $SmokeEvidence.waitingForInvalidationMatches.Count
        absorbedOutputErrorCount = $SmokeEvidence.absorbedOutputErrorMatches.Count
        conservativeRebuildCount = $SmokeEvidence.conservativeRebuildMatches.Count
        activeSwitchRebuildPipelineCount = $SmokeEvidence.activeSwitchRebuildPipelineMatches.Count
        firstDataBlockAfterConfigureCount = $SmokeEvidence.firstDataBlockAfterConfigureMatches.Count
        firstBlockGuardCount = $SmokeEvidence.firstBlockGuardMatches.Count
        activeSwitchBoundaryEnvelopeCount = $SmokeEvidence.activeSwitchBoundaryEnvelopeMatches.Count
        activeSwitchBoundaryPopEventCount = $SmokeEvidence.activeSwitchBoundaryPopCandidateLines.Count
        activeSwitchBoundaryPopCandidateCount = $SmokeEvidence.activeSwitchBoundaryPopPositiveLines.Count
        activeSwitchBoundaryPopTrigger = Get-LogKeyValue -Line $SmokeEvidence.activeSwitchBoundaryPopLastMetricLine -Name "activeSwitchTrigger"
        activeSwitchBoundaryPopReason = Get-LogKeyValue -Line $SmokeEvidence.activeSwitchBoundaryPopLastMetricLine -Name "activeSwitchReason"
        activeSwitchBoundaryPopFallbackToSilenceCount = $SmokeEvidence.activeSwitchBoundaryPopFallbackToSilenceCount
        activeSwitchBoundaryPopMaxPreviousToBridgeStep = Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "previousToBridgeEnvelopeStep"
        activeSwitchBoundaryPopMaxBridgeToFirstStep = Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "bridgeToFirstEnvelopeStep"
        activeSwitchBoundaryPopMaxFirstBlockFadeEndpoint = Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "firstBlockEndFadeGain"
        activeSwitchPreflightCount = $SmokeEvidence.activeSwitchPreflightMatches.Count
        activeSwitchPreFadeCount = $SmokeEvidence.activeSwitchPreFadeMatches.Count
        maxPositionMs = $SmokeEvidence.maxPositionMs
        maxAudioPeak = $SmokeEvidence.maxAudioPeak
        popClickVerification = $DiagnosticReport.popClickVerification
        actualEndpointOutputVerification = $DiagnosticReport.actualEndpointOutputVerification
        submittedPcmConclusion = $DiagnosticReport.submittedPcmConclusion
        renderMirrorObserved = $DiagnosticReport.renderMirrorObserved
        renderMirrorClean = $DiagnosticReport.renderMirrorClean
        renderMirrorArtifactDetected = $DiagnosticReport.renderMirrorArtifactDetected
        renderMirrorHardArtifactDetected = $DiagnosticReport.renderMirrorHardArtifactDetected
        bufferUnderrunDetected = $DiagnosticReport.bufferUnderrunDetected
        bufferUnderrunCount = $DiagnosticReport.bufferUnderrunCount
        decoderBackpressureDetected = $DiagnosticReport.decoderBackpressureDetected
        decoderBackpressureCount = $DiagnosticReport.decoderBackpressureCount
        positionStallOrLagDetected = $DiagnosticReport.positionStallOrLagDetected
        systemInvalidationDuringSwitch = $DiagnosticReport.systemInvalidationDuringSwitch
        oldPcmLeakDetected = $DiagnosticReport.oldPcmLeakDetected
        staleBufferReuseDetected = $DiagnosticReport.staleBufferReuseDetected
        staleSessionWriteDetected = $DiagnosticReport.staleSessionWriteDetected
        staleBufferReadDetected = $DiagnosticReport.staleBufferReadDetected
        staleSessionWriteCount = $DiagnosticReport.staleSessionWriteCount
        staleBufferReadCount = $DiagnosticReport.staleBufferReadCount
        submittedPcmDiscontinuityDetected = $DiagnosticReport.submittedPcmDiscontinuityDetected
        submittedPcmDiscontinuityCount = $DiagnosticReport.submittedPcmDiscontinuityCount
        submittedPcmHardDiscontinuityDetected = $DiagnosticReport.submittedPcmHardDiscontinuityDetected
        submittedPcmPeakJumpObserved = $DiagnosticReport.submittedPcmPeakJumpObserved
        submittedPcmMetricBlockCount = $DiagnosticReport.submittedPcmMetricBlockCount
        maxSubmittedPcmPeak = $DiagnosticReport.maxSubmittedPcmPeak
        maxSubmittedPcmJump = $DiagnosticReport.maxSubmittedPcmJump
        internalGlitchCandidatesDetected = $DiagnosticReport.internalGlitchCandidatesDetected
        stopFadeOutBeginCount = $SmokeEvidence.stopFadeOutBeginLines.Count
        stopFadeOutEndCount = $SmokeEvidence.stopFadeOutEndLines.Count
        stopFadeOutCompleted = $SmokeEvidence.stopFadeOutCompleted
        stopFadeOutFinalGain = $SmokeEvidence.stopFadeOutFinalGain
        stopFadeOutSubmittedFrames = Get-LogIntValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "submittedFadeFrames"
        stopFadeOutMaxSubmittedPeak = Get-LogDoubleValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "maxSubmittedPeak"
        stopFadeOutLastSubmittedSample = Get-LogDoubleValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "lastSubmittedSample"
        processCleanup = $ProcessCleanup
    }
    $assertions = [ordered]@{
        passed = $AssertionsPassed
        failure = $AssertionFailure
    }

    Write-HarnessReport -Path $RunHarnessReportPath `
        -Result $HarnessResult `
        -AppReportResult $SmokeEvidence.appReportResult `
        -FailureReasons $FailureReasons `
        -InconclusiveReasons $InconclusiveReasons `
        -Warnings $Warnings `
        -Files $files `
        -RequestedActions $RequestedActions `
        -ObservedActions $observedActions `
        -Assertions $assertions `
        -ManualObservation $ManualObservation `
        -ExitCode $ProcessExitCode `
        -BackendEvidence $SmokeEvidence.backendEvidence `
        -EvidenceLayer $SmokeEvidence.harnessEvidenceLayer `
        -VerificationLayer $SmokeEvidence.harnessVerificationLayer `
        -EndpointOutputVerified $false
}

function Write-SmokeDiagnosticOutput {
    param(
        [object]$DiagnosticReport,
        [object]$SmokeEvidence,
        [int]$AsioOutputIndex = [int]::MinValue,
        [object]$ProcessCleanup,
        [object]$ManualObservation,
        [string]$RunHarnessReportPath,
        [string]$HarnessResult,
        [bool]$AssertionsPassed,
        [string]$AssertionFailure = "",
        [int]$ProcessExitCode
    )

    Write-Output "loadCount:$($SmokeEvidence.loadMatches.Count)"
    Write-Output "seekCompletionCount:$($SmokeEvidence.seekCompletionMatches.Count)"
    Write-Output "waitingForInvalidationCount:$($SmokeEvidence.waitingForInvalidationMatches.Count)"
    Write-Output "absorbedOutputErrorCount:$($SmokeEvidence.absorbedOutputErrorMatches.Count)"
    Write-Output "conservativeRebuildCount:$($SmokeEvidence.conservativeRebuildMatches.Count)"
    Write-Output "activeSwitchRebuildPipelineCount:$($SmokeEvidence.activeSwitchRebuildPipelineMatches.Count)"
    Write-Output "firstDataBlockAfterConfigureCount:$($SmokeEvidence.firstDataBlockAfterConfigureMatches.Count)"
    Write-Output "firstBlockGuardCount:$($SmokeEvidence.firstBlockGuardMatches.Count)"
    Write-Output "activeSwitchBoundaryEnvelopeCount:$($SmokeEvidence.activeSwitchBoundaryEnvelopeMatches.Count)"
    Write-Output "activeSwitchBoundaryPopEventCount:$($SmokeEvidence.activeSwitchBoundaryPopCandidateLines.Count)"
    Write-Output "activeSwitchBoundaryPopCandidateCount:$($SmokeEvidence.activeSwitchBoundaryPopPositiveLines.Count)"
    Write-Output "activeSwitchBoundaryPopTrigger:$(Get-LogKeyValue -Line $SmokeEvidence.activeSwitchBoundaryPopLastMetricLine -Name "activeSwitchTrigger")"
    Write-Output "activeSwitchBoundaryPopReason:$(Get-LogKeyValue -Line $SmokeEvidence.activeSwitchBoundaryPopLastMetricLine -Name "activeSwitchReason")"
    Write-Output "activeSwitchBoundaryPopFallbackToSilenceCount:$($SmokeEvidence.activeSwitchBoundaryPopFallbackToSilenceCount)"
    Write-Output "activeSwitchBoundaryPopMaxPreviousToBridgeStep:$(Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "previousToBridgeEnvelopeStep")"
    Write-Output "activeSwitchBoundaryPopMaxBridgeToFirstStep:$(Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "bridgeToFirstEnvelopeStep")"
    Write-Output "activeSwitchBoundaryPopMaxFirstBlockFadeEndpoint:$(Get-MaxLogDoubleValue -Lines $SmokeEvidence.activeSwitchBoundaryPopMetricLines -Name "firstBlockEndFadeGain")"
    Write-Output "activeSwitchPreflightCount:$($SmokeEvidence.activeSwitchPreflightMatches.Count)"
    Write-Output "activeSwitchPreFadeCount:$($SmokeEvidence.activeSwitchPreFadeMatches.Count)"
    Write-Output "seekResumePipelineCount:$($SmokeEvidence.seekResumePipelineMatches.Count)"
    Write-Output "seekRequestTimeMs:$($DiagnosticReport.seekRequestTimeMs)"
    Write-Output "pipelineStartTimeMs:$($DiagnosticReport.pipelineStartTimeMs)"
    Write-Output "firstDecodedPcmAfterSeekMs:$($DiagnosticReport.firstDecodedPcmAfterSeekMs)"
    Write-Output "firstSubmittedPcmAfterSeekMs:$($DiagnosticReport.firstSubmittedPcmAfterSeekMs)"
    Write-Output "firstAudibleOrFadeOpenMs:$($DiagnosticReport.firstAudibleOrFadeOpenMs)"
    Write-Output "seekResumeLatencyMs:$($DiagnosticReport.seekResumeLatencyMs)"
    Write-Output "seekResumeStartupSilenceMs:$($DiagnosticReport.seekResumeStartupSilenceMs)"
    Write-Output "seekResumeWarmupDiscardMs:$($DiagnosticReport.seekResumeWarmupDiscardMs)"
    Write-Output "seekResumeFadeInMs:$($DiagnosticReport.seekResumeFadeInMs)"
    Write-Output "firstSubmittedBlockPeak:$($DiagnosticReport.firstSubmittedBlockPeak)"
    Write-Output "firstSubmittedBlockStartSample:$($DiagnosticReport.firstSubmittedBlockStartSample)"
    Write-Output "firstSubmittedBlockEndSample:$($DiagnosticReport.firstSubmittedBlockEndSample)"
    Write-Output "firstSubmittedBlockFadeApplied:$($DiagnosticReport.firstSubmittedBlockFadeApplied)"
    Write-Output "firstSubmittedBlockMinGain:$($DiagnosticReport.firstSubmittedBlockMinGain)"
    Write-Output "firstSubmittedBlockMaxGain:$($DiagnosticReport.firstSubmittedBlockMaxGain)"
    Write-Output "first50msSubmittedPcmJumpAfterSeek:$($DiagnosticReport.first50msSubmittedPcmJumpAfterSeek)"
    Write-Output "renderMirrorFirst50msAfterSeekObserved:$($DiagnosticReport.renderMirrorFirst50msAfterSeekObserved)"
    Write-Output "renderMirrorFirst50msAfterSeekPeak:$($DiagnosticReport.renderMirrorFirst50msAfterSeekPeak)"
    Write-Output "renderMirrorFirst50msAfterSeekArtifactDetected:$($DiagnosticReport.renderMirrorFirst50msAfterSeekArtifactDetected)"
    Write-Output "realtimeDecodeEnabled:$($DiagnosticReport.realtimeDecodeEnabled)"
    Write-Output "maxPosition:$($SmokeEvidence.maxPositionMs)"
    Write-Output "maxAudioPeak:$($SmokeEvidence.maxAudioPeak)"
    Write-Output "reportResult:$($DiagnosticReport.result)"
    Write-Output "loadedBackend:$($SmokeEvidence.loadedBackend)"
    Write-Output "asioOutputSelectionRequested:$($AsioOutputIndex -ne [int]::MinValue)"
    Write-Output "asioOutputSelectionConfirmed:$($SmokeEvidence.asioSelectionConfirmed)"
    Write-Output "asioSelectedIndex:$($SmokeEvidence.asioSelectedIndex)"
    Write-Output "asioSelectedId:$($SmokeEvidence.asioSelectedId)"
    Write-Output "asioSelectedDescription:$($SmokeEvidence.asioSelectedDescription)"
    Write-Output "asioOutputDeviceCount:$($SmokeEvidence.asioOutputDeviceCount)"
    Write-Output "asioConfigureOutputObserved:$($SmokeEvidence.asioConfigureOutputObserved)"
    Write-Output "asioConfigureDriverId:$(Get-LogKeyValue -Line $SmokeEvidence.asioConfigureOutputLine -Name "driverId")"
    Write-Output "asioConfigureRate:$(Get-LogIntValueOrNull -Line $SmokeEvidence.asioConfigureOutputLine -Name "rate")"
    Write-Output "asioActiveSampleRate:$(Get-LogDoubleValueOrNull -Line $SmokeEvidence.asioSampleRateActiveLine -Name "actual")"
    Write-Output "asioPreferredBufferSize:$(Get-LogIntValueOrNull -Line $SmokeEvidence.asioBufferSizeLine -Name "preferred")"
    Write-Output "asioPrepareOutputObserved:$($SmokeEvidence.asioPrepareOutputObserved)"
    Write-Output "asioAudioStateActiveObserved:$($SmokeEvidence.asioAudioStateActiveObserved)"
    Write-Output "asioFirstBufferSwitchObserved:$($SmokeEvidence.asioFirstBufferSwitchObserved)"
    Write-Output "asioBackendStartVerified:$($SmokeEvidence.asioBackendStartVerified)"
    Write-Output "popClickVerification:$($DiagnosticReport.popClickVerification)"
    Write-Output "submittedPcmConclusion:$($DiagnosticReport.submittedPcmConclusion)"
    Write-Output "renderMirrorObserved:$($DiagnosticReport.renderMirrorObserved)"
    Write-Output "renderMirrorClean:$($DiagnosticReport.renderMirrorClean)"
    Write-Output "renderMirrorArtifactDetected:$($DiagnosticReport.renderMirrorArtifactDetected)"
    Write-Output "renderMirrorHardArtifactDetected:$($DiagnosticReport.renderMirrorHardArtifactDetected)"
    Write-Output "seekResumeMirrorObserved:$($DiagnosticReport.seekResumeMirrorObserved)"
    Write-Output "seekResumeMirrorClean:$($DiagnosticReport.seekResumeMirrorClean)"
    Write-Output "seekResumeMirrorArtifactDetected:$($DiagnosticReport.seekResumeMirrorArtifactDetected)"
    Write-Output "artifactWindow:$($DiagnosticReport.artifactWindow)"
    Write-Output "artifactClassification:$($DiagnosticReport.artifactClassification)"
    Write-Output "firstArtifactOffsetMsAfterResume:$($DiagnosticReport.firstArtifactOffsetMsAfterResume)"
    Write-Output "seekResumeBoundaryWindowMs:$($DiagnosticReport.seekResumeBoundaryWindowMs)"
    Write-Output "compressedContentSample:$($DiagnosticReport.compressedContentSample)"
    Write-Output "seekResumeBoundaryArtifactDetected:$($DiagnosticReport.seekResumeBoundaryArtifactDetected)"
    Write-Output "seekResumeBoundaryArtifactCount:$($DiagnosticReport.seekResumeBoundaryArtifactCount)"
    Write-Output "seekResumeFullSegmentArtifactCount:$($DiagnosticReport.seekResumeFullSegmentArtifactCount)"
    Write-Output "seekResumeContentTransientLikely:$($DiagnosticReport.seekResumeContentTransientLikely)"
    Write-Output "seekResumeHardArtifactEvidenceDetected:$($DiagnosticReport.seekResumeHardArtifactEvidenceDetected)"
    Write-Output "seekResumeDetectorOnlyCaution:$($DiagnosticReport.seekResumeDetectorOnlyCaution)"
    Write-Output "bufferUnderrunDetected:$($DiagnosticReport.bufferUnderrunDetected)"
    Write-Output "bufferUnderrunCount:$($DiagnosticReport.bufferUnderrunCount)"
    Write-Output "decoderBackpressureDetected:$($DiagnosticReport.decoderBackpressureDetected)"
    Write-Output "decoderBackpressureCount:$($DiagnosticReport.decoderBackpressureCount)"
    Write-Output "positionStallOrLagDetected:$($DiagnosticReport.positionStallOrLagDetected)"
    Write-Output "systemInvalidationDuringSwitch:$($DiagnosticReport.systemInvalidationDuringSwitch)"
    Write-Output "sameOutputInvalidationDetected:$($DiagnosticReport.sameOutputInvalidationDetected)"
    Write-Output "sameOutputInvalidationCount:$($DiagnosticReport.sameOutputInvalidationCount)"
    Write-Output "sameOutputInvalidationAbsorbedEventCount:$($DiagnosticReport.sameOutputInvalidationAbsorbedEventCount)"
    Write-Output "wasapiErrorRecoveryDetected:$($DiagnosticReport.wasapiErrorRecoveryDetected)"
    Write-Output "oldPcmLeakDetected:$($DiagnosticReport.oldPcmLeakDetected)"
    Write-Output "sourceSwitchMirrorClean:$($DiagnosticReport.sourceSwitchMirrorClean)"
    Write-Output "sourceSwitchClean:$($DiagnosticReport.sourceSwitchClean)"
    Write-Output "staleSessionWriteDetected:$($DiagnosticReport.staleSessionWriteDetected)"
    Write-Output "staleBufferReadDetected:$($DiagnosticReport.staleBufferReadDetected)"
    Write-Output "submittedPcmDiscontinuityDetected:$($DiagnosticReport.submittedPcmDiscontinuityDetected)"
    Write-Output "submittedPcmHardDiscontinuityDetected:$($DiagnosticReport.submittedPcmHardDiscontinuityDetected)"
    Write-Output "maxSubmittedPcmPeak:$($DiagnosticReport.maxSubmittedPcmPeak)"
    Write-Output "maxSubmittedPcmJump:$($DiagnosticReport.maxSubmittedPcmJump)"
    Write-Output "internalGlitchCandidatesDetected:$($DiagnosticReport.internalGlitchCandidatesDetected)"
    Write-Output "stopFadeOutBeginCount:$($SmokeEvidence.stopFadeOutBeginLines.Count)"
    Write-Output "stopFadeOutEndCount:$($SmokeEvidence.stopFadeOutEndLines.Count)"
    Write-Output "stopFadeOutCompleted:$($SmokeEvidence.stopFadeOutCompleted)"
    Write-Output "stopFadeOutFinalGain:$($SmokeEvidence.stopFadeOutFinalGain)"
    Write-Output "stopFadeOutSubmittedFrames:$(Get-LogIntValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "submittedFadeFrames")"
    Write-Output "stopFadeOutMaxSubmittedPeak:$(Get-LogDoubleValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "maxSubmittedPeak")"
    Write-Output "stopFadeOutLastSubmittedSample:$(Get-LogDoubleValueOrNull -Line $SmokeEvidence.stopFadeOutLastEndLine -Name "lastSubmittedSample")"
    Write-Output "processCleanupTimedOut:$($ProcessCleanup.timedOut)"
    Write-Output "processCleanupKilled:$([string]::Join(',', @($ProcessCleanup.killedProcessIds)))"
    Write-Output "processCleanupResidual:$([string]::Join(',', @($ProcessCleanup.residualProcessIds)))"
    Write-Output "exit:$ProcessExitCode"
    Write-Output "manualObservationRequested:$($ManualObservation.requested)"
    Write-Output "manualObservationWindowSeconds:$($ManualObservation.windowSeconds)"
    Write-Output "manualObservationNote:$($ManualObservation.note)"
    Write-Output "manualEndpointResult:$($ManualObservation.result)"
    Write-Output "manualEndpointTiming:$($ManualObservation.timing)"
    Write-Output "harnessReport:$RunHarnessReportPath"
    Write-Output "harnessResult:$HarnessResult"
    if ($AssertionsPassed) {
        Write-Output "assertions:ok"
    } else {
        Write-Output "assertions:failed:$AssertionFailure"
    }
}
