param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [int]$QuitAfterMs = 8000,
    [Alias("HoldSeconds")]
    [ValidateRange(0, 86400)]
    [int]$ManualObservationWindowSeconds = 0,
    [AllowEmptyString()]
    [ValidateSet("", "PopHeard", "NoPop", "Unclear")]
    [string]$ManualEndpointResult = "",
    [AllowEmptyString()]
    [ValidateSet("", "SwitchInstant", "Recovery", "PlaybackStart", "Unknown")]
    [string]$ManualEndpointTiming = "",
    [string]$ManualObservationNote = "",
    [int]$GracePeriodMs = 5000,
    [int]$CapturePaddingMs = 2000,
    [string]$LogPath = "",
    [string]$ReportFile = "",
    [string]$HarnessReportFile = "",
    [string]$LoopbackWavFile = "",
    [string]$LoopbackReportFile = "",
    [string]$SummaryReportFile = "",
    [ValidateRange(1, 100000)]
    [int]$KeepRuns = 20,
    [switch]$NoCleanup,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AdditionalSmokeArguments
)

$ErrorActionPreference = "Stop"

function ConvertTo-ProcessArgumentToken {
    param(
        [AllowEmptyString()]
        [string]$Value
    )

    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }

    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    $builder = [System.Text.StringBuilder]::new()
    $null = $builder.Append('"')
    $backslashCount = 0

    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $backslashCount += 1
            continue
        }

        if ($character -eq '"') {
            if ($backslashCount -gt 0) {
                $null = $builder.Append(('\' * ($backslashCount * 2 + 1)))
                $backslashCount = 0
            } else {
                $null = $builder.Append('\')
            }
            $null = $builder.Append('"')
            continue
        }

        if ($backslashCount -gt 0) {
            $null = $builder.Append(('\' * $backslashCount))
            $backslashCount = 0
        }

        $null = $builder.Append($character)
    }

    if ($backslashCount -gt 0) {
        $null = $builder.Append(('\' * ($backslashCount * 2)))
    }

    $null = $builder.Append('"')
    return $builder.ToString()
}

function Add-ProcessArgument {
    param(
        [System.Diagnostics.ProcessStartInfo]$StartInfo,
        [string]$Value,
        [System.Collections.Generic.List[string]]$FallbackArguments
    )

    if ($null -ne $StartInfo.ArgumentList) {
        $null = $StartInfo.ArgumentList.Add($Value)
        return
    }

    $FallbackArguments.Add((ConvertTo-ProcessArgumentToken -Value $Value)) | Out-Null
}

function Add-SmokePassThroughArguments {
    param(
        [hashtable]$Parameters,
        [string[]]$Arguments
    )

    for ($index = 0; $index -lt $Arguments.Count; ++$index) {
        $argument = $Arguments[$index]
        if ($argument -notmatch '^-(?<name>[A-Za-z][A-Za-z0-9]*)(?::(?<bool>true|false))?$') {
            throw "Additional smoke argument must be a named PowerShell parameter: $argument"
        }

        $name = $Matches.name
        if ($Parameters.ContainsKey($name)) {
            throw "Additional smoke argument duplicates wrapper-owned parameter: -$name"
        }

        if (-not [string]::IsNullOrWhiteSpace($Matches.bool)) {
            $Parameters[$name] = $Matches.bool -ieq "true"
            continue
        }

        $hasValue = $index + 1 -lt $Arguments.Count `
            -and $Arguments[$index + 1] -notmatch '^-[A-Za-z]'
        if ($hasValue) {
            $Parameters[$name] = $Arguments[++$index]
        } else {
            $Parameters[$name] = $true
        }
    }
}

function Resolve-RepoPath {
    param(
        [string]$Path,
        [string]$DefaultPath
    )

    $candidate = if ([string]::IsNullOrWhiteSpace($Path)) {
        $DefaultPath
    } else {
        $Path
    }

    if ([System.IO.Path]::IsPathRooted($candidate)) {
        return [System.IO.Path]::GetFullPath($candidate)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $candidate))
}

function Add-UniqueString {
    param(
        [System.Collections.Generic.List[string]]$List,
        [AllowEmptyString()]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return
    }
    if (-not $List.Contains($Value)) {
        $List.Add($Value) | Out-Null
    }
}

function ConvertTo-StringArray {
    param(
        [object]$Value
    )

    $items = @()
    if ($null -ne $Value) {
        foreach ($item in @($Value)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$item)) {
                $items += [string]$item
            }
        }
    }
    return @($items)
}

function Get-ReportProperty {
    param(
        [object[]]$Sources,
        [string]$Name,
        [object]$Default = $null
    )

    foreach ($source in @($Sources)) {
        if ($null -eq $source) {
            continue
        }
        $property = $source.PSObject.Properties[$Name]
        if ($null -ne $property) {
            return $property.Value
        }
    }
    return $Default
}

function ConvertTo-SummaryBool {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return $false
    }
    if ($Value -is [bool]) {
        return $Value
    }
    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $false
    }
    if ($text -ieq "true" -or $text -eq "1") {
        return $true
    }
    return $false
}

function Write-WrapperSummaryReport {
    param(
        [string]$Path,
        [string]$Result,
        [object]$SmokeHarness,
        [object]$AppReport,
        [object]$LoopbackReport,
        [string]$SmokeHarnessReportPath,
        [string]$AppReportPath,
        [string]$LoopbackAggregateReportPath,
        [string[]]$LoopbackSegmentReportPaths,
        [string[]]$LoopbackWavFilePaths,
        [object]$ManualObservation,
        [string[]]$WrapperErrors,
        [AllowEmptyString()]
        [string]$SmokeError,
        [AllowEmptyString()]
        [string]$CaptureCleanupWarning
    )

    $observedActions = if ($null -ne $SmokeHarness) { $SmokeHarness.observedActions } else { $null }
    $endpointInvalidationHresults = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $LoopbackReport) {
        foreach ($hresult in ConvertTo-StringArray -Value $LoopbackReport.interruptionHresults) {
            Add-UniqueString -List $endpointInvalidationHresults -Value $hresult
        }
        Add-UniqueString -List $endpointInvalidationHresults -Value ([string]$LoopbackReport.interruptionHresult)
        foreach ($segment in @($LoopbackReport.segments)) {
            if ($null -ne $segment) {
                Add-UniqueString -List $endpointInvalidationHresults -Value ([string]$segment.interruptionHresult)
            }
        }
    }

    $loopbackInterrupted = $false
    if ($null -ne $LoopbackReport) {
        $loopbackInterrupted =
            (ConvertTo-SummaryBool -Value $LoopbackReport.captureInterrupted) `
            -or -not [string]::IsNullOrWhiteSpace([string]$LoopbackReport.interruptionReason) `
            -or $endpointInvalidationHresults.Count -gt 0
    }

    $appReportResultSources = @($AppReport)
    if ($null -ne $SmokeHarness) {
        $appReportResultSources += [pscustomobject]@{ result = $SmokeHarness.appReportResult }
    }

    $summary = [ordered]@{
        schemaVersion = 1
        result = $Result
        evidenceLayer = "loopback-manual-smoke-wrapper-summary"
        verificationLayer = "evidence-aggregation-only"
        endpointOutputVerified = $false
        smokeHarnessReport = $SmokeHarnessReportPath
        appReport = $AppReportPath
        loopbackAggregateReport = $LoopbackAggregateReportPath
        loopbackSegmentReports = @($LoopbackSegmentReportPaths)
        loopbackWavFiles = @($LoopbackWavFilePaths)
        manualObservation = $ManualObservation
        smokeHarnessResult = Get-ReportProperty -Sources @($SmokeHarness) -Name "result" -Default ""
        appReportResult = Get-ReportProperty -Sources $appReportResultSources -Name "result" -Default ""
        loopbackResult = Get-ReportProperty -Sources @($LoopbackReport) -Name "result" -Default ""
        loopbackCaptureInterrupted = $loopbackInterrupted
        loopbackInterruptionReason = Get-ReportProperty -Sources @($LoopbackReport) -Name "interruptionReason" -Default ""
        loopbackInterruptionHresult = Get-ReportProperty -Sources @($LoopbackReport) -Name "interruptionHresult" -Default ""
        endpointInvalidationHresults = @($endpointInvalidationHresults.ToArray())
        loopbackSegmentCount = Get-ReportProperty -Sources @($LoopbackReport) -Name "segmentCount" -Default 0
        loopbackTransientCandidateCount = Get-ReportProperty -Sources @($LoopbackReport) -Name "transientCandidateCount" -Default $null
        loopbackDropoutCandidateCount = Get-ReportProperty -Sources @($LoopbackReport) -Name "dropoutCandidateCount" -Default $null
        loopbackTailFadeCandidateObserved = Get-ReportProperty -Sources @($LoopbackReport) -Name "tailFadeCandidateObserved" -Default $null
        loopbackTrailingSilenceDurationMs = Get-ReportProperty -Sources @($LoopbackReport) -Name "trailingSilenceDurationMs" -Default $null
        activeOutputSwitchDetected = Get-ReportProperty -Sources @($observedActions, $AppReport) -Name "activeOutputSwitchDetected" -Default $null
        activeOutputSwitchStartedCount = Get-ReportProperty -Sources @($observedActions, $AppReport) -Name "activeOutputSwitchStartedCount" -Default $null
        activeOutputSwitchCompletedCount = Get-ReportProperty -Sources @($observedActions, $AppReport) -Name "activeOutputSwitchCompletedCount" -Default $null
        sameOutputInvalidationCount = Get-ReportProperty -Sources @($observedActions, $AppReport) -Name "sameOutputInvalidationCount" -Default $null
        interpretation = [ordered]@{
            canClaimNoPop = $false
            limitation = "Summary aggregates existing evidence only. Manual NoPop remains an observation, and zero loopback transient/dropout candidates are not proof of pop-free endpoint output. Tail fade is a candidate signal for tone fixtures."
        }
        wrapperErrors = @($WrapperErrors)
        smokeError = $SmokeError
        captureCleanupWarning = $CaptureCleanupWarning
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }

    $reportDir = Split-Path -Parent $Path
    if (-not (Test-Path $reportDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $reportDir -Force
    }
    $summary | ConvertTo-Json -Depth 12 | Set-Content -Path $Path -Encoding UTF8
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
. (Join-Path $PSScriptRoot "cleanup-test-artifacts.ps1")
$captureExe = Join-Path $repoRoot "$BuildDir\$Configuration\WasapiLoopbackCapture.exe"
if (-not (Test-Path $captureExe -PathType Leaf)) {
    throw "Loopback capture tool not found: $captureExe. Build target WasapiLoopbackCapture first."
}

$smokeScript = Join-Path $PSScriptRoot "run-playback-smoke.ps1"
if (-not (Test-Path $smokeScript -PathType Leaf)) {
    throw "Smoke script not found: $smokeScript"
}

$runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), ([guid]::NewGuid().ToString("N").Substring(0, 8))
$logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
$loopbackDir = Resolve-AudioPlayerCacheSubdir -RepoRoot $repoRoot -BuildDir $BuildDir -Name "loopback"
if (-not (Test-Path $logDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $logDir -Force
}
if (-not (Test-Path $loopbackDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $loopbackDir -Force
}

$defaultLogPath = Join-Path $logDir "player-loopback-smoke-$runId.log"
$defaultReportPath = [System.IO.Path]::ChangeExtension($defaultLogPath, ".report.json")
$defaultHarnessPath = [System.IO.Path]::ChangeExtension($defaultLogPath, ".harness.json")
$defaultSummaryPath = [System.IO.Path]::ChangeExtension($defaultLogPath, ".summary.json")
$defaultWavPath = Join-Path $loopbackDir "loopback-$runId.wav"
$defaultLoopbackReportPath = Join-Path $loopbackDir "loopback-$runId.report.json"
$readyFile = Join-Path $loopbackDir "loopback-$runId.ready.json"
$stopFile = Join-Path $loopbackDir "loopback-$runId.stop"

$runLogPath = Resolve-RepoPath -Path $LogPath -DefaultPath $defaultLogPath
$runReportPath = Resolve-RepoPath -Path $ReportFile -DefaultPath $defaultReportPath
$runHarnessReportPath = Resolve-RepoPath -Path $HarnessReportFile -DefaultPath $defaultHarnessPath
$summaryReportPath = Resolve-RepoPath -Path $SummaryReportFile -DefaultPath $defaultSummaryPath
$loopbackWavPath = Resolve-RepoPath -Path $LoopbackWavFile -DefaultPath $defaultWavPath
$loopbackReportPath = Resolve-RepoPath -Path $LoopbackReportFile -DefaultPath $defaultLoopbackReportPath

$manualObservationWindowMs = if ($ManualObservationWindowSeconds -gt 0) {
    $ManualObservationWindowSeconds * 1000
} else {
    0
}
$effectiveQuitAfterMs = if ($manualObservationWindowMs -gt 0) {
    [Math]::Max($QuitAfterMs, $manualObservationWindowMs)
} else {
    $QuitAfterMs
}
$requestedCaptureDurationMs = $effectiveQuitAfterMs + $GracePeriodMs + $CapturePaddingMs
$captureDurationCeilingMs = $effectiveQuitAfterMs + 15000
$captureDurationMs = [Math]::Min($requestedCaptureDurationMs, $captureDurationCeilingMs)

$capturePsi = [System.Diagnostics.ProcessStartInfo]::new()
$capturePsi.FileName = $captureExe
$capturePsi.UseShellExecute = $false
$capturePsi.CreateNoWindow = $true
$capturePsi.RedirectStandardOutput = $true
$capturePsi.RedirectStandardError = $true
$fallbackArguments = [System.Collections.Generic.List[string]]::new()
foreach ($argument in @(
        "--duration-ms", $captureDurationMs.ToString(),
        "--wav", $loopbackWavPath,
        "--report", $loopbackReportPath,
        "--ready-file", $readyFile,
        "--stop-file", $stopFile
    )) {
    Add-ProcessArgument -StartInfo $capturePsi -Value $argument -FallbackArguments $fallbackArguments
}
if ($null -eq $capturePsi.ArgumentList) {
    $capturePsi.Arguments = [string]::Join(' ', $fallbackArguments)
}

if (Test-Path $readyFile -PathType Leaf) {
    Remove-Item -Path $readyFile -Force
}
if (Test-Path $stopFile -PathType Leaf) {
    Remove-Item -Path $stopFile -Force
}

$captureProcess = [System.Diagnostics.Process]::Start($capturePsi)
if ($null -eq $captureProcess) {
    throw "Failed to start loopback capture tool"
}

$smokeError = $null
$captureCleanupStopRequested = $false
$captureCleanupExited = $false
$captureCleanupForcedKill = $false
$captureCleanupWarning = ""
try {
    $readyWait = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path $readyFile -PathType Leaf)) {
        if ($captureProcess.HasExited) {
            $stdout = $captureProcess.StandardOutput.ReadToEnd()
            $stderr = $captureProcess.StandardError.ReadToEnd()
            throw "Loopback capture exited before becoming ready. stdout=$stdout stderr=$stderr"
        }
        if ($readyWait.ElapsedMilliseconds -gt 5000) {
            throw "Loopback capture did not become ready within 5000 ms"
        }
        Start-Sleep -Milliseconds 50
    }

    $smokeParams = @{
        Source = $Source
        BuildDir = $BuildDir
        Configuration = $Configuration
        QuitAfterMs = $QuitAfterMs
        GracePeriodMs = $GracePeriodMs
        LogPath = $runLogPath
        ReportFile = $runReportPath
        HarnessReportFile = $runHarnessReportPath
        NoCleanup = $true
    }
    if ($ManualObservationWindowSeconds -gt 0) {
        $smokeParams.ManualObservationWindowSeconds = $ManualObservationWindowSeconds
    }
    if (-not [string]::IsNullOrWhiteSpace($ManualEndpointResult)) {
        $smokeParams.ManualEndpointResult = $ManualEndpointResult
    }
    if (-not [string]::IsNullOrWhiteSpace($ManualEndpointTiming)) {
        $smokeParams.ManualEndpointTiming = $ManualEndpointTiming
    }
    if (-not [string]::IsNullOrWhiteSpace($ManualObservationNote)) {
        $smokeParams.ManualObservationNote = $ManualObservationNote
    }
    Add-SmokePassThroughArguments -Parameters $smokeParams -Arguments $AdditionalSmokeArguments
    & $smokeScript @smokeParams
} catch {
    $smokeError = $_
} finally {
    if ($null -ne $captureProcess -and -not $captureProcess.HasExited) {
        try {
            Set-Content -Path $stopFile -Value "stop" -Encoding ASCII
            $captureCleanupStopRequested = $true
        } catch {
            $captureCleanupWarning = "Failed to create loopback stop file: $($_.Exception.Message)"
            Write-Warning $captureCleanupWarning
        }
        if ($captureProcess.WaitForExit(5000)) {
            $captureCleanupExited = $true
        } else {
            $captureCleanupForcedKill = $true
            $captureCleanupWarning = "Loopback capture did not exit within 5000 ms after stop request; forcing process termination."
            Write-Warning $captureCleanupWarning
            try {
                Stop-Process -Id $captureProcess.Id -Force -ErrorAction Stop
            } catch {
                Write-Warning "Failed to force-stop loopback capture process $($captureProcess.Id): $($_.Exception.Message)"
            }
            try {
                $captureProcess.WaitForExit(2000) | Out-Null
            } catch {
            }
        }
    } elseif ($null -ne $captureProcess) {
        $captureCleanupExited = $true
    }
}

$captureStdout = $captureProcess.StandardOutput.ReadToEnd()
$captureStderr = $captureProcess.StandardError.ReadToEnd()
$wrapperErrors = [System.Collections.Generic.List[string]]::new()
$loopbackReport = $null
$loopbackReportExists = Test-Path $loopbackReportPath -PathType Leaf
if ($loopbackReportExists) {
    try {
        $loopbackReport = Get-Content -Path $loopbackReportPath -Encoding UTF8 -Raw | ConvertFrom-Json
    } catch {
        Write-Warning "Loopback report exists but could not be parsed: $loopbackReportPath"
    }
}
$loopbackInconclusiveReport =
    $null -ne $loopbackReport -and [string]$loopbackReport.result -eq "INCONCLUSIVE"
$loopbackCaptureInterrupted = $false
if ($null -ne $loopbackReport) {
    $loopbackCaptureInterrupted =
        (ConvertTo-SummaryBool -Value $loopbackReport.captureInterrupted) `
        -or -not [string]::IsNullOrWhiteSpace([string]$loopbackReport.interruptionReason) `
        -or -not [string]::IsNullOrWhiteSpace([string]$loopbackReport.interruptionHresult)
}
$loopbackSegmentWavFiles = @()
$loopbackSegmentMetadataFiles = @()
if ($null -ne $loopbackReport) {
    if ($null -ne $loopbackReport.wavFiles) {
        foreach ($wavFile in @($loopbackReport.wavFiles)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$wavFile)) {
                $loopbackSegmentWavFiles += [string]$wavFile
            }
        }
    }
    if ($null -ne $loopbackReport.segmentMetadataFiles) {
        foreach ($metadataFile in @($loopbackReport.segmentMetadataFiles)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$metadataFile)) {
                $loopbackSegmentMetadataFiles += [string]$metadataFile
            }
        }
    }
    if ($loopbackSegmentWavFiles.Count -eq 0 -and $null -ne $loopbackReport.segments) {
        foreach ($segment in @($loopbackReport.segments)) {
            if ($null -ne $segment.wavFile -and -not [string]::IsNullOrWhiteSpace([string]$segment.wavFile)) {
                $loopbackSegmentWavFiles += [string]$segment.wavFile
            }
        }
    }
    if ($loopbackSegmentMetadataFiles.Count -eq 0 -and $null -ne $loopbackReport.segments) {
        foreach ($segment in @($loopbackReport.segments)) {
            if ($null -ne $segment.metadataFile -and -not [string]::IsNullOrWhiteSpace([string]$segment.metadataFile)) {
                $loopbackSegmentMetadataFiles += [string]$segment.metadataFile
            }
        }
    }
    foreach ($segment in @($loopbackReport.segments)) {
        if ($null -ne $segment) {
            $loopbackCaptureInterrupted =
                $loopbackCaptureInterrupted `
                -or (ConvertTo-SummaryBool -Value $segment.interrupted) `
                -or -not [string]::IsNullOrWhiteSpace([string]$segment.interruptionHresult)
        }
    }
}
if ($loopbackSegmentWavFiles.Count -eq 0 -and -not [string]::IsNullOrWhiteSpace($loopbackWavPath)) {
    $loopbackSegmentWavFiles += $loopbackWavPath
}

if ($captureProcess.ExitCode -ne 0) {
    if ($loopbackInconclusiveReport) {
        Write-Warning "Loopback capture exited with code $($captureProcess.ExitCode), but an INCONCLUSIVE sidecar report was written and will be preserved."
    } else {
        $wrapperErrors.Add("Loopback capture failed with exit code $($captureProcess.ExitCode). stdout=$captureStdout stderr=$captureStderr") | Out-Null
    }
}
if (-not (Test-Path $loopbackWavPath -PathType Leaf)) {
    if ($loopbackInconclusiveReport) {
        Write-Warning "Loopback wav was not created, but an INCONCLUSIVE sidecar report was written: $loopbackReportPath"
    } else {
        $wrapperErrors.Add("Loopback wav was not created: $loopbackWavPath") | Out-Null
    }
}
foreach ($segmentWavFile in $loopbackSegmentWavFiles) {
    if (-not (Test-Path $segmentWavFile -PathType Leaf)) {
        if ($loopbackInconclusiveReport) {
            Write-Warning "Loopback segment wav was not found, but an INCONCLUSIVE sidecar report was written: $segmentWavFile"
        } else {
            $wrapperErrors.Add("Loopback segment wav was not found: $segmentWavFile") | Out-Null
        }
    }
}
foreach ($segmentMetadataFile in $loopbackSegmentMetadataFiles) {
    if (-not (Test-Path $segmentMetadataFile -PathType Leaf)) {
        if ($loopbackInconclusiveReport) {
            Write-Warning "Loopback segment metadata was not found, but an INCONCLUSIVE sidecar report was written: $segmentMetadataFile"
        } else {
            $wrapperErrors.Add("Loopback segment metadata was not found: $segmentMetadataFile") | Out-Null
        }
    }
}
if (-not $loopbackReportExists) {
    Write-Warning "Loopback report missing: $loopbackReportPath"
    $wrapperErrors.Add("Loopback report was not created: $loopbackReportPath") | Out-Null
}
if ($loopbackInconclusiveReport) {
    Write-Warning "Loopback capture result is INCONCLUSIVE; preserving sidecar evidence without treating the capture sidecar as a hard wrapper failure."
}

$appReport = $null
if (Test-Path $runReportPath -PathType Leaf) {
    try {
        $appReport = Get-Content -Path $runReportPath -Encoding UTF8 -Raw | ConvertFrom-Json
    } catch {
        Write-Warning "Smoke app report exists but could not be parsed: $runReportPath"
    }
}

$smokeHarness = $null
$smokeHarnessResult = ""
if (Test-Path $runHarnessReportPath -PathType Leaf) {
    try {
        $smokeHarness = Get-Content -Path $runHarnessReportPath -Encoding UTF8 -Raw | ConvertFrom-Json
        $smokeHarnessResult = [string]$smokeHarness.result
    } catch {
        Write-Warning "Smoke harness report exists but could not be parsed: $runHarnessReportPath"
    }
}

$manualObservation = if ($null -ne $smokeHarness -and $null -ne $smokeHarness.manualObservation) {
    $smokeHarness.manualObservation
} else {
    [ordered]@{
        requested = $ManualObservationWindowSeconds -gt 0 `
            -or -not [string]::IsNullOrWhiteSpace($ManualObservationNote) `
            -or -not [string]::IsNullOrWhiteSpace($ManualEndpointResult) `
            -or -not [string]::IsNullOrWhiteSpace($ManualEndpointTiming)
        windowSeconds = $ManualObservationWindowSeconds
        note = $ManualObservationNote
        result = $ManualEndpointResult
        timing = $ManualEndpointTiming
        effectiveQuitAfterMs = $effectiveQuitAfterMs
        endpointOutputVerified = $false
        observationLayer = "manual-listening-note-if-provided"
    }
}

$wrapperResult = "PASS"
if ($loopbackCaptureInterrupted -or $loopbackInconclusiveReport) {
    $wrapperResult = "INCONCLUSIVE"
} elseif ($wrapperErrors.Count -gt 0) {
    $wrapperResult = "FAIL"
} elseif ($smokeHarnessResult -eq "INCONCLUSIVE") {
    $wrapperResult = "INCONCLUSIVE"
} elseif ($null -ne $loopbackReport -and [string]$loopbackReport.result -eq "FAIL") {
    $wrapperResult = "FAIL"
} elseif ($null -ne $smokeError -or $smokeHarnessResult -eq "FAIL") {
    $wrapperResult = "FAIL"
}

Write-WrapperSummaryReport -Path $summaryReportPath `
    -Result $wrapperResult `
    -SmokeHarness $smokeHarness `
    -AppReport $appReport `
    -LoopbackReport $loopbackReport `
    -SmokeHarnessReportPath $runHarnessReportPath `
    -AppReportPath $runReportPath `
    -LoopbackAggregateReportPath $loopbackReportPath `
    -LoopbackSegmentReportPaths @($loopbackSegmentMetadataFiles) `
    -LoopbackWavFilePaths @($loopbackSegmentWavFiles) `
    -ManualObservation $manualObservation `
    -WrapperErrors @($wrapperErrors.ToArray()) `
    -SmokeError $(if ($null -ne $smokeError) { [string]$smokeError } else { "" }) `
    -CaptureCleanupWarning $captureCleanupWarning

if (Test-Path $runHarnessReportPath -PathType Leaf) {
    Write-Output "smokeHarness:$runHarnessReportPath"
} else {
    Write-Warning "Smoke harness report missing: $runHarnessReportPath"
}
if (Test-Path $runReportPath -PathType Leaf) {
    Write-Output "smokeReport:$runReportPath"
} else {
    Write-Warning "Smoke app report missing: $runReportPath"
}
if (Test-Path $loopbackWavPath -PathType Leaf) {
    Write-Output "loopbackWav:$loopbackWavPath"
}
if ($loopbackReportExists) {
    Write-Output "loopbackReport:$loopbackReportPath"
}
if (Test-Path $summaryReportPath -PathType Leaf) {
    Write-Output "wrapperSummary:$summaryReportPath"
}
if ($null -ne $loopbackReport) {
    Write-Output "loopbackResult:$($loopbackReport.result)"
    Write-Output "loopbackSegmentCount:$($loopbackReport.segmentCount)"
    foreach ($segmentWavFile in $loopbackSegmentWavFiles) {
        if (Test-Path $segmentWavFile -PathType Leaf) {
            Write-Output "loopbackSegmentWav:$segmentWavFile"
        }
    }
    foreach ($segmentMetadataFile in $loopbackSegmentMetadataFiles) {
        if (Test-Path $segmentMetadataFile -PathType Leaf) {
            Write-Output "loopbackSegmentMetadata:$segmentMetadataFile"
        }
    }
    Write-Output "loopbackCaptureInterrupted:$($loopbackReport.captureInterrupted)"
    if ($loopbackReport.captureInterrupted) {
        Write-Output "loopbackInterruptionReason:$($loopbackReport.interruptionReason)"
        Write-Output "loopbackInterruptionHresult:$($loopbackReport.interruptionHresult)"
    }
    Write-Output "loopbackTransientCandidateCount:$($loopbackReport.transientCandidateCount)"
    Write-Output "loopbackDropoutCandidateCount:$($loopbackReport.dropoutCandidateCount)"
    Write-Output "loopbackTailFadeCandidateObserved:$($loopbackReport.tailFadeCandidateObserved)"
    Write-Output "loopbackTrailingSilenceDurationMs:$($loopbackReport.trailingSilenceDurationMs)"
}
if (-not [string]::IsNullOrWhiteSpace($captureStdout)) {
    Write-Output $captureStdout.Trim()
}
Write-Output "wrapperResult:$wrapperResult"
Write-Output "captureCleanup:stopRequested=$captureCleanupStopRequested exited=$captureCleanupExited forcedKill=$captureCleanupForcedKill warning=$captureCleanupWarning"
$cleanupSummary = Invoke-TestArtifactRetention `
    -RepoRoot $repoRoot `
    -BuildDir $BuildDir `
    -KeepRuns $KeepRuns `
    -PreserveRunTokens @($runId) `
    -NoCleanup:$NoCleanup
Write-Output "cleanup:removedRuns=$($cleanupSummary.removedRuns) removedFiles=$($cleanupSummary.removedFiles) keptRuns=$($cleanupSummary.keptRuns)"
Write-Output "archive:archivedRuns=$($cleanupSummary.archivedRuns) archivedFiles=$($cleanupSummary.archivedFiles) manifest=$($cleanupSummary.archiveManifest)"
foreach ($archiveError in @($cleanupSummary.archiveErrors)) {
    Write-Warning "archiveError:$archiveError"
}
if ($wrapperErrors.Count -gt 0) {
    throw ($wrapperErrors -join "; ")
}
if ($null -ne $smokeError) {
    throw $smokeError
}
