param(
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$QtPrefix = "",
    [string]$SmokeSource = "",
    [switch]$SkipUnitTests,
    [switch]$SkipSmoke,
    [switch]$SkipSchemaCheck,
    [switch]$NoBuild,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$resolvedBuildDir = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $BuildDir

$results = @()

function Add-ValidationResult {
    param(
        [string]$Name,
        [string]$Result,
        [string]$Detail = ""
    )

    $script:results += [pscustomobject]@{
        name = $Name
        result = $Result
        detail = $Detail
    }

    $color = switch ($Result) {
        "PASS" { "Green" }
        "FAIL" { "Red" }
        "SKIP" { "Yellow" }
        default { "White" }
    }
    Write-Host "  [$Result] $Name$(if ($Detail) { " - $Detail" })" -ForegroundColor $color
}

Write-Host "=== AudioPlayer Validation ===" -ForegroundColor Cyan
Write-Host "Build: $resolvedBuildDir ($Configuration)`n" -ForegroundColor Gray

# 1. Unit tests
if ($SkipUnitTests) {
    Add-ValidationResult -Name "Unit tests" -Result "SKIP"
} else {
    Write-Host "[1/3] Unit tests..." -ForegroundColor Cyan
    $testReportFile = Join-Path $resolvedBuildDir "test-report.json"
    $testArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        ReportFile = $testReportFile
    }
    if (-not [string]::IsNullOrWhiteSpace($QtPrefix)) { $testArgs.QtPrefix = $QtPrefix }
    if ($NoBuild) { $testArgs.NoBuild = $true }
    if ($Verbose) { $testArgs.Verbose = $true }

    try {
        & (Join-Path $PSScriptRoot "run-tests.ps1") @testArgs
        $testExit = $LASTEXITCODE
    } catch {
        $testExit = 1
    }

    if ($testExit -eq 0) {
        $suiteCount = 0
        if (Test-Path $testReportFile -PathType Leaf) {
            $testReport = Get-Content -Path $testReportFile -Raw | ConvertFrom-Json
            $suiteCount = $testReport.suiteCount
        }
        Add-ValidationResult -Name "Unit tests" -Result "PASS" -Detail "$suiteCount suites"
    } else {
        Add-ValidationResult -Name "Unit tests" -Result "FAIL" -Detail "exit code $testExit"
    }
}

# 2. Report schema validation
if ($SkipSchemaCheck) {
    Add-ValidationResult -Name "Report schema" -Result "SKIP"
} else {
    Write-Host "`n[2/3] Report schema validation..." -ForegroundColor Cyan
    try {
        & (Join-Path $PSScriptRoot "test-harness-reports.ps1") -SelfTest
        $schemaExit = $LASTEXITCODE
    } catch {
        $schemaExit = 1
    }

    if ($schemaExit -eq 0) {
        Add-ValidationResult -Name "Report schema" -Result "PASS" -Detail "self-test passed"
    } else {
        Add-ValidationResult -Name "Report schema" -Result "FAIL" -Detail "self-test failed"
    }
}

# 3. Smoke test
if ($SkipSmoke) {
    Add-ValidationResult -Name "Smoke test" -Result "SKIP"
} else {
    $sourcePath = $SmokeSource
    if ([string]::IsNullOrWhiteSpace($sourcePath)) {
        $sourcePath = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path (Join-Path $BuildDir "fixtures\smoke.wav")
    }

    if (-not (Test-Path $sourcePath -PathType Leaf)) {
        Add-ValidationResult -Name "Smoke test" -Result "SKIP" -Detail "no smoke fixture at $sourcePath"
    } else {
        Write-Host "`n[3/3] Smoke test..." -ForegroundColor Cyan
        try {
            & (Join-Path $PSScriptRoot "run-playback-smoke.ps1") `
                -Source $sourcePath `
                -BuildDir $BuildDir `
                -Configuration $Configuration `
                -RequirePlaying `
                -RejectPlaybackErrors
            $smokeExit = $LASTEXITCODE
        } catch {
            $smokeExit = 1
        }

        if ($smokeExit -eq 0) {
            Add-ValidationResult -Name "Smoke test" -Result "PASS"
        } else {
            Add-ValidationResult -Name "Smoke test" -Result "FAIL" -Detail "exit code $smokeExit"
        }
    }
}

# Summary
Write-Host "`n=== Summary ===" -ForegroundColor Cyan
$passed = @($results | Where-Object { $_.result -eq "PASS" }).Count
$failed = @($results | Where-Object { $_.result -eq "FAIL" }).Count
$skipped = @($results | Where-Object { $_.result -eq "SKIP" }).Count

foreach ($r in $results) {
    $color = switch ($r.result) {
        "PASS" { "Green" }
        "FAIL" { "Red" }
        "SKIP" { "Yellow" }
    }
    Write-Host "  $($r.name): $($r.result)" -ForegroundColor $color
}

Write-Host "`nPassed: $passed  Failed: $failed  Skipped: $skipped" -ForegroundColor $(if ($failed -gt 0) { "Red" } else { "Green" })

# Write aggregate report
$aggregateReport = [pscustomobject]@{
    schemaVersion = 1
    reportType = "validation-aggregate"
    timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ssK")
    result = if ($failed -gt 0) { "FAIL" } else { "PASS" }
    buildDir = $resolvedBuildDir
    configuration = $Configuration
    passed = $passed
    failed = $failed
    skipped = $skipped
    checks = $results
}

$aggregatePath = Join-Path $resolvedBuildDir "validation-report.json"
$aggregateReport | ConvertTo-Json -Depth 5 | Set-Content -Path $aggregatePath -Encoding UTF8
Write-Host "`nAggregate report: $aggregatePath" -ForegroundColor Gray

exit $(if ($failed -gt 0) { 1 } else { 0 })
