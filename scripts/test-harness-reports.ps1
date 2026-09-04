param(
    [string[]]$Path = @(),
    [string]$BuildDir = "",
    [switch]$LatestSmoke,
    [switch]$LatestRegression,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common-paths.ps1")

function Add-Failure {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Message
    )

    $Failures.Add($Message) | Out-Null
}

function Test-Property {
    param(
        [object]$Object,
        [string]$Name
    )

    return $null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]
}

function Get-PropertyValue {
    param(
        [object]$Object,
        [string]$Name
    )

    if (-not (Test-Property -Object $Object -Name $Name)) {
        return $null
    }
    return $Object.PSObject.Properties[$Name].Value
}

function Test-AllowedResult {
    param([string]$Result)

    return $Result -eq "PASS" -or $Result -eq "FAIL" -or $Result -eq "INCONCLUSIVE"
}

function Test-AllowedCaseResult {
    param([string]$Result)

    return (Test-AllowedResult -Result $Result) -or $Result -eq "SKIPPED"
}

function Test-ArrayProperty {
    param(
        [object]$Object,
        [string]$Name,
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Prefix
    )

    if (-not (Test-Property -Object $Object -Name $Name)) {
        Add-Failure -Failures $Failures -Message "$Prefix missing $Name"
        return
    }
    $value = $Object.PSObject.Properties[$Name].Value
    if ($null -ne $value -and $value -isnot [array]) {
        Add-Failure -Failures $Failures -Message "$Prefix $Name must be an array"
    }
}

function Test-RequiredProperties {
    param(
        [object]$Object,
        [string[]]$Names,
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Prefix
    )

    foreach ($name in $Names) {
        if (-not (Test-Property -Object $Object -Name $name)) {
            Add-Failure -Failures $Failures -Message "$Prefix missing $name"
        }
    }
}

function Test-HarnessReport {
    param(
        [object]$Report,
        [string]$ReportPath
    )

    $failures = [System.Collections.Generic.List[string]]::new()
    Test-RequiredProperties -Object $Report -Names @(
        "schemaVersion",
        "result",
        "appReportResult",
        "failureReasons",
        "inconclusiveReasons",
        "warnings",
        "evidenceLayer",
        "verificationLayer",
        "endpointOutputVerified",
        "files",
        "requestedActions",
        "observedActions",
        "assertions",
        "manualObservation",
        "exitCode",
        "generatedAtUtc"
    ) -Failures $failures -Prefix "harness"

    $result = [string](Get-PropertyValue -Object $Report -Name "result")
    if (-not (Test-AllowedResult -Result $result)) {
        Add-Failure -Failures $failures -Message "harness result must be PASS, FAIL, or INCONCLUSIVE"
    }
    if ($result -eq "WARN") {
        Add-Failure -Failures $failures -Message "harness top-level result must not be WARN"
    }

    Test-ArrayProperty -Object $Report -Name "failureReasons" -Failures $failures -Prefix "harness"
    Test-ArrayProperty -Object $Report -Name "inconclusiveReasons" -Failures $failures -Prefix "harness"
    Test-ArrayProperty -Object $Report -Name "warnings" -Failures $failures -Prefix "harness"

    $files = Get-PropertyValue -Object $Report -Name "files"
    Test-RequiredProperties -Object $files -Names @(
        "textLogFile",
        "jsonlDiagnosticFile",
        "appReportFile",
        "harnessReportFile"
    ) -Failures $failures -Prefix "harness.files"

    $endpointOutputVerified = Get-PropertyValue -Object $Report -Name "endpointOutputVerified"
    if ($endpointOutputVerified -isnot [bool]) {
        Add-Failure -Failures $failures -Message "harness endpointOutputVerified must be boolean"
    }

    $schemaVersion = [int](Get-PropertyValue -Object $Report -Name "schemaVersion")
    $backendEvidence = Get-PropertyValue -Object $Report -Name "backendEvidence"
    if ($schemaVersion -ge 2 -or $null -ne $backendEvidence) {
        Test-RequiredProperties -Object $backendEvidence -Names @(
            "backend",
            "scope",
            "backendStartVerified",
            "submittedOutputVerified",
            "endpointOutputVerified",
            "limitations"
        ) -Failures $failures -Prefix "harness.backendEvidence"
        foreach ($boolName in @("backendStartVerified", "submittedOutputVerified", "endpointOutputVerified")) {
            $value = Get-PropertyValue -Object $backendEvidence -Name $boolName
            if ($null -ne $value -and $value -isnot [bool]) {
                Add-Failure -Failures $failures -Message "harness.backendEvidence $boolName must be boolean"
            }
        }
        Test-ArrayProperty -Object $backendEvidence -Name "limitations" -Failures $failures -Prefix "harness.backendEvidence"
    }

    return [ordered]@{
        path = $ReportPath
        kind = "harness"
        valid = $failures.Count -eq 0
        failures = @($failures)
    }
}

function Test-RegressionReport {
    param(
        [object]$Report,
        [string]$ReportPath
    )

    $failures = [System.Collections.Generic.List[string]]::new()
    Test-RequiredProperties -Object $Report -Names @(
        "schemaVersion",
        "result",
        "generatedAtUtc",
        "buildDir",
        "configuration",
        "caseFilter",
        "includeLocalMediaEvidence",
        "summary",
        "evidenceOnlySummary",
        "categorySummary",
        "roleSummary",
        "cases"
    ) -Failures $failures -Prefix "regression"

    $result = [string](Get-PropertyValue -Object $Report -Name "result")
    if (-not (Test-AllowedResult -Result $result)) {
        Add-Failure -Failures $failures -Message "regression result must be PASS, FAIL, or INCONCLUSIVE"
    }
    if ($result -eq "WARN") {
        Add-Failure -Failures $failures -Message "regression top-level result must not be WARN"
    }

    foreach ($summaryName in @("summary", "evidenceOnlySummary")) {
        $summary = Get-PropertyValue -Object $Report -Name $summaryName
        Test-RequiredProperties -Object $summary -Names @(
            "total",
            "passed",
            "failed",
            "inconclusive",
            "skipped"
        ) -Failures $failures -Prefix "regression.$summaryName"
    }

    $cases = @(Get-PropertyValue -Object $Report -Name "cases")
    for ($i = 0; $i -lt $cases.Count; ++$i) {
        $case = $cases[$i]
        $prefix = "regression.cases[$i]"
        Test-RequiredProperties -Object $case -Names @(
            "name",
            "result",
            "evidenceOnly",
            "category",
            "caseRole",
            "evidenceLayer",
            "requiresLocalMedia",
            "usesGeneratedFixture",
            "expectedInconclusiveReason",
            "skipped",
            "source"
        ) -Failures $failures -Prefix $prefix

        $caseResult = [string](Get-PropertyValue -Object $case -Name "result")
        if (-not (Test-AllowedCaseResult -Result $caseResult)) {
            Add-Failure -Failures $failures -Message "$prefix result must be PASS, FAIL, INCONCLUSIVE, or SKIPPED"
        }
        foreach ($textName in @("name", "category", "caseRole", "evidenceLayer", "source")) {
            $value = [string](Get-PropertyValue -Object $case -Name $textName)
            if ([string]::IsNullOrWhiteSpace($value)) {
                Add-Failure -Failures $failures -Message "$prefix $textName must not be empty"
            }
        }
        foreach ($boolName in @("evidenceOnly", "requiresLocalMedia", "usesGeneratedFixture", "skipped")) {
            $value = Get-PropertyValue -Object $case -Name $boolName
            if ($null -ne $value -and $value -isnot [bool]) {
                Add-Failure -Failures $failures -Message "$prefix $boolName must be boolean"
            }
        }
    }

    return [ordered]@{
        path = $ReportPath
        kind = "regression"
        valid = $failures.Count -eq 0
        failures = @($failures)
    }
}

function New-ValidHarnessReportFixture {
    return [pscustomobject][ordered]@{
        schemaVersion = 2
        result = "PASS"
        appReportResult = "PASS"
        failureReasons = [object[]]@()
        inconclusiveReasons = [object[]]@()
        warnings = [object[]]@("actual-endpoint-output-not-verified")
        evidenceLayer = "scripted-playback-and-app-report"
        verificationLayer = "scripted-playback-and-app-report"
        endpointOutputVerified = $false
        backendEvidence = [pscustomobject][ordered]@{
            backend = "Windows ASIO"
            scope = "asio-selection-start-submitted-output-and-driver-callback"
            backendStartVerified = $true
            submittedOutputVerified = $true
            endpointOutputVerified = $false
            limitations = [object[]]@("endpoint-output-not-verified")
            asio = [pscustomobject][ordered]@{
                requested = $true
                backendStartVerified = $true
                submittedOutputVerified = $true
                submittedOutputEvidenceLayer = "asio-submitted-pcm-artifact-monitor"
                endpointOutputVerified = $false
            }
        }
        files = [pscustomobject][ordered]@{
            textLogFile = "test.log"
            jsonlDiagnosticFile = "test.jsonl"
            appReportFile = "test.report.json"
            harnessReportFile = "test.harness.json"
        }
        requestedActions = [pscustomobject][ordered]@{}
        observedActions = [pscustomobject][ordered]@{}
        assertions = [pscustomobject][ordered]@{}
        manualObservation = [pscustomobject][ordered]@{}
        exitCode = 0
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }
}

function New-ValidRegressionReportFixture {
    return [pscustomobject][ordered]@{
        schemaVersion = 1
        result = "PASS"
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        buildDir = "build-mm"
        configuration = "Debug"
        caseFilter = [object[]]@("wav-play-stop")
        includeLocalMediaEvidence = $false
        summary = [pscustomobject][ordered]@{
            total = 1
            executed = 1
            passed = 1
            failed = 0
            inconclusive = 0
            skipped = 0
        }
        evidenceOnlySummary = [pscustomobject][ordered]@{
            total = 0
            passed = 0
            failed = 0
            inconclusive = 0
            skipped = 0
        }
        categorySummary = [object[]]@([pscustomobject][ordered]@{
            category = "play-stop"
            total = 1
            passed = 1
            failed = 0
            inconclusive = 0
            skipped = 0
            evidenceOnly = 0
        })
        roleSummary = [object[]]@([pscustomobject][ordered]@{
            role = "gate"
            total = 1
            passed = 1
            failed = 0
            inconclusive = 0
            skipped = 0
        })
        cases = [object[]]@([pscustomobject][ordered]@{
            name = "wav-play-stop"
            result = "PASS"
            evidenceOnly = $false
            category = "play-stop"
            caseRole = "gate"
            evidenceLayer = "scripted-playback-gate"
            requiresLocalMedia = $false
            usesGeneratedFixture = $true
            expectedInconclusiveReason = ""
            skipped = $false
            source = "build-mm\fixtures\smoke.wav"
        })
    }
}

function Invoke-SelfTest {
    $failures = [System.Collections.Generic.List[string]]::new()

    $validHarness = Test-HarnessReport -Report (New-ValidHarnessReportFixture) -ReportPath "selftest-valid.harness.json"
    if (-not $validHarness.valid) {
        Add-Failure -Failures $failures -Message "valid harness fixture failed schema validation"
        foreach ($failure in @($validHarness.failures)) {
            Add-Failure -Failures $failures -Message "  $failure"
        }
    }

    $legacyHarness = New-ValidHarnessReportFixture
    $legacyHarness.schemaVersion = 1
    $legacyHarness.PSObject.Properties.Remove("backendEvidence")
    $legacyHarnessResult = Test-HarnessReport -Report $legacyHarness -ReportPath "selftest-legacy-v1.harness.json"
    if (-not $legacyHarnessResult.valid) {
        Add-Failure -Failures $failures -Message "legacy v1 harness fixture failed schema validation"
        foreach ($failure in @($legacyHarnessResult.failures)) {
            Add-Failure -Failures $failures -Message "  $failure"
        }
    }

    $invalidHarness = New-ValidHarnessReportFixture
    $invalidHarness.result = "WARN"
    $invalidHarnessResult = Test-HarnessReport -Report $invalidHarness -ReportPath "selftest-invalid.harness.json"
    if ($invalidHarnessResult.valid) {
        Add-Failure -Failures $failures -Message "invalid harness fixture unexpectedly passed schema validation"
    }

    $missingBackendEvidenceHarness = New-ValidHarnessReportFixture
    $missingBackendEvidenceHarness.PSObject.Properties.Remove("backendEvidence")
    $missingBackendEvidenceResult = Test-HarnessReport -Report $missingBackendEvidenceHarness -ReportPath "selftest-missing-backend-evidence.harness.json"
    if ($missingBackendEvidenceResult.valid) {
        Add-Failure -Failures $failures -Message "harness fixture missing backendEvidence unexpectedly passed schema validation"
    }

    $validRegression = Test-RegressionReport -Report (New-ValidRegressionReportFixture) -ReportPath "selftest-valid-regression.json"
    if (-not $validRegression.valid) {
        Add-Failure -Failures $failures -Message "valid regression fixture failed schema validation"
        foreach ($failure in @($validRegression.failures)) {
            Add-Failure -Failures $failures -Message "  $failure"
        }
    }

    $invalidRegression = New-ValidRegressionReportFixture
    $invalidRegression.cases[0].category = ""
    $invalidRegressionResult = Test-RegressionReport -Report $invalidRegression -ReportPath "selftest-invalid-regression.json"
    if ($invalidRegressionResult.valid) {
        Add-Failure -Failures $failures -Message "invalid regression fixture unexpectedly passed schema validation"
    }

    if ($failures.Count -gt 0) {
        foreach ($failure in @($failures)) {
            Write-Output ("selfTest:FAIL:{0}" -f $failure)
        }
        throw "Harness report schema self-test failed"
    }

    Write-Output "selfTest:PASS"
}

function Get-LatestReportPath {
    param(
        [string]$ReportDirectory,
        [string]$Pattern
    )

    if (-not (Test-Path $ReportDirectory -PathType Container)) {
        return ""
    }
    $file = Get-ChildItem -Path $ReportDirectory -Filter $Pattern -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $file) {
        return ""
    }
    return $file.FullName
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir

if ($SelfTest) {
    Invoke-SelfTest
    return
}

$reportPaths = [System.Collections.Generic.List[string]]::new()
foreach ($inputPath in $Path) {
    if ([string]::IsNullOrWhiteSpace($inputPath)) {
        continue
    }
    $reportPaths.Add((Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $inputPath)) | Out-Null
}

$logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
if ($LatestSmoke) {
    $latestSmokePath = Get-LatestReportPath -ReportDirectory $logDir -Pattern "player-smoke-*.harness.json"
    if ([string]::IsNullOrWhiteSpace($latestSmokePath)) {
        throw "No harness report found in $logDir"
    }
    $reportPaths.Add($latestSmokePath) | Out-Null
}
if ($LatestRegression) {
    $latestRegressionPath = Get-LatestReportPath -ReportDirectory $logDir -Pattern "playback-regression-*.json"
    if ([string]::IsNullOrWhiteSpace($latestRegressionPath)) {
        throw "No regression report found in $logDir"
    }
    $reportPaths.Add($latestRegressionPath) | Out-Null
}
if ($reportPaths.Count -eq 0) {
    $latestSmokePath = Get-LatestReportPath -ReportDirectory $logDir -Pattern "player-smoke-*.harness.json"
    if (-not [string]::IsNullOrWhiteSpace($latestSmokePath)) {
        $reportPaths.Add($latestSmokePath) | Out-Null
    }
}
if ($reportPaths.Count -eq 0) {
    throw "No report paths supplied and no latest harness report was found in $logDir"
}

$results = [System.Collections.Generic.List[object]]::new()
foreach ($reportPath in $reportPaths) {
    if (-not (Test-Path $reportPath -PathType Leaf)) {
        throw "Report not found: $reportPath"
    }

    $report = Get-Content -Path $reportPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $fileName = [System.IO.Path]::GetFileName($reportPath)
    $result = if ($fileName -like "playback-regression-*.json" -and $fileName -notlike "*.harness.json") {
        Test-RegressionReport -Report $report -ReportPath $reportPath
    } else {
        Test-HarnessReport -Report $report -ReportPath $reportPath
    }
    $results.Add($result) | Out-Null
}

$failed = @($results | Where-Object { -not $_.valid })
foreach ($result in $results) {
    $status = if ($result.valid) { "PASS" } else { "FAIL" }
    Write-Output ("{0}:{1}:{2}" -f $result.kind, $status, $result.path)
    foreach ($failure in @($result.failures)) {
        Write-Output ("  {0}" -f $failure)
    }
}

if ($failed.Count -gt 0) {
    throw "Harness report schema validation failed for $($failed.Count) report(s)"
}
