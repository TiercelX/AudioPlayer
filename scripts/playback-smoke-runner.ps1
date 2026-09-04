# Path setup, parameter validation, and process launch for smoke tests.
# Dot-source this file from scripts that need to launch and manage the
# AudioPlayer process for a smoke test run.
#
# Depends on harness-common.ps1 for: Add-ProcessArgument, Add-UniqueString,
# Stop-TrackedProcessTree, Write-HarnessReport.
# Depends on common-paths.ps1 for: Resolve-AudioPlayerBuildDir,
# Resolve-AudioPlayerAppExePath, Resolve-AudioPlayerLogDir.
# Depends on cleanup-test-artifacts.ps1 for: Invoke-TestArtifactRetention.

function Write-EarlyFailureReport {
    param(
        [hashtable]$Context,
        [string]$Result,
        [string]$AppReportResult = "",
        [object]$ObservedActions,
        [object]$Assertions
    )

    Write-HarnessReport -Path $Context.runHarnessReportPath `
        -Result $Result `
        -AppReportResult $AppReportResult `
        -FailureReasons $Context.failureReasons `
        -InconclusiveReasons $Context.inconclusiveReasons `
        -Warnings $Context.warnings `
        -Files ([ordered]@{
            textLogFile = $Context.runLogPath
            jsonlDiagnosticFile = $null
            appReportFile = $Context.runReportPath
            harnessReportFile = $Context.runHarnessReportPath
        }) `
        -RequestedActions $Context.requestedActions `
        -ObservedActions $ObservedActions `
        -Assertions $Assertions `
        -ManualObservation $Context.manualObservation `
        -ExitCode $Context.exitCode
}

function Invoke-SmokeRunner {
    param(
        [hashtable]$BoundParameters,
        [string]$Source,
        [string]$BuildDir,
        [string]$Configuration,
        [int]$QuitAfterMs,
        [int]$SeekAfterMs,
        [long]$SeekToMs,
        [int]$SecondSeekAfterMs,
        [long]$SecondSeekToMs,
        [string]$SeekSequence,
        [int]$PauseAfterMs,
        [int]$ResumeAfterMs,
        [int]$StopAfterMs,
        [int]$RefreshOutputAfterMs,
        [int]$RefreshOutputCount,
        [int]$RefreshOutputIntervalMs,
        [switch]$ListOutputDevices,
        [int]$OutputDeviceIndex,
        [int]$AsioOutputIndex,
        [switch]$ExclusiveMode,
        [int]$SwitchOutputAfterMs,
        [int]$SwitchOutputToIndex,
        [int]$RepeatOutputSwitch,
        [int]$SwitchIntervalMs,
        [int]$ManualObservationWindowSeconds,
        [string]$ManualObservationNote,
        [string]$ManualEndpointResult,
        [string]$ManualEndpointTiming,
        [string]$ReportFile,
        [string]$HarnessReportFile,
        [int]$SwitchSourceAfterMs,
        [string]$SwitchSource,
        [int]$GracePeriodMs,
        [switch]$SeekPauseResume,
        [switch]$RequirePlaying,
        [switch]$RequirePauseResume,
        [switch]$RequireStop,
        [switch]$RequireStopFadeOut,
        [switch]$RejectPlaybackErrors,
        [string]$RequireErrorPattern = "",
        [switch]$RequireAudibleLevels,
        [switch]$RequireSourceSwitchClean,
        [switch]$RequireFinished,
        [int]$RequireFinishedBufferMs,
        [string]$RefreshOutputTrigger,
        [string]$HotReconfigureCompatibilityOverride,
        [string]$LogPath,
        [string]$FfmpegPathOverride,
        [string]$FfprobePathOverride
    )

    $repoRoot = Split-Path -Parent $PSScriptRoot

    # ── Source / exe path resolution ──
    $sourceCandidate = if ([System.IO.Path]::IsPathRooted($Source)) {
        $Source
    } else {
        Join-Path $repoRoot $Source
    }
    $sourcePath = [System.IO.Path]::GetFullPath($sourceCandidate)
    if (-not (Test-Path $sourcePath -PathType Leaf)) {
        throw "Source file not found: $sourcePath"
    }

    $exePath = Resolve-AudioPlayerAppExePath `
        -RepoRoot $repoRoot `
        -BuildDir $BuildDir `
        -Configuration $Configuration
    if (-not (Test-Path $exePath -PathType Leaf)) {
        throw "Built app not found: $exePath"
    }

    # ── ffmpeg/ffprobe resolution ──
    $slimWithProbeDir = Join-Path $repoRoot "$BuildDir\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin"
    $slimRuntimeDir = Join-Path $repoRoot "$BuildDir\ffmpeg-audio-core\runtime-deploy-msvc\bin"
    $defaultFfmpegPath = Resolve-FirstExistingPath @(
        (Join-Path $slimWithProbeDir "ffmpeg.exe"),
        (Join-Path $slimRuntimeDir "ffmpeg.exe")
    )
    $defaultFfprobePath = Resolve-FirstExistingPath @(
        (Join-Path $slimWithProbeDir "ffprobe.exe")
    )
    $effectiveFfprobePath = $defaultFfprobePath
    if ($BoundParameters.ContainsKey("FfprobePathOverride")) {
        if ($FfprobePathOverride -ieq "disabled" -or
            $FfprobePathOverride -ieq "none" -or
            $FfprobePathOverride -ieq "off" -or
            [string]::IsNullOrWhiteSpace($FfprobePathOverride)) {
            $effectiveFfprobePath = ""
        } else {
            $effectiveFfprobePath = if ([System.IO.Path]::IsPathRooted($FfprobePathOverride)) {
                $FfprobePathOverride
            } else {
                Join-Path $repoRoot $FfprobePathOverride
            }
        }
    }

    # ── Parameter validation ──
    if (($SeekAfterMs -ge 0) -xor ($SeekToMs -ge 0)) {
        throw "SeekAfterMs and SeekToMs must be provided together"
    }

    if (($SecondSeekAfterMs -ge 0) -xor ($SecondSeekToMs -ge 0)) {
        throw "SecondSeekAfterMs and SecondSeekToMs must be provided together"
    }

    if (-not [string]::IsNullOrWhiteSpace($SeekSequence)) {
        $seekSequenceEntries = @($SeekSequence.Split(",") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($seekSequenceEntries.Count -eq 0) {
            throw "SeekSequence must contain at least one afterMs:targetMs entry"
        }
        foreach ($entry in $seekSequenceEntries) {
            if ($entry -notmatch '^\s*\d+\s*:\s*\d+\s*$') {
                throw "SeekSequence entry '$entry' must use afterMs:targetMs"
            }
        }
    }

    if (($SwitchSourceAfterMs -ge 0) -xor (-not [string]::IsNullOrWhiteSpace($SwitchSource))) {
        throw "SwitchSourceAfterMs and SwitchSource must be provided together"
    }

    if ($OutputDeviceIndex -ne [int]::MinValue -and $AsioOutputIndex -ne [int]::MinValue) {
        throw "OutputDeviceIndex and AsioOutputIndex cannot be provided together"
    }

    if ($RefreshOutputCount -lt 1) {
        throw "RefreshOutputCount must be at least 1"
    }

    if ($RefreshOutputIntervalMs -lt 0) {
        throw "RefreshOutputIntervalMs must not be negative"
    }

    if ($RefreshOutputCount -gt 1 -and $RefreshOutputAfterMs -lt 0) {
        throw "RefreshOutputAfterMs must be provided when RefreshOutputCount is greater than 1"
    }

    if ($RefreshOutputCount -gt 1 -and $RefreshOutputIntervalMs -le 0) {
        throw "RefreshOutputIntervalMs must be greater than 0 when RefreshOutputCount is greater than 1"
    }

    if (($SwitchOutputAfterMs -ge 0) -xor ($SwitchOutputToIndex -ne [int]::MinValue)) {
        throw "SwitchOutputAfterMs and SwitchOutputToIndex must be provided together"
    }

    if ($RepeatOutputSwitch -lt 1) {
        throw "RepeatOutputSwitch must be at least 1"
    }

    if ($RepeatOutputSwitch -gt 1 -and $SwitchOutputAfterMs -lt 0) {
        throw "SwitchOutputAfterMs must be provided when RepeatOutputSwitch is greater than 1"
    }

    if ($RepeatOutputSwitch -gt 1 -and $SwitchIntervalMs -le 0) {
        throw "SwitchIntervalMs must be greater than 0 when RepeatOutputSwitch is greater than 1"
    }

    $switchSourcePath = ""
    if (-not [string]::IsNullOrWhiteSpace($SwitchSource)) {
        $switchSourceCandidate = if ([System.IO.Path]::IsPathRooted($SwitchSource)) {
            $SwitchSource
        } else {
            Join-Path $repoRoot $SwitchSource
        }
        $switchSourcePath = [System.IO.Path]::GetFullPath($switchSourceCandidate)
        if (-not (Test-Path $switchSourcePath -PathType Leaf)) {
            throw "Switch source file not found: $switchSourcePath"
        }
    }

    # ── Log / report path setup ──
    $logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
    $runLogPath = if ([string]::IsNullOrWhiteSpace($LogPath)) {
        $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), ([guid]::NewGuid().ToString("N").Substring(0, 8))
        Join-Path $logDir "player-smoke-$runId.log"
    } elseif ([System.IO.Path]::IsPathRooted($LogPath)) {
        $LogPath
    } else {
        Join-Path $repoRoot $LogPath
    }
    $runLogPath = [System.IO.Path]::GetFullPath($runLogPath)
    $runLogDir = Split-Path -Parent $runLogPath
    if (-not (Test-Path $runLogDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $runLogDir -Force
    }
    if (Test-Path $runLogPath -PathType Leaf) {
        Remove-Item -Path $runLogPath -Force
    }

    $runReportPath = if ([string]::IsNullOrWhiteSpace($ReportFile)) {
        [System.IO.Path]::ChangeExtension($runLogPath, ".report.json")
    } elseif ([System.IO.Path]::IsPathRooted($ReportFile)) {
        $ReportFile
    } else {
        Join-Path $repoRoot $ReportFile
    }
    $runReportPath = [System.IO.Path]::GetFullPath($runReportPath)
    $runReportDir = Split-Path -Parent $runReportPath
    if (-not (Test-Path $runReportDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $runReportDir -Force
    }
    if (Test-Path $runReportPath -PathType Leaf) {
        Remove-Item -Path $runReportPath -Force
    }

    $runHarnessReportPath = if ([string]::IsNullOrWhiteSpace($HarnessReportFile)) {
        [System.IO.Path]::ChangeExtension($runLogPath, ".harness.json")
    } elseif ([System.IO.Path]::IsPathRooted($HarnessReportFile)) {
        $HarnessReportFile
    } else {
        Join-Path $repoRoot $HarnessReportFile
    }
    $runHarnessReportPath = [System.IO.Path]::GetFullPath($runHarnessReportPath)
    $runHarnessReportDir = Split-Path -Parent $runHarnessReportPath
    if (-not (Test-Path $runHarnessReportDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $runHarnessReportDir -Force
    }
    if (Test-Path $runHarnessReportPath -PathType Leaf) {
        Remove-Item -Path $runHarnessReportPath -Force
    }

    # ── State initialization ──
    $failureReasons = [System.Collections.Generic.List[string]]::new()
    $inconclusiveReasons = [System.Collections.Generic.List[string]]::new()
    $warnings = [System.Collections.Generic.List[string]]::new()
    $manualObservationRequested = $ManualObservationWindowSeconds -gt 0 `
        -or -not [string]::IsNullOrWhiteSpace($ManualObservationNote) `
        -or -not [string]::IsNullOrWhiteSpace($ManualEndpointResult) `
        -or -not [string]::IsNullOrWhiteSpace($ManualEndpointTiming)
    $manualObservationWindowMs = if ($ManualObservationWindowSeconds -gt 0) {
        $ManualObservationWindowSeconds * 1000
    } else {
        0
    }

    # ── Timing calculations ──
    $finishedSourceDurationMs = -1L
    $requireFinishedMinimumQuitAfterMs = -1L
    if ($RequireFinished) {
        $finishedSourceDurationMs = Get-MediaDurationMs `
            -SourcePath $sourcePath `
            -FfprobePath $effectiveFfprobePath
        if ($finishedSourceDurationMs -le 0) {
            throw "RequireFinished needs ffprobe duration before launch. Build the runtime-with-ffprobe audio core or provide ffprobe via -FfprobePathOverride/AUDIOPLAYER_FFPROBE_PATH."
        }

        $requireFinishedMinimumQuitAfterMs = $finishedSourceDurationMs + $RequireFinishedBufferMs
    }
    $effectiveQuitAfterMs = [long]$QuitAfterMs
    if ($manualObservationWindowMs -gt 0) {
        $effectiveQuitAfterMs = [Math]::Max($effectiveQuitAfterMs, [long]$manualObservationWindowMs)
    }
    if ($requireFinishedMinimumQuitAfterMs -gt 0) {
        $effectiveQuitAfterMs = [Math]::Max($effectiveQuitAfterMs, $requireFinishedMinimumQuitAfterMs)
    }
    if ($effectiveQuitAfterMs -gt [int]::MaxValue) {
        throw "Effective QuitAfterMs is too large for the app CLI: $effectiveQuitAfterMs"
    }
    if ($RequireFinished -and $effectiveQuitAfterMs -gt $QuitAfterMs) {
        Add-UniqueString -List $warnings -Value "require-finished-extended-quit-after-ms:$QuitAfterMs->$effectiveQuitAfterMs"
    }
    $effectiveQuitAfterMs = [int]$effectiveQuitAfterMs
    $requireFinishedTiming = if ($RequireFinished) {
        [ordered]@{
            sourceDurationMs = $finishedSourceDurationMs
            bufferMs = $RequireFinishedBufferMs
            minimumQuitAfterMs = $requireFinishedMinimumQuitAfterMs
        }
    } else {
        $null
    }
    $manualObservation = [ordered]@{
        requested = $manualObservationRequested
        windowSeconds = $ManualObservationWindowSeconds
        note = $ManualObservationNote
        result = $ManualEndpointResult
        timing = $ManualEndpointTiming
        effectiveQuitAfterMs = $effectiveQuitAfterMs
        endpointOutputVerified = $false
        observationLayer = "manual-listening-note-if-provided"
    }
    $requestedActions = [ordered]@{
        source = $sourcePath
        quitAfterMs = $QuitAfterMs
        effectiveQuitAfterMs = $effectiveQuitAfterMs
        requireFinishedTiming = $requireFinishedTiming
        manualObservationRequested = $manualObservationRequested
        manualObservationWindowSeconds = $ManualObservationWindowSeconds
        manualObservationNote = $ManualObservationNote
        manualEndpointResult = $ManualEndpointResult
        manualEndpointTiming = $ManualEndpointTiming
        listOutputDevices = [bool]$ListOutputDevices
        outputDeviceIndex = if ($OutputDeviceIndex -ne [int]::MinValue) { $OutputDeviceIndex } else { $null }
        asioOutputSelectionRequested = ($AsioOutputIndex -ne [int]::MinValue)
        asioOutputIndex = if ($AsioOutputIndex -ne [int]::MinValue) { $AsioOutputIndex } else { $null }
        outputSwitchRequested = ($SwitchOutputAfterMs -ge 0)
        refreshOutputRequested = ($RefreshOutputAfterMs -ge 0)
        sourceSwitchRequested = (-not [string]::IsNullOrWhiteSpace($switchSourcePath))
        seekRequested = ($SeekAfterMs -ge 0 -and $SeekToMs -ge 0) -or (-not [string]::IsNullOrWhiteSpace($SeekSequence))
        secondSeekRequested = ($SecondSeekAfterMs -ge 0 -and $SecondSeekToMs -ge 0)
        seekPauseResume = [bool]$SeekPauseResume
        seekSequence = $SeekSequence
        pauseRequested = ($PauseAfterMs -ge 0)
        resumeRequested = ($ResumeAfterMs -ge 0)
        stopRequested = ($StopAfterMs -ge 0)
        requirePlaying = [bool]$RequirePlaying
        requirePauseResume = [bool]$RequirePauseResume
        requireStop = [bool]$RequireStop
        requireStopFadeOut = [bool]$RequireStopFadeOut
        requireFinished = [bool]$RequireFinished
        requireFinishedBufferMs = $RequireFinishedBufferMs
        rejectPlaybackErrors = [bool]$RejectPlaybackErrors
        requireErrorPattern = $RequireErrorPattern
        requireAudibleLevels = [bool]$RequireAudibleLevels
        requireSourceSwitchClean = [bool]$RequireSourceSwitchClean
    }

    # ── Process creation ──
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $fallbackArguments = [System.Collections.Generic.List[string]]::new()
    $psi.FileName = $exePath
    $psi.WorkingDirectory = Split-Path -Parent $exePath
    $psi.UseShellExecute = $false
    $psi.Environment["AUDIOPLAYER_LOG_FILE"] = $runLogPath
    if ($BoundParameters.ContainsKey("FfmpegPathOverride")) {
        $psi.Environment["AUDIOPLAYER_FFMPEG_PATH"] = $FfmpegPathOverride
    } elseif (-not [string]::IsNullOrWhiteSpace($defaultFfmpegPath)) {
        $psi.Environment["AUDIOPLAYER_FFMPEG_PATH"] = $defaultFfmpegPath
    }
    if ($BoundParameters.ContainsKey("FfprobePathOverride")) {
        $psi.Environment["AUDIOPLAYER_FFPROBE_PATH"] = if ([string]::IsNullOrWhiteSpace($effectiveFfprobePath)) {
            $FfprobePathOverride
        } else {
            $effectiveFfprobePath
        }
    } elseif (-not [string]::IsNullOrWhiteSpace($defaultFfprobePath)) {
        $psi.Environment["AUDIOPLAYER_FFPROBE_PATH"] = $defaultFfprobePath
    }
    if (-not [string]::IsNullOrWhiteSpace($RefreshOutputTrigger)) {
        $psi.Environment["AUDIOPLAYER_SCRIPTED_SMOKE_TEST"] = "1"
        $psi.Environment["AUDIOPLAYER_SCRIPTED_OUTPUT_SWITCH_TRIGGER"] = $RefreshOutputTrigger
    }
    if (-not [string]::IsNullOrWhiteSpace($HotReconfigureCompatibilityOverride)) {
        $psi.Environment["AUDIOPLAYER_SCRIPTED_SMOKE_TEST"] = "1"
        $psi.Environment["AUDIOPLAYER_SCRIPTED_HOT_RECONFIGURE_COMPATIBILITY"] = $HotReconfigureCompatibilityOverride
    }
    Add-ProcessArgument -StartInfo $psi -Value "--quit-after-ms" -FallbackArguments $fallbackArguments
    Add-ProcessArgument -StartInfo $psi -Value $effectiveQuitAfterMs.ToString() -FallbackArguments $fallbackArguments
    Add-ProcessArgument -StartInfo $psi -Value "--source" -FallbackArguments $fallbackArguments
    Add-ProcessArgument -StartInfo $psi -Value $sourcePath -FallbackArguments $fallbackArguments
    Add-ProcessArgument -StartInfo $psi -Value "--report-file" -FallbackArguments $fallbackArguments
    Add-ProcessArgument -StartInfo $psi -Value $runReportPath -FallbackArguments $fallbackArguments

    if ($ListOutputDevices) {
        Add-ProcessArgument -StartInfo $psi -Value "--list-output-devices" -FallbackArguments $fallbackArguments
    }

    if ($OutputDeviceIndex -ne [int]::MinValue) {
        Add-ProcessArgument -StartInfo $psi -Value "--output-device-index" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $OutputDeviceIndex.ToString() -FallbackArguments $fallbackArguments
    }

    if ($AsioOutputIndex -ne [int]::MinValue) {
        Add-ProcessArgument -StartInfo $psi -Value "--asio-output-index" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $AsioOutputIndex.ToString() -FallbackArguments $fallbackArguments
    }

    if ($ExclusiveMode) {
        Add-ProcessArgument -StartInfo $psi -Value "--exclusive-mode" -FallbackArguments $fallbackArguments
    }

    if ($SeekAfterMs -ge 0 -and $SeekToMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--seek-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SeekAfterMs.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--seek-to-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SeekToMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($SecondSeekAfterMs -ge 0 -and $SecondSeekToMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--second-seek-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SecondSeekAfterMs.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--second-seek-to-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SecondSeekToMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($SeekPauseResume) {
        Add-ProcessArgument -StartInfo $psi -Value "--seek-pause-resume" -FallbackArguments $fallbackArguments
    }

    if (-not [string]::IsNullOrWhiteSpace($SeekSequence)) {
        Add-ProcessArgument -StartInfo $psi -Value "--seek-sequence" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SeekSequence -FallbackArguments $fallbackArguments
    }

    if ($PauseAfterMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--pause-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $PauseAfterMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($ResumeAfterMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--resume-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $ResumeAfterMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($StopAfterMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--stop-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $StopAfterMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($RefreshOutputAfterMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--refresh-output-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $RefreshOutputAfterMs.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--refresh-output-count" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $RefreshOutputCount.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--refresh-output-interval-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $RefreshOutputIntervalMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($SwitchOutputAfterMs -ge 0) {
        Add-ProcessArgument -StartInfo $psi -Value "--switch-output-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SwitchOutputAfterMs.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--switch-output-to-index" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SwitchOutputToIndex.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--repeat-output-switch" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $RepeatOutputSwitch.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--switch-interval-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SwitchIntervalMs.ToString() -FallbackArguments $fallbackArguments
    }

    if ($SwitchSourceAfterMs -ge 0 -and -not [string]::IsNullOrWhiteSpace($switchSourcePath)) {
        Add-ProcessArgument -StartInfo $psi -Value "--switch-source-after-ms" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $SwitchSourceAfterMs.ToString() -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value "--switch-source" -FallbackArguments $fallbackArguments
        Add-ProcessArgument -StartInfo $psi -Value $switchSourcePath -FallbackArguments $fallbackArguments
    }

    if ($null -eq $psi.ArgumentList) {
        $psi.Arguments = [string]::Join(' ', $fallbackArguments)
    }

    # ── Process launch ──
    $processCleanup = [ordered]@{
        appProcessId = $null
        timedOut = $false
        maxWaitMs = $null
        descendantsBeforeCleanup = @()
        killedProcessIds = @()
        cleanupErrors = @()
        residualProcessIds = @()
    }

    $process = [System.Diagnostics.Process]::Start($psi)
    if ($null -eq $process) {
        Add-UniqueString -List $failureReasons -Value "failed-to-start-audioplayer"
        Write-EarlyFailureReport -Context ([ordered]@{
            runHarnessReportPath = $runHarnessReportPath
            runLogPath = $runLogPath
            runReportPath = $runReportPath
            failureReasons = $failureReasons
            inconclusiveReasons = $inconclusiveReasons
            warnings = $warnings
            requestedActions = $requestedActions
            manualObservation = $manualObservation
            exitCode = -1
        }) `
            -Result "FAIL" `
            -ObservedActions ([ordered]@{}) `
            -Assertions ([ordered]@{ passed = $false; failure = "Failed to start AudioPlayer" })
        throw "Failed to start AudioPlayer"
    }

    $processCleanup["appProcessId"] = $process.Id
    $maxWaitMs = $effectiveQuitAfterMs + $GracePeriodMs
    $processCleanup["maxWaitMs"] = $maxWaitMs
    if (-not $process.WaitForExit($maxWaitMs)) {
        $processCleanup["timedOut"] = $true
        $treeCleanup = Stop-TrackedProcessTree -RootProcessId $process.Id -IncludeRoot
        $processCleanup["descendantsBeforeCleanup"] = @($treeCleanup.descendantsBeforeCleanup)
        $processCleanup["killedProcessIds"] = @($treeCleanup.killedProcessIds)
        $processCleanup["cleanupErrors"] = @($treeCleanup.cleanupErrors)
        $processCleanup["residualProcessIds"] = @($treeCleanup.residualProcessIds)
        Add-UniqueString -List $failureReasons -Value "process-timeout"
        if ($processCleanup["residualProcessIds"].Count -gt 0) {
            Add-UniqueString -List $failureReasons -Value "process-cleanup-residual:$([string]::Join(',', $processCleanup["residualProcessIds"]))"
        }
        Write-EarlyFailureReport -Context ([ordered]@{
            runHarnessReportPath = $runHarnessReportPath
            runLogPath = $runLogPath
            runReportPath = $runReportPath
            failureReasons = $failureReasons
            inconclusiveReasons = $inconclusiveReasons
            warnings = $warnings
            requestedActions = $requestedActions
            manualObservation = $manualObservation
            exitCode = -1
        }) `
            -Result "FAIL" `
            -ObservedActions ([ordered]@{ timedOut = $true; maxWaitMs = $maxWaitMs; processCleanup = $processCleanup }) `
            -Assertions ([ordered]@{ passed = $false; failure = "AudioPlayer did not exit within $maxWaitMs ms" })
        throw "AudioPlayer did not exit within $maxWaitMs ms"
    }

    $treeCleanup = Stop-TrackedProcessTree -RootProcessId $process.Id
    $processCleanup["descendantsBeforeCleanup"] = @($treeCleanup.descendantsBeforeCleanup)
    $processCleanup["killedProcessIds"] = @($treeCleanup.killedProcessIds)
    $processCleanup["cleanupErrors"] = @($treeCleanup.cleanupErrors)
    $processCleanup["residualProcessIds"] = @($treeCleanup.residualProcessIds)
    if ($processCleanup["killedProcessIds"].Count -gt 0) {
        Add-UniqueString -List $warnings -Value "process-tree-cleanup:killed-descendants:$([string]::Join(',', $processCleanup["killedProcessIds"]))"
    }
    if ($processCleanup["residualProcessIds"].Count -gt 0) {
        Add-UniqueString -List $failureReasons -Value "process-tree-cleanup-residual:$([string]::Join(',', $processCleanup["residualProcessIds"]))"
    }

    # ── Early file-existence checks ──
    if (-not (Test-Path $runLogPath -PathType Leaf)) {
        Add-UniqueString -List $failureReasons -Value "missing-text-log"
        Write-EarlyFailureReport -Context ([ordered]@{
            runHarnessReportPath = $runHarnessReportPath
            runLogPath = $runLogPath
            runReportPath = $runReportPath
            failureReasons = $failureReasons
            inconclusiveReasons = $inconclusiveReasons
            warnings = $warnings
            requestedActions = $requestedActions
            manualObservation = $manualObservation
            exitCode = $process.ExitCode
        }) `
            -Result "FAIL" `
            -ObservedActions ([ordered]@{ exitCode = $process.ExitCode; processCleanup = $processCleanup }) `
            -Assertions ([ordered]@{ passed = $false; failure = "Expected player log file was not created" })
        throw "Expected player log file was not created: $runLogPath"
    }

    if (-not (Test-Path $runReportPath -PathType Leaf)) {
        Add-UniqueString -List $failureReasons -Value "missing-app-report"
        Write-EarlyFailureReport -Context ([ordered]@{
            runHarnessReportPath = $runHarnessReportPath
            runLogPath = $runLogPath
            runReportPath = $runReportPath
            failureReasons = $failureReasons
            inconclusiveReasons = $inconclusiveReasons
            warnings = $warnings
            requestedActions = $requestedActions
            manualObservation = $manualObservation
            exitCode = $process.ExitCode
        }) `
            -Result "FAIL" `
            -ObservedActions ([ordered]@{ exitCode = $process.ExitCode; processCleanup = $processCleanup }) `
            -Assertions ([ordered]@{ passed = $false; failure = "Expected diagnostic report file was not created" })
        throw "Expected diagnostic report file was not created: $runReportPath"
    }

    return [ordered]@{
        sourcePath = $sourcePath
        exePath = $exePath
        effectiveFfprobePath = $effectiveFfprobePath
        runLogPath = $runLogPath
        runReportPath = $runReportPath
        runHarnessReportPath = $runHarnessReportPath
        failureReasons = $failureReasons
        inconclusiveReasons = $inconclusiveReasons
        warnings = $warnings
        manualObservation = $manualObservation
        requestedActions = $requestedActions
        effectiveQuitAfterMs = $effectiveQuitAfterMs
        processCleanup = $processCleanup
        process = $process
    }
}
