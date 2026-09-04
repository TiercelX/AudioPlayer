# Log/report extraction helpers and backend evidence shaping for smoke tests.
# Dot-source this file from scripts that need log evidence parsing after a
# playback run completes.
#
# Depends on harness-common.ps1 for: Get-LogKeyValue, Get-LogFlag,
# Get-LogIntValueOrNull, Get-LogDoubleValueOrNull, Get-MaxLogDoubleValue,
# Add-UniqueString.

function Read-SmokeLogEvidence {
    param(
        [string[]]$LogLines,
        [object]$DiagnosticReport,
        [int]$AsioOutputIndex = [int]::MinValue,
        [string]$RequireErrorPattern = "",
        [bool]$RejectPlaybackErrors = $false,
        [System.Collections.Generic.List[string]]$FailureReasons,
        [System.Collections.Generic.List[string]]$InconclusiveReasons,
        [System.Collections.Generic.List[string]]$Warnings
    )

    $loadLines = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*load backend=' |
            ForEach-Object { $_.Line }
    )
    $positionMatches = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*positionMs=(\d+)' |
            ForEach-Object {
                [long]$_.Matches[0].Groups[1].Value
            }
    )
    $seekCompletionMatches = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*seekCompleted target=(\d+) actual=(\d+)' |
            ForEach-Object {
                [PSCustomObject]@{
                    Target = [long]$_.Matches[0].Groups[1].Value
                    Actual = [long]$_.Matches[0].Groups[2].Value
                }
            }
    )
    $loadMatches = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*load backend='
    )
    $audioLevelMatches = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*audioLevel peak=([0-9.]+)' |
            ForEach-Object {
                [double]$_.Matches[0].Groups[1].Value
            }
    )
    $waitingForInvalidationMatches = @(
        $LogLines |
            Select-String -Pattern 'activeOutputSwitch phase .* -> WaitingForInvalidation .*trigger=OutputFormatChange.*transactionReason=audioOutputsChanged'
    )
    $absorbedOutputErrorMatches = @(
        $LogLines |
            Select-String -Pattern 'activeOutputSwitch absorbed-output-error .*trigger=OutputFormatChange phase=WaitingForInvalidation reason=audioOutputsChanged'
    )
    $conservativeRebuildMatches = @(
        $LogLines |
            Select-String -Pattern 'activeOutputSwitch conservative-rebuild .*trigger=OutputFormatChange phase=WaitingForInvalidation'
    )
    $activeSwitchRebuildPipelineMatches = @(
        $LogLines |
            Select-String -Pattern 'startPipeline startPositionMs=.*pipelineStartProfile=ActiveSwitchRebuild'
    )
    $firstDataBlockAfterConfigureMatches = @(
        $LogLines |
            Select-String -Pattern '\[audio\].*firstDataBlockAfterConfigure .*pipelineStartProfile=ActiveSwitchRebuild'
    )
    $firstBlockGuardMatches = @(
        $LogLines |
            Select-String -Pattern 'activeSwitchFirstBlockEntryGuard '
    )
    $activeSwitchBoundaryEnvelopeMatches = @(
        $LogLines |
            Select-String -Pattern '\[audio\].*activeSwitchBoundaryEnvelope .*pipelineStartProfile=ActiveSwitchRebuild'
    )
    $activeSwitchBoundaryPopCandidateMatches = @(
        $LogLines |
            Select-String -Pattern '\[audio\].*activeSwitchBoundaryPopCandidate '
    )
    $activeSwitchBoundaryPopCandidateLines = @($activeSwitchBoundaryPopCandidateMatches | ForEach-Object { $_.Line })
    $activeSwitchBoundaryPopPositiveLines = @($activeSwitchBoundaryPopCandidateLines | Where-Object {
        Get-LogFlag -Line $_ -Name "candidate"
    })
    $activeSwitchBoundaryPopMetricLines = @(
        if ($activeSwitchBoundaryPopPositiveLines.Count -gt 0) {
            $activeSwitchBoundaryPopPositiveLines
        } else {
            $activeSwitchBoundaryPopCandidateLines
        }
    )
    $activeSwitchBoundaryPopLastMetricLine = if ($activeSwitchBoundaryPopMetricLines.Count -gt 0) {
        [string]$activeSwitchBoundaryPopMetricLines[-1]
    } else {
        ""
    }
    $activeSwitchBoundaryPopFallbackToSilenceCount = @($activeSwitchBoundaryPopCandidateLines | Where-Object {
        Get-LogFlag -Line $_ -Name "fallbackToSilence"
    }).Count
    $activeSwitchPreflightMatches = @(
        $LogLines |
            Select-String -Pattern 'activeSwitchPreflight decision='
    )
    $activeSwitchPreFadeMatches = @(
        $LogLines |
            Select-String -Pattern 'activeSwitchPreFade '
    )
    $seekResumePipelineMatches = @(
        $LogLines |
            Select-String -Pattern 'startPipeline startPositionMs=.*pipelineStartProfile=SeekResume'
    )
    $maxPositionMs = if ($positionMatches.Count -gt 0) {
        ($positionMatches | Measure-Object -Maximum).Maximum
    } else {
        -1
    }
    $maxAudioPeak = if ($audioLevelMatches.Count -gt 0) {
        ($audioLevelMatches | Measure-Object -Maximum).Maximum
    } else {
        0.0
    }
    $latestLoadLine = if ($loadLines.Count -gt 0) {
        [string]$loadLines[-1]
    } else {
        ""
    }
    $loadedBackend = if ($latestLoadLine -match '\[automation\].*load backend=(?<backend>.*?)\s+source=') {
        $Matches.backend
    } else {
        ""
    }
    $asioOutputDeviceListLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[automation\].*asioOutputDeviceList count=' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioOutputDeviceCount = $null
    $asioOutputDeviceCountText = Get-LogKeyValue -Line $asioOutputDeviceListLine -Name "count"
    $parsedAsioOutputDeviceCount = 0
    if ([int]::TryParse($asioOutputDeviceCountText, [ref]$parsedAsioOutputDeviceCount)) {
        $asioOutputDeviceCount = $parsedAsioOutputDeviceCount
    }
    $asioSelectionAutomationLines = @(
        $LogLines |
            Select-String -Pattern '\[automation\].*action=select-asio-output-device ' |
            ForEach-Object { $_.Line }
    )
    $asioSelectionConfirmed = $asioSelectionAutomationLines.Count -gt 0
    $asioSelectionLine = if ($asioSelectionConfirmed) {
        [string]$asioSelectionAutomationLines[-1]
    } else {
        ""
    }
    $asioSelectedIndex = $null
    $asioSelectedIndexText = Get-LogKeyValue -Line $asioSelectionLine -Name "index"
    $parsedAsioSelectedIndex = 0
    if ([int]::TryParse($asioSelectedIndexText, [ref]$parsedAsioSelectedIndex)) {
        $asioSelectedIndex = $parsedAsioSelectedIndex
    }
    $asioSelectedId = Get-LogKeyValue -Line $asioSelectionLine -Name "id"
    $asioSelectionUiLine = [string](
        @(
            $LogLines |
                Select-String -Pattern 'selectAsioOutputDeviceByIndex index=' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioSelectedDescription = if ($asioSelectionUiLine -match 'description=(?<description>.*)$') {
        $Matches.description
    } else {
        ""
    }
    $asioConfigureOutputLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio configureOutput ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioSampleRateActiveLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio sampleRate active ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioDriverChannelsLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio driverChannels ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioBufferSizeLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio bufferSize ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioPrepareOutputLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[player\].*asio prepareOutput ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioAudioStateActiveLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[player\].*asio audioStateChanged .*state=Active' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioFirstBufferSwitchLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio firstBufferSwitch ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioSetSampleRateFailedLine = [string](
        @(
            $LogLines |
                Select-String -Pattern '\[audio\].*asio setSampleRate failed ' |
                ForEach-Object { $_.Line }
        ) | Select-Object -Last 1
    )
    $asioConfigureOutputObserved = -not [string]::IsNullOrWhiteSpace($asioConfigureOutputLine)
    $asioPrepareOutputObserved = -not [string]::IsNullOrWhiteSpace($asioPrepareOutputLine) -and (Get-LogFlag -Line $asioPrepareOutputLine -Name "prepared")
    $asioAudioStateActiveObserved = -not [string]::IsNullOrWhiteSpace($asioAudioStateActiveLine)
    $asioFirstBufferSwitchObserved = -not [string]::IsNullOrWhiteSpace($asioFirstBufferSwitchLine)
    $asioLoaded = -not [string]::IsNullOrWhiteSpace($loadedBackend) -and $loadedBackend -like "*ASIO*"
    $asioBackendStartVerified = ($AsioOutputIndex -ne [int]::MinValue) `
        -and $asioSelectionConfirmed `
        -and $asioLoaded `
        -and [bool]$DiagnosticReport.playbackStarted `
        -and $asioConfigureOutputObserved `
        -and $asioPrepareOutputObserved `
        -and $asioAudioStateActiveObserved `
        -and $asioFirstBufferSwitchObserved

    $appReportResult = [string]$DiagnosticReport.result
    $compressedContentSample = [bool]$DiagnosticReport.compressedContentSample
    $artifactClassification = [string]$DiagnosticReport.artifactClassification
    $seekResumeDetectorOnlyCaution = [bool]$DiagnosticReport.seekResumeDetectorOnlyCaution
    $seekResumeHardArtifactEvidenceDetected = [bool]$DiagnosticReport.seekResumeHardArtifactEvidenceDetected
    $seekResumeBoundaryArtifactDetected = [bool]$DiagnosticReport.seekResumeBoundaryArtifactDetected
    $expectedErrorObserved = -not [string]::IsNullOrWhiteSpace($RequireErrorPattern) -and [bool](
        $LogLines | Select-String -Pattern "\[automation\].*error message=.*$RequireErrorPattern" -Quiet
    )
    $expectedAppReportFailure = $appReportResult -eq "FAIL" -and $expectedErrorObserved -and -not $RejectPlaybackErrors
    if ($DiagnosticReport.systemInvalidationDuringSwitch) {
        Add-UniqueString -List $Warnings -Value "system-invalidation-during-switch"
    }
    if ($DiagnosticReport.popClickVerification -eq "INCONCLUSIVE") {
        Add-UniqueString -List $Warnings -Value "actual-endpoint-output-not-verified"
    }
    if ($compressedContentSample -and $DiagnosticReport.seekResumeMirrorArtifactDetected) {
        Add-UniqueString -List $Warnings -Value "compressed-content-seek-resume-artifact-classification:$artifactClassification"
    }
    if ($seekResumeBoundaryArtifactDetected -and -not $seekResumeHardArtifactEvidenceDetected) {
        Add-UniqueString -List $Warnings -Value "seek-resume-boundary-candidate-cautious"
    }
    $asioRenderMirrorObserved = [bool]$DiagnosticReport.asioRenderMirrorObserved
    $asioRenderMirrorClean = [bool]$DiagnosticReport.asioRenderMirrorClean
    $asioRenderMirrorArtifactDetected = [bool]$DiagnosticReport.asioRenderMirrorArtifactDetected
    if ($AsioOutputIndex -ne [int]::MinValue) {
        if (-not $asioRenderMirrorObserved) {
            Add-UniqueString -List $Warnings -Value "asio-submitted-output-evidence-not-captured"
        }
        Add-UniqueString -List $InconclusiveReasons -Value "asio-output-evidence-not-verified"
        if ($asioBackendStartVerified) {
            if ($asioRenderMirrorObserved -and $asioRenderMirrorClean) {
                Add-UniqueString -List $InconclusiveReasons -Value "asio-submitted-output-clean-endpoint-not-verified"
            } else {
                Add-UniqueString -List $InconclusiveReasons -Value "asio-selection-start-only"
            }
        } else {
            Add-UniqueString -List $InconclusiveReasons -Value "asio-backend-start-not-fully-observed"
        }
    }

    # Bit-depth precision evidence
    $bitDepthMatchLine = $LogLines |
        Select-String -Pattern 'selectOutputFormat.*bitDepthMatch=' |
        ForEach-Object { $_.Line } |
        Select-Object -Last 1
    $bitDepthMatch = if ($bitDepthMatchLine) {
        Get-LogKeyValue -Line $bitDepthMatchLine -Name "bitDepthMatch"
    } else { "" }
    $sourceBitDepth = if ($bitDepthMatchLine) {
        Get-LogKeyValue -Line $bitDepthMatchLine -Name "sourceBitDepth"
    } else { "" }
    $outputBits = if ($bitDepthMatchLine) {
        Get-LogKeyValue -Line $bitDepthMatchLine -Name "bits"
    } else { "" }
    $noiseShapingEnabled = [bool](
        $LogLines | Select-String -Pattern 'noiseShaping enabled' -Quiet
    )
    $stopFadeOutBeginLines = @(
        $LogLines |
            Select-String -Pattern '\[audio\].*stopPcmFadeOut begin ' |
            ForEach-Object { $_.Line }
    )
    $stopFadeOutEndLines = @(
        $LogLines |
            Select-String -Pattern '\[audio\].*stopPcmFadeOut end ' |
            ForEach-Object { $_.Line }
    )
    $stopFadeOutLastEndLine = if ($stopFadeOutEndLines.Count -gt 0) {
        [string]$stopFadeOutEndLines[-1]
    } else {
        ""
    }
    $stopFadeOutFinalGain = Get-LogDoubleValueOrNull -Line $stopFadeOutLastEndLine -Name "finalGain"
    $stopFadeOutCompletedFlag = Get-LogIntValueOrNull -Line $stopFadeOutLastEndLine -Name "completed"
    $stopFadeOutCompleted = $stopFadeOutBeginLines.Count -gt 0 `
        -and $stopFadeOutEndLines.Count -gt 0 `
        -and $stopFadeOutCompletedFlag -eq 1 `
        -and $null -ne $stopFadeOutFinalGain `
        -and $stopFadeOutFinalGain -le 0.001

    $harnessEvidenceLayer = if ($AsioOutputIndex -ne [int]::MinValue) {
        if ($asioRenderMirrorObserved) {
            "scripted-playback-asio-selection-start-submitted-output-and-app-report"
        } else {
            "scripted-playback-asio-selection-start-and-app-report"
        }
    } else {
        "scripted-playback-and-app-report"
    }
    $harnessVerificationLayer = if ($AsioOutputIndex -ne [int]::MinValue) {
        if ($asioRenderMirrorObserved) {
            "asio-selection-start-submitted-output-and-app-report"
        } else {
            "asio-selection-start-and-app-report"
        }
    } else {
        "scripted-playback-and-app-report"
    }
    $backendEvidenceLimitations = [System.Collections.Generic.List[string]]::new()
    $backendSubmittedOutputVerified = if ($AsioOutputIndex -ne [int]::MinValue) {
        $asioRenderMirrorObserved -and -not $asioRenderMirrorArtifactDetected
    } else {
        [bool]$DiagnosticReport.renderMirrorObserved -and -not [bool]$DiagnosticReport.renderMirrorArtifactDetected
    }
    if ($AsioOutputIndex -ne [int]::MinValue) {
        if (-not $asioRenderMirrorObserved) {
            Add-UniqueString -List $backendEvidenceLimitations -Value "asio-submitted-output-pcm-not-captured"
        }
        Add-UniqueString -List $backendEvidenceLimitations -Value "endpoint-output-not-verified"
        if (-not $asioFirstBufferSwitchObserved) {
            Add-UniqueString -List $backendEvidenceLimitations -Value "asio-driver-callback-not-observed"
        }
    } elseif ($DiagnosticReport.popClickVerification -eq "INCONCLUSIVE") {
        if (-not $backendSubmittedOutputVerified) {
            Add-UniqueString -List $backendEvidenceLimitations -Value "submitted-output-not-verified"
        }
        Add-UniqueString -List $backendEvidenceLimitations -Value "endpoint-output-not-verified"
    }
    $backendEvidence = [ordered]@{
        backend = if ([string]::IsNullOrWhiteSpace($loadedBackend)) { "unknown" } else { $loadedBackend }
        scope = if ($AsioOutputIndex -ne [int]::MinValue) {
            if ($asioBackendStartVerified -and $backendSubmittedOutputVerified) {
                "asio-selection-start-submitted-output-and-driver-callback"
            } elseif ($asioBackendStartVerified) {
                "asio-selection-start-and-driver-callback"
            } else {
                "asio-selection-start-requested"
            }
        } else {
            "scripted-playback-and-app-report"
        }
        backendStartVerified = if ($AsioOutputIndex -ne [int]::MinValue) {
            $asioBackendStartVerified
        } else {
            [bool]$DiagnosticReport.playbackStarted
        }
        submittedOutputVerified = $backendSubmittedOutputVerified
        endpointOutputVerified = $false
        limitations = @($backendEvidenceLimitations.ToArray())
        asio = [ordered]@{
            requested = ($AsioOutputIndex -ne [int]::MinValue)
            requestedIndex = if ($AsioOutputIndex -ne [int]::MinValue) { $AsioOutputIndex } else { $null }
            selectionConfirmed = $asioSelectionConfirmed
            selectedIndex = $asioSelectedIndex
            selectedId = $asioSelectedId
            selectedDescription = $asioSelectedDescription
            outputDeviceCount = $asioOutputDeviceCount
            loadedBackendIsAsio = $asioLoaded
            configureOutputObserved = $asioConfigureOutputObserved
            configureSession = Get-LogIntValueOrNull -Line $asioConfigureOutputLine -Name "session"
            configureDriverId = Get-LogKeyValue -Line $asioConfigureOutputLine -Name "driverId"
            configureRate = Get-LogIntValueOrNull -Line $asioConfigureOutputLine -Name "rate"
            configureChannels = Get-LogIntValueOrNull -Line $asioConfigureOutputLine -Name "channels"
            configureSampleFormat = Get-LogIntValueOrNull -Line $asioConfigureOutputLine -Name "sampleFormat"
            activeSampleRate = Get-LogDoubleValueOrNull -Line $asioSampleRateActiveLine -Name "actual"
            setSampleRateFailed = -not [string]::IsNullOrWhiteSpace($asioSetSampleRateFailedLine)
            driverInputChannels = Get-LogIntValueOrNull -Line $asioDriverChannelsLine -Name "inputs"
            driverOutputChannels = Get-LogIntValueOrNull -Line $asioDriverChannelsLine -Name "outputs"
            requestedOutputChannels = Get-LogIntValueOrNull -Line $asioDriverChannelsLine -Name "requestedOutputs"
            preferredBufferSize = Get-LogIntValueOrNull -Line $asioBufferSizeLine -Name "preferred"
            prepareOutputObserved = $asioPrepareOutputObserved
            audioStateActiveObserved = $asioAudioStateActiveObserved
            firstBufferSwitchObserved = $asioFirstBufferSwitchObserved
            backendStartVerified = $asioBackendStartVerified
            submittedOutputVerified = $backendSubmittedOutputVerified
            submittedOutputEvidenceLayer = if ($asioRenderMirrorObserved) { "asio-submitted-pcm-artifact-monitor" } else { "not-collected" }
            endpointOutputVerified = $false
        }
    }

    return [ordered]@{
        loadLines = $loadLines
        positionMatches = $positionMatches
        seekCompletionMatches = $seekCompletionMatches
        loadMatches = $loadMatches
        audioLevelMatches = $audioLevelMatches
        waitingForInvalidationMatches = $waitingForInvalidationMatches
        absorbedOutputErrorMatches = $absorbedOutputErrorMatches
        conservativeRebuildMatches = $conservativeRebuildMatches
        activeSwitchRebuildPipelineMatches = $activeSwitchRebuildPipelineMatches
        firstDataBlockAfterConfigureMatches = $firstDataBlockAfterConfigureMatches
        firstBlockGuardMatches = $firstBlockGuardMatches
        activeSwitchBoundaryEnvelopeMatches = $activeSwitchBoundaryEnvelopeMatches
        activeSwitchBoundaryPopCandidateMatches = $activeSwitchBoundaryPopCandidateMatches
        activeSwitchBoundaryPopCandidateLines = $activeSwitchBoundaryPopCandidateLines
        activeSwitchBoundaryPopPositiveLines = $activeSwitchBoundaryPopPositiveLines
        activeSwitchBoundaryPopMetricLines = $activeSwitchBoundaryPopMetricLines
        activeSwitchBoundaryPopLastMetricLine = $activeSwitchBoundaryPopLastMetricLine
        activeSwitchBoundaryPopFallbackToSilenceCount = $activeSwitchBoundaryPopFallbackToSilenceCount
        activeSwitchPreflightMatches = $activeSwitchPreflightMatches
        activeSwitchPreFadeMatches = $activeSwitchPreFadeMatches
        seekResumePipelineMatches = $seekResumePipelineMatches
        maxPositionMs = $maxPositionMs
        maxAudioPeak = $maxAudioPeak
        latestLoadLine = $latestLoadLine
        loadedBackend = $loadedBackend
        asioOutputDeviceListLine = $asioOutputDeviceListLine
        asioOutputDeviceCount = $asioOutputDeviceCount
        asioSelectionAutomationLines = $asioSelectionAutomationLines
        asioSelectionConfirmed = $asioSelectionConfirmed
        asioSelectionLine = $asioSelectionLine
        asioSelectedIndex = $asioSelectedIndex
        asioSelectedId = $asioSelectedId
        asioSelectionUiLine = $asioSelectionUiLine
        asioSelectedDescription = $asioSelectedDescription
        asioConfigureOutputLine = $asioConfigureOutputLine
        asioSampleRateActiveLine = $asioSampleRateActiveLine
        asioDriverChannelsLine = $asioDriverChannelsLine
        asioBufferSizeLine = $asioBufferSizeLine
        asioPrepareOutputLine = $asioPrepareOutputLine
        asioAudioStateActiveLine = $asioAudioStateActiveLine
        asioFirstBufferSwitchLine = $asioFirstBufferSwitchLine
        asioSetSampleRateFailedLine = $asioSetSampleRateFailedLine
        asioConfigureOutputObserved = $asioConfigureOutputObserved
        asioPrepareOutputObserved = $asioPrepareOutputObserved
        asioAudioStateActiveObserved = $asioAudioStateActiveObserved
        asioFirstBufferSwitchObserved = $asioFirstBufferSwitchObserved
        asioLoaded = $asioLoaded
        asioBackendStartVerified = $asioBackendStartVerified
        appReportResult = $appReportResult
        compressedContentSample = $compressedContentSample
        artifactClassification = $artifactClassification
        seekResumeDetectorOnlyCaution = $seekResumeDetectorOnlyCaution
        seekResumeHardArtifactEvidenceDetected = $seekResumeHardArtifactEvidenceDetected
        seekResumeBoundaryArtifactDetected = $seekResumeBoundaryArtifactDetected
        expectedErrorObserved = $expectedErrorObserved
        expectedAppReportFailure = $expectedAppReportFailure
        asioRenderMirrorObserved = $asioRenderMirrorObserved
        asioRenderMirrorClean = $asioRenderMirrorClean
        asioRenderMirrorArtifactDetected = $asioRenderMirrorArtifactDetected
        bitDepthMatch = $bitDepthMatch
        sourceBitDepth = $sourceBitDepth
        outputBits = $outputBits
        noiseShapingEnabled = $noiseShapingEnabled
        stopFadeOutBeginLines = $stopFadeOutBeginLines
        stopFadeOutEndLines = $stopFadeOutEndLines
        stopFadeOutLastEndLine = $stopFadeOutLastEndLine
        stopFadeOutFinalGain = $stopFadeOutFinalGain
        stopFadeOutCompleted = $stopFadeOutCompleted
        harnessEvidenceLayer = $harnessEvidenceLayer
        harnessVerificationLayer = $harnessVerificationLayer
        backendSubmittedOutputVerified = $backendSubmittedOutputVerified
        backendEvidence = $backendEvidence
    }
}
