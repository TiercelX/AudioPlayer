# Shared helpers for AudioPlayer playback harness scripts.
# Dot-source this file from scripts that need process cleanup, log parsing,
# media duration probing, or harness report writing.

function Assert-LogRegexContains {
    param(
        [string[]]$Lines,
        [string]$Pattern,
        [string]$Description
    )

    if (-not ($Lines | Select-String -Pattern $Pattern)) {
        throw "Playback log missing expected event: $Description"
    }
}

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

function Resolve-FirstExistingPath {
    param(
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path $candidate -PathType Leaf)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ""
}

function ConvertFrom-DurationTextToMs {
    param(
        [string]$DurationText
    )

    $durationSeconds = 0.0
    if (-not [double]::TryParse(
            $DurationText,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$durationSeconds)) {
        return -1
    }

    if ($durationSeconds -le 0) {
        return -1
    }

    return [long][Math]::Ceiling($durationSeconds * 1000.0)
}

function Get-MediaDurationMs {
    param(
        [string]$SourcePath,
        [string]$FfprobePath
    )

    if (-not [string]::IsNullOrWhiteSpace($FfprobePath) -and (Test-Path $FfprobePath -PathType Leaf)) {
        $output = & $FfprobePath `
            -v error `
            -show_entries format=duration `
            -of default=noprint_wrappers=1:nokey=1 `
            $SourcePath 2>$null
        if ($LASTEXITCODE -eq 0 -and $null -ne $output) {
            $durationText = [string](@($output) | Select-Object -First 1)
            $durationMs = ConvertFrom-DurationTextToMs -DurationText $durationText
            if ($durationMs -gt 0) {
                return $durationMs
            }
        }
    }

    return -1
}

function Add-UniqueString {
    param(
        [System.Collections.Generic.List[string]]$List,
        [string]$Value
    )

    if (-not [string]::IsNullOrWhiteSpace($Value) -and -not $List.Contains($Value)) {
        $List.Add($Value) | Out-Null
    }
}

function ConvertTo-JsonArray {
    param([System.Collections.Generic.List[string]]$List)

    if ($null -eq $List) {
        return ,@()
    }
    return ,@($List.ToArray())
}

function Get-LogKeyValue {
    param(
        [AllowEmptyString()]
        [string]$Line,
        [string]$Name,
        [AllowEmptyString()]
        [string]$Default = ""
    )

    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $Default
    }

    $pattern = "(?:^|\s)" + [regex]::Escape($Name) + "=([^\s]+)"
    $match = [regex]::Match($Line, $pattern)
    if ($match.Success) {
        return $match.Groups[1].Value
    }
    return $Default
}

function Get-LogFlag {
    param(
        [AllowEmptyString()]
        [string]$Line,
        [string]$Name
    )

    $value = (Get-LogKeyValue -Line $Line -Name $Name -Default "0").ToLowerInvariant()
    return $value -eq "1" -or $value -eq "true"
}

function Get-LogDoubleValue {
    param(
        [AllowEmptyString()]
        [string]$Line,
        [string]$Name,
        [double]$Default = [double]::NaN
    )

    $parsed = 0.0
    $style = [System.Globalization.NumberStyles]::Float
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    if ([double]::TryParse((Get-LogKeyValue -Line $Line -Name $Name), $style, $culture, [ref]$parsed)) {
        return $parsed
    }
    return $Default
}

function Get-LogIntValueOrNull {
    param(
        [AllowEmptyString()]
        [string]$Line,
        [string]$Name
    )

    $parsed = 0
    if ([int]::TryParse((Get-LogKeyValue -Line $Line -Name $Name), [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Get-LogDoubleValueOrNull {
    param(
        [AllowEmptyString()]
        [string]$Line,
        [string]$Name
    )

    $parsed = 0.0
    $style = [System.Globalization.NumberStyles]::Float
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    if ([double]::TryParse((Get-LogKeyValue -Line $Line -Name $Name), $style, $culture, [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Get-MaxLogDoubleValue {
    param(
        [string[]]$Lines,
        [string]$Name
    )

    $values = @($Lines | ForEach-Object {
        Get-LogDoubleValue -Line $_ -Name $Name
    } | Where-Object { -not [double]::IsNaN($_) })
    if ($values.Count -eq 0) {
        return $null
    }

    return [double](($values | Measure-Object -Maximum).Maximum)
}

function Get-DescendantProcessInfo {
    param(
        [int]$RootProcessId
    )

    $result = [System.Collections.Generic.List[object]]::new()
    $queue = [System.Collections.Generic.Queue[int]]::new()
    $seen = [System.Collections.Generic.HashSet[int]]::new()
    $depthByProcessId = @{}
    $queue.Enqueue($RootProcessId)
    $null = $seen.Add($RootProcessId)
    $depthByProcessId[[string]$RootProcessId] = 0

    while ($queue.Count -gt 0) {
        $parentProcessId = $queue.Dequeue()
        $parentDepth = [int]$depthByProcessId[[string]$parentProcessId]
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $parentProcessId" -ErrorAction SilentlyContinue)
        foreach ($child in $children) {
            $childProcessId = [int]$child.ProcessId
            if (-not $seen.Add($childProcessId)) {
                continue
            }

            $childDepth = $parentDepth + 1
            $depthByProcessId[[string]$childProcessId] = $childDepth
            $result.Add([ordered]@{
                processId = $childProcessId
                parentProcessId = [int]$child.ParentProcessId
                depth = $childDepth
                name = [string]$child.Name
                executablePath = [string]$child.ExecutablePath
                commandLine = [string]$child.CommandLine
            }) | Out-Null
            $queue.Enqueue($childProcessId)
        }
    }

    return $result.ToArray()
}

function Wait-TrackedProcessExit {
    param(
        [int[]]$ProcessIds,
        [int]$TimeoutMs = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $live = @(
            foreach ($processId in $ProcessIds) {
                if (Get-Process -Id $processId -ErrorAction SilentlyContinue) {
                    $processId
                }
            }
        )
        if ($live.Count -eq 0) {
            return @()
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    return $live
}

function Stop-TrackedProcessTree {
    param(
        [int]$RootProcessId,
        [switch]$IncludeRoot
    )

    $descendants = @(Get-DescendantProcessInfo -RootProcessId $RootProcessId)
    $targets = [System.Collections.Generic.List[int]]::new()
    foreach ($descendant in ($descendants | Sort-Object @{ Expression = "depth"; Descending = $true }, @{ Expression = "processId"; Descending = $true })) {
        $targets.Add([int]$descendant.processId) | Out-Null
    }
    if ($IncludeRoot -and (Get-Process -Id $RootProcessId -ErrorAction SilentlyContinue)) {
        $targets.Add($RootProcessId) | Out-Null
    }

    $killed = [System.Collections.Generic.List[int]]::new()
    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($processId in $targets) {
        try {
            $process = Get-Process -Id $processId -ErrorAction Stop
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
            $killed.Add($process.Id) | Out-Null
        } catch {
            $errors.Add("pid=${processId}:$($_.Exception.Message)") | Out-Null
        }
    }

    $residual = Wait-TrackedProcessExit -ProcessIds $targets.ToArray() -TimeoutMs 5000
    return [ordered]@{
        descendantsBeforeCleanup = @($descendants)
        killedProcessIds = @($killed.ToArray())
        cleanupErrors = @($errors.ToArray())
        residualProcessIds = @($residual)
    }
}

function Normalize-SmokeResult {
    param(
        [AllowEmptyString()]
        [string]$AppReportResult,
        [System.Collections.Generic.List[string]]$FailureReasons,
        [System.Collections.Generic.List[string]]$InconclusiveReasons,
        [bool]$ExpectedAppReportFailure = $false
    )

    if ($FailureReasons.Count -gt 0) {
        return "FAIL"
    }
    if ($AppReportResult -eq "FAIL") {
        if ($ExpectedAppReportFailure) {
            if ($InconclusiveReasons.Count -gt 0) {
                return "INCONCLUSIVE"
            }
            return "PASS"
        }
        return "FAIL"
    }
    if ($AppReportResult -eq "WARN" -or $AppReportResult -eq "INCONCLUSIVE") {
        return "INCONCLUSIVE"
    }
    if ($InconclusiveReasons.Count -gt 0) {
        return "INCONCLUSIVE"
    }
    if ($AppReportResult -eq "PASS") {
        return "PASS"
    }
    return "INCONCLUSIVE"
}

function Write-HarnessReport {
    param(
        [string]$Path,
        [string]$Result,
        [AllowEmptyString()]
        [string]$AppReportResult,
        [System.Collections.Generic.List[string]]$FailureReasons,
        [System.Collections.Generic.List[string]]$InconclusiveReasons,
        [System.Collections.Generic.List[string]]$Warnings,
        [object]$Files,
        [object]$RequestedActions,
        [object]$ObservedActions,
        [object]$Assertions,
        [object]$ManualObservation,
        [int]$ExitCode,
        [object]$BackendEvidence = $null,
        [string]$EvidenceLayer = "scripted-playback-and-app-report",
        [string]$VerificationLayer = "scripted-playback-and-app-report",
        [bool]$EndpointOutputVerified = $false
    )

    if ($null -eq $BackendEvidence) {
        $BackendEvidence = [ordered]@{
            backend = "unknown"
            scope = "not-evaluated"
            backendStartVerified = $false
            submittedOutputVerified = $false
            endpointOutputVerified = $EndpointOutputVerified
            limitations = @("backend-evidence-not-evaluated")
        }
    }

    $report = [ordered]@{
        schemaVersion = 2
        result = $Result
        appReportResult = $AppReportResult
        failureReasons = ConvertTo-JsonArray -List $FailureReasons
        inconclusiveReasons = ConvertTo-JsonArray -List $InconclusiveReasons
        warnings = ConvertTo-JsonArray -List $Warnings
        evidenceLayer = $EvidenceLayer
        verificationLayer = $VerificationLayer
        endpointOutputVerified = $EndpointOutputVerified
        backendEvidence = $BackendEvidence
        files = $Files
        requestedActions = $RequestedActions
        observedActions = $ObservedActions
        assertions = $Assertions
        manualObservation = $ManualObservation
        exitCode = $ExitCode
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }

    $reportDir = Split-Path -Parent $Path
    if (-not (Test-Path $reportDir -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $reportDir -Force
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
}
