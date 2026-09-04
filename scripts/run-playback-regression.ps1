param(
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string[]]$CaseFilter = @(),
    [string]$RegressionReportFile = "",
    [string]$FfmpegPathOverride = "",
    [string]$FfprobePathOverride = "",
    [switch]$IncludeLocalMediaEvidence,
    [ValidateRange(1, 100000)]
    [int]$KeepRuns = 20,
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

function ConvertTo-SafeFileName {
    param([string]$Name)

    return ($Name -replace '[^A-Za-z0-9_.-]', '_')
}

function ConvertTo-JsonArray {
    param([object]$Value)

    if ($null -eq $Value) {
        return ,@()
    }
    return ,@($Value)
}

function Get-CaseMetadata {
    param(
        [hashtable]$Case,
        [bool]$EvidenceOnly
    )

    $name = ([string]$Case.Name).ToLowerInvariant()
    $source = [string]$Case.Args.Source
    $sourceName = [System.IO.Path]::GetFileName($source).ToLowerInvariant()
    $category = "playback"
    if ($name -like "*seek-resume*") {
        $category = "seek-resume"
    } elseif ($name -like "*seek*") {
        $category = "seek"
    } elseif ($name -like "*pause-resume*") {
        $category = "pause-resume"
    } elseif ($name -like "*switch-source*") {
        $category = "source-switch"
    } elseif ($name -like "*output-refresh*") {
        $category = "output-reconfiguration"
    } elseif ($name -like "*finish*") {
        $category = "finish"
    } elseif ($name -like "*no-ffmpeg*" -or $name -like "*no-ffprobe*") {
        $category = "tool-failure-path"
    } elseif ($name -like "*play-stop*") {
        $category = "play-stop"
    }

    $requiresLocalMedia = $source.StartsWith("media\") -or
        $source.StartsWith("media/") -or
        $sourceName -eq "real-alac-sample.m4a"
    $usesGeneratedFixture = -not $requiresLocalMedia
    $caseRole = if ($EvidenceOnly) { "evidence-only" } else { "gate" }
    $evidenceLayer = if ($EvidenceOnly) {
        "scripted-playback-local-compatibility-evidence"
    } else {
        "scripted-playback-gate"
    }
    $expectedInconclusiveReason = if ($EvidenceOnly) {
        "local-media-compatibility-evidence-does-not-gate-regression"
    } else {
        ""
    }

    return [ordered]@{
        category = $category
        caseRole = $caseRole
        evidenceLayer = $evidenceLayer
        requiresLocalMedia = $requiresLocalMedia
        usesGeneratedFixture = $usesGeneratedFixture
        expectedInconclusiveReason = $expectedInconclusiveReason
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
. (Join-Path $PSScriptRoot "cleanup-test-artifacts.ps1")
$smokeScript = Join-Path $PSScriptRoot "run-playback-smoke.ps1"
$fixtureScript = Join-Path $PSScriptRoot "ensure-playback-fixtures.ps1"

& $fixtureScript -BuildDir $BuildDir

$fixtureWav = "$BuildDir\fixtures\smoke.wav"
$fixtureFlac = "$BuildDir\fixtures\smoke.flac"
$fixtureMp3 = "$BuildDir\fixtures\smoke.mp3"
$fixtureAac = "$BuildDir\fixtures\smoke.aac"
$fixtureM4a = "$BuildDir\fixtures\smoke.m4a"
$fixtureAlac = "$BuildDir\fixtures\smoke-alac.m4a"
$fixtureSeekResumeAlac = "$BuildDir\fixtures\sine-1khz-minus18db-48k-stereo-alac.m4a"
$fixtureAc3 = "$BuildDir\fixtures\smoke.ac3"
$fixtureEc3 = "$BuildDir\fixtures\smoke.ec3"
$fixtureSeekResumeSine = "$BuildDir\fixtures\sine-1khz-minus18db-48k-stereo.wav"
$fixtureRealAlacSample = "$BuildDir\fixtures\real-alac-sample.m4a"

$regressionRunId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), ([guid]::NewGuid().ToString("N").Substring(0, 8))
$defaultReportDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
$regressionReportPath = if ([string]::IsNullOrWhiteSpace($RegressionReportFile)) {
    Join-Path $defaultReportDir "playback-regression-$regressionRunId.json"
} elseif ([System.IO.Path]::IsPathRooted($RegressionReportFile)) {
    $RegressionReportFile
} else {
    Join-Path $repoRoot $RegressionReportFile
}
$regressionReportPath = [System.IO.Path]::GetFullPath($regressionReportPath)
$regressionReportDir = Split-Path -Parent $regressionReportPath
if (-not (Test-Path $regressionReportDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $regressionReportDir -Force
}
if (Test-Path $regressionReportPath -PathType Leaf) {
    Remove-Item -Path $regressionReportPath -Force
}
$caseResults = [System.Collections.Generic.List[object]]::new()

$cases = @(
    @{
        Name = "ec3-seek"
        Args = @{
            Source = $fixtureEc3
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 9000
            SeekAfterMs = 1500
            SeekToMs = 4000
            RequirePlaying = $true
            RequireSeekCompletion = $true
            ExpectedSeekTargetMs = 4000
            RequireAudibleLevels = $true
            RejectPlaybackErrors = $true
        }
    },
    @{
        Name = "wav-play-stop"
        Args = @{
            Source = $fixtureWav
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "flac-play-stop"
        Args = @{
            Source = $fixtureFlac
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "mp3-play-stop"
        Args = @{
            Source = $fixtureMp3
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "aac-play-stop"
        Args = @{
            Source = $fixtureAac
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "m4a-play-stop"
        Args = @{
            Source = $fixtureM4a
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "alac-play-stop"
        Args = @{
            Source = $fixtureAlac
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "alac-no-ffprobe-play-stop"
        Args = @{
            Source = $fixtureAlac
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            FfprobePathOverride = "disabled"
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "ac3-play-stop"
        Args = @{
            Source = $fixtureAc3
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            StopAfterMs = 4000
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RequireAudibleLevels = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 1500
        }
    },
    @{
        Name = "wav-double-seek"
        Args = @{
            Source = $fixtureWav
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 10000
            SeekAfterMs = 1200
            SeekToMs = 4000
            SecondSeekAfterMs = 2600
            SecondSeekToMs = 7000
            RequirePlaying = $true
            RequireSeekCompletionCount = 2
            ExpectedSeekTargetMs = 7000
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 7000
        }
    },
    @{
        Name = "sine-seek-resume-repeat"
        Args = @{
            Source = $fixtureSeekResumeSine
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 11000
            SeekPauseResume = $true
            SeekSequence = "1000:5000,2500:12000,4000:20000,5500:30000"
            RequirePlaying = $true
            RequireSeekCompletionCount = 4
            RequireSeekResumeProfileCount = 4
            ExpectedSeekTargetMs = 30000
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 30000
        }
    },
    @{
        Name = "alac-sine-seek-resume-repeat"
        Args = @{
            Source = $fixtureSeekResumeAlac
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 11000
            SeekPauseResume = $true
            SeekSequence = "1000:5000,2500:12000,4000:20000,5500:30000"
            RequirePlaying = $true
            RequireSeekCompletionCount = 4
            RequireSeekResumeProfileCount = 4
            ExpectedSeekTargetMs = 30000
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 30000
        }
    },
    @{
        Name = "wav-switch-source"
        Args = @{
            Source = $fixtureWav
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 9000
            SwitchSourceAfterMs = 1800
            SwitchSource = $fixtureFlac
            StopAfterMs = 5500
            RequirePlaying = $true
            RequireLoadCount = 2
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
        }
    },
    @{
        Name = "wav-output-refresh"
        Args = @{
            Source = $fixtureWav
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 7000
            RefreshOutputAfterMs = 1500
            StopAfterMs = 4500
            RequirePlaying = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RequireLogPattern = 'applyOutputDeviceChange hot-reconfigure|refreshOutputConfiguration'
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 2000
        }
    },
    @{
        Name = "eb3-seek"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.eb3"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 8000
            SeekAfterMs = 3500
            SeekToMs = 30000
            RequirePlaying = $true
            RequireSeekCompletion = $true
            ExpectedSeekTargetMs = 30000
            RejectPlaybackErrors = $true
        }
    },
    @{
        Name = "eb3-pause-resume-stop"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.eb3"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 12000
            PauseAfterMs = 2500
            ResumeAfterMs = 4500
            StopAfterMs = 7000
            RequirePlaying = $true
            RequirePauseResume = $true
            RequireStop = $true
            RequireStopFadeOut = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 3000
        }
    },
    @{
        Name = "eb3-no-ffprobe-pause-stop"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.eb3"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 10000
            PauseAfterMs = 2500
            StopAfterMs = 6000
            FfprobePathOverride = "disabled"
            RequirePlaying = $true
            RequireStop = $true
            RejectPlaybackErrors = $true
            MinReachedPositionMs = 2000
        }
    },
    @{
        Name = "eb3-finish-near-end"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.eb3"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 9000
            SeekAfterMs = 1500
            SeekToMs = 445000
            RequirePlaying = $true
            RequireFinished = $true
            RejectPlaybackErrors = $true
        }
    },
    @{
        Name = "eb3-no-ffmpeg-error"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.eb3"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 4000
            FfmpegPathOverride = "disabled"
            RequireErrorPattern = "ffmpeg"
        }
    },
    @{
        Name = "mlp-seek"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.mlp"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 10000
            SeekAfterMs = 4000
            SeekToMs = 30000
            RequirePlaying = $true
            RequireSeekCompletion = $true
            ExpectedSeekTargetMs = 30000
            RejectPlaybackErrors = $true
        }
    },
    @{
        Name = "mlp-finish-near-end"
        Args = @{
            Source = "media\POWDER SNOW Live V9.8.6.mlp"
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 12000
            SeekAfterMs = 2500
            SeekToMs = 445000
            RequirePlaying = $true
            RequireFinished = $true
            RejectPlaybackErrors = $true
        }
    }
)

$localMediaEvidenceCases = @(
    @{
        Name = "real-alac-seek-resume-repeat"
        EvidenceOnly = $true
        Args = @{
            Source = $fixtureRealAlacSample
            BuildDir = $BuildDir
            Configuration = $Configuration
            QuitAfterMs = 11000
            SeekPauseResume = $true
            SeekSequence = "1000:71594,2500:69093,4000:134747,5500:49710"
            RequirePlaying = $true
            RequireSeekCompletionCount = 4
            RequireSeekResumeProfileCount = 4
            RejectPlaybackErrors = $true
        }
    }
)

if ($IncludeLocalMediaEvidence -or $CaseFilter.Count -gt 0) {
    foreach ($evidenceCase in $localMediaEvidenceCases) {
        $includeEvidenceCase = [bool]$IncludeLocalMediaEvidence
        if (-not $includeEvidenceCase) {
            foreach ($filter in $CaseFilter) {
                if ($evidenceCase.Name -like $filter) {
                    $includeEvidenceCase = $true
                    break
                }
            }
        }
        if ($includeEvidenceCase) {
            $cases += $evidenceCase
        }
    }
}

foreach ($case in $cases) {
    $evidenceOnly = $case.ContainsKey("EvidenceOnly") -and [bool]$case.EvidenceOnly
    $caseMetadata = Get-CaseMetadata -Case $case -EvidenceOnly $evidenceOnly
    if ($CaseFilter.Count -gt 0) {
        $matchesFilter = $false
        foreach ($filter in $CaseFilter) {
            if ($case.Name -like $filter) {
                $matchesFilter = $true
                break
            }
        }
        if (-not $matchesFilter) {
            continue
        }
    }

    $sourceCandidate = Join-Path $repoRoot $case.Args.Source
    if (-not [System.IO.Path]::IsPathRooted($case.Args.Source) -and -not (Test-Path $sourceCandidate -PathType Leaf)) {
        $caseRole = if ($evidenceOnly) { ":evidence-only" } else { "" }
        Write-Output "case:$($case.Name):skip:missing-source$caseRole"
        $caseResults.Add([ordered]@{
            name = $case.Name
            result = "SKIPPED"
            evidenceOnly = $evidenceOnly
            category = $caseMetadata.category
            caseRole = $caseMetadata.caseRole
            evidenceLayer = $caseMetadata.evidenceLayer
            requiresLocalMedia = $caseMetadata.requiresLocalMedia
            usesGeneratedFixture = $caseMetadata.usesGeneratedFixture
            expectedInconclusiveReason = $caseMetadata.expectedInconclusiveReason
            skipped = $true
            reason = "missing-source"
            source = $case.Args.Source
        }) | Out-Null
        continue
    }

    Write-Output "case:$($case.Name):begin"
    $caseArgs = @{}
    foreach ($entry in $case.Args.GetEnumerator()) {
        $caseArgs[$entry.Key] = $entry.Value
    }

    if ($PSBoundParameters.ContainsKey("FfmpegPathOverride") -and -not $caseArgs.ContainsKey("FfmpegPathOverride")) {
        $caseArgs["FfmpegPathOverride"] = $FfmpegPathOverride
    }
    if ($PSBoundParameters.ContainsKey("FfprobePathOverride") -and -not $caseArgs.ContainsKey("FfprobePathOverride")) {
        $caseArgs["FfprobePathOverride"] = $FfprobePathOverride
    }

    $caseHarnessReportPath = Join-Path $regressionReportDir ("playback-regression-$regressionRunId-{0}.harness.json" -f (ConvertTo-SafeFileName -Name $case.Name))
    $caseArgs["HarnessReportFile"] = $caseHarnessReportPath
    $caseArgs["NoCleanup"] = $true

    $caseError = ""
    $caseOutput = @()
    try {
        $caseOutput = & $smokeScript @caseArgs 2>&1
    } catch {
        $caseError = $_.Exception.Message
        $caseOutput = @($_.Exception.Message)
    }
    $caseOutput | ForEach-Object { Write-Output $_ }

    $harnessReport = $null
    $harnessParseError = ""
    if (Test-Path $caseHarnessReportPath -PathType Leaf) {
        try {
            $harnessReport = Get-Content -Path $caseHarnessReportPath -Encoding UTF8 -Raw | ConvertFrom-Json
        } catch {
            $harnessParseError = $_.Exception.Message
        }
    }

    $caseResult = "FAIL"
    $failureReasons = @()
    $inconclusiveReasons = @()
    $warnings = @()
    $textLogFile = $null
    $appReportFile = $null
    $jsonlDiagnosticFile = $null
    $verificationLayer = ""
    $endpointOutputVerified = $false
    $processCleanup = $null
    if ($null -ne $harnessReport) {
        $caseResult = [string]$harnessReport.result
        $failureReasons = ConvertTo-JsonArray -Value $harnessReport.failureReasons
        $inconclusiveReasons = ConvertTo-JsonArray -Value $harnessReport.inconclusiveReasons
        $warnings = ConvertTo-JsonArray -Value $harnessReport.warnings
        $textLogFile = $harnessReport.files.textLogFile
        $appReportFile = $harnessReport.files.appReportFile
        $jsonlDiagnosticFile = $harnessReport.files.jsonlDiagnosticFile
        $verificationLayer = [string]$harnessReport.verificationLayer
        $endpointOutputVerified = [bool]$harnessReport.endpointOutputVerified
        $processCleanup = $harnessReport.observedActions.processCleanup
    } else {
        if (-not [string]::IsNullOrWhiteSpace($harnessParseError)) {
            $failureReasons = @("harness-report-parse-failed:$harnessParseError")
        } else {
            $failureReasons = @("harness-report-missing")
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($caseError) -and $caseResult -ne "FAIL") {
        $warnings = @($warnings + "smoke-script-error:$caseError")
    }

    $caseResults.Add([ordered]@{
        name = $case.Name
        result = $caseResult
        evidenceOnly = $evidenceOnly
        category = $caseMetadata.category
        caseRole = $caseMetadata.caseRole
        evidenceLayer = $caseMetadata.evidenceLayer
        requiresLocalMedia = $caseMetadata.requiresLocalMedia
        usesGeneratedFixture = $caseMetadata.usesGeneratedFixture
        expectedInconclusiveReason = $caseMetadata.expectedInconclusiveReason
        skipped = $false
        source = $case.Args.Source
        harnessReportFile = $caseHarnessReportPath
        textLogFile = $textLogFile
        appReportFile = $appReportFile
        jsonlDiagnosticFile = $jsonlDiagnosticFile
        verificationLayer = $verificationLayer
        endpointOutputVerified = $endpointOutputVerified
        processCleanup = $processCleanup
        failureReasons = @($failureReasons)
        inconclusiveReasons = @($inconclusiveReasons)
        warnings = @($warnings)
        error = $caseError
    }) | Out-Null

    $caseRole = if ($evidenceOnly) { ":evidence-only" } else { "" }
    Write-Output "case:$($case.Name):$($caseResult.ToLowerInvariant())$caseRole"
}

$gateCaseResults = @($caseResults | Where-Object { -not $_.evidenceOnly })
$evidenceCaseResults = @($caseResults | Where-Object { $_.evidenceOnly })
$failedCount = @($gateCaseResults | Where-Object { $_.result -eq "FAIL" }).Count
$inconclusiveCount = @($gateCaseResults | Where-Object { $_.result -eq "INCONCLUSIVE" }).Count
$passedCount = @($gateCaseResults | Where-Object { $_.result -eq "PASS" }).Count
$skippedCount = @($gateCaseResults | Where-Object { $_.result -eq "SKIPPED" }).Count
$executedCount = $gateCaseResults.Count - $skippedCount
$evidenceFailedCount = @($evidenceCaseResults | Where-Object { $_.result -eq "FAIL" }).Count
$evidenceInconclusiveCount = @($evidenceCaseResults | Where-Object { $_.result -eq "INCONCLUSIVE" }).Count
$evidencePassedCount = @($evidenceCaseResults | Where-Object { $_.result -eq "PASS" }).Count
$evidenceSkippedCount = @($evidenceCaseResults | Where-Object { $_.result -eq "SKIPPED" }).Count
$categorySummary = @(
    $caseResults |
        Group-Object -Property { [string]$_["category"] } |
        Sort-Object Name |
        ForEach-Object {
            [ordered]@{
                category = $_.Name
                total = $_.Count
                passed = @($_.Group | Where-Object { $_.result -eq "PASS" }).Count
                failed = @($_.Group | Where-Object { $_.result -eq "FAIL" }).Count
                inconclusive = @($_.Group | Where-Object { $_.result -eq "INCONCLUSIVE" }).Count
                skipped = @($_.Group | Where-Object { $_.result -eq "SKIPPED" }).Count
                evidenceOnly = @($_.Group | Where-Object { $_.evidenceOnly }).Count
            }
        }
)
$roleSummary = @(
    $caseResults |
        Group-Object -Property { [string]$_["caseRole"] } |
        Sort-Object Name |
        ForEach-Object {
            [ordered]@{
                role = $_.Name
                total = $_.Count
                passed = @($_.Group | Where-Object { $_.result -eq "PASS" }).Count
                failed = @($_.Group | Where-Object { $_.result -eq "FAIL" }).Count
                inconclusive = @($_.Group | Where-Object { $_.result -eq "INCONCLUSIVE" }).Count
                skipped = @($_.Group | Where-Object { $_.result -eq "SKIPPED" }).Count
            }
        }
)
$regressionResult = if ($failedCount -gt 0) {
    "FAIL"
} elseif ($inconclusiveCount -gt 0 -or $executedCount -eq 0) {
    "INCONCLUSIVE"
} else {
    "PASS"
}

$regressionReport = [ordered]@{
    schemaVersion = 1
    result = $regressionResult
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    buildDir = $BuildDir
    configuration = $Configuration
    caseFilter = @($CaseFilter)
    includeLocalMediaEvidence = [bool]$IncludeLocalMediaEvidence
    summary = [ordered]@{
        total = $gateCaseResults.Count
        executed = $executedCount
        passed = $passedCount
        failed = $failedCount
        inconclusive = $inconclusiveCount
        skipped = $skippedCount
    }
    evidenceOnlySummary = [ordered]@{
        total = $evidenceCaseResults.Count
        passed = $evidencePassedCount
        failed = $evidenceFailedCount
        inconclusive = $evidenceInconclusiveCount
        skipped = $evidenceSkippedCount
    }
    categorySummary = @($categorySummary)
    roleSummary = @($roleSummary)
    cases = @($caseResults)
}
$regressionReport | ConvertTo-Json -Depth 10 | Set-Content -Path $regressionReportPath -Encoding UTF8

Write-Output "regressionReport:$regressionReportPath"
Write-Output "regressionResult:$regressionResult"
Write-Output "regressionSummary:total=$($gateCaseResults.Count) executed=$executedCount passed=$passedCount failed=$failedCount inconclusive=$inconclusiveCount skipped=$skippedCount evidenceOnly=$($evidenceCaseResults.Count) evidencePassed=$evidencePassedCount evidenceFailed=$evidenceFailedCount evidenceInconclusive=$evidenceInconclusiveCount evidenceSkipped=$evidenceSkippedCount"

$preserveRunTokens = @()
foreach ($caseResult in $caseResults) {
    if (-not [string]::IsNullOrWhiteSpace([string]$caseResult.textLogFile)) {
        $textLogName = [System.IO.Path]::GetFileName([string]$caseResult.textLogFile)
        if ($textLogName -match '^player-smoke-(?<run>.+?)\.log$') {
            $preserveRunTokens += $Matches.run
        }
    }
}
$cleanupSummary = Invoke-TestArtifactRetention `
    -RepoRoot $repoRoot `
    -BuildDir $BuildDir `
    -KeepRuns $KeepRuns `
    -PreserveRunTokens $preserveRunTokens `
    -NoCleanup:$NoCleanup
Write-Output "cleanup:removedRuns=$($cleanupSummary.removedRuns) removedFiles=$($cleanupSummary.removedFiles) keptRuns=$($cleanupSummary.keptRuns)"
Write-Output "archive:archivedRuns=$($cleanupSummary.archivedRuns) archivedFiles=$($cleanupSummary.archivedFiles) manifest=$($cleanupSummary.archiveManifest)"
foreach ($archiveError in @($cleanupSummary.archiveErrors)) {
    Write-Warning "archiveError:$archiveError"
}

if ($regressionResult -eq "FAIL") {
    throw "Playback regression result FAIL: $failedCount case(s) failed"
}
