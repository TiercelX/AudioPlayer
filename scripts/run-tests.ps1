param(
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$QtPrefix = "",
    [switch]$NoBuild,
    [switch]$Verbose,
    [string]$ReportFile = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$resolvedBuildDir = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $BuildDir

function Resolve-QtPrefix {
    param([string]$RequestedQtPrefix)

    if (-not [string]::IsNullOrWhiteSpace($RequestedQtPrefix)) {
        $qtConfig = Join-Path $RequestedQtPrefix "lib\cmake\Qt6\Qt6Config.cmake"
        if (Test-Path $qtConfig -PathType Leaf) {
            return $RequestedQtPrefix
        }
        throw "QtPrefix does not contain lib\cmake\Qt6\Qt6Config.cmake: $RequestedQtPrefix"
    }

    $envPrefix = [System.Environment]::GetEnvironmentVariable("CMAKE_PREFIX_PATH")
    if (-not [string]::IsNullOrWhiteSpace($envPrefix)) {
        $candidate = ($envPrefix -split ';')[0]
        $qtConfig = Join-Path $candidate "lib\cmake\Qt6\Qt6Config.cmake"
        if (Test-Path $qtConfig -PathType Leaf) {
            return $candidate
        }
    }

    foreach ($root in @("D:\Qt", "C:\Qt")) {
        if (-not (Test-Path $root -PathType Container)) { continue }
        $match = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                Get-ChildItem -Path $_.FullName -Directory -Filter "msvc*_64" -ErrorAction SilentlyContinue
            } |
            Where-Object { Test-Path (Join-Path $_.FullName "lib\cmake\Qt6\Qt6Config.cmake") } |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }

    throw "Unable to locate Qt 6. Set -QtPrefix or CMAKE_PREFIX_PATH."
}

function Parse-TestTrace {
    param([string]$TracePath)

    $suites = @()
    if (-not (Test-Path $TracePath -PathType Leaf)) {
        return $suites
    }

    $currentSuite = $null
    foreach ($line in (Get-Content $TracePath)) {
        if ($line -match '^=== RUN\s+(\S+)') {
            $currentSuite = @{
                name = $Matches[1]
                result = "RUNNING"
            }
        }
        elseif ($line -match '^=== (PASS|FAIL)\s+(\S+)' -and $null -ne $currentSuite) {
            $currentSuite.result = $Matches[1]
            $suites += [pscustomobject]$currentSuite
            $currentSuite = $null
        }
    }

    return $suites
}

function Write-TestReport {
    param(
        [string]$ReportPath,
        [string]$OverallResult,
        [array]$Suites,
        [string]$Configuration,
        [string]$BuildDir,
        [string]$TraceFile,
        [int]$ExitCode,
        [datetime]$StartTime,
        [datetime]$EndTime
    )

    $suiteReports = @()
    foreach ($s in $Suites) {
        $suiteReports += [pscustomobject]@{
            name = $s.name
            result = $s.result
        }
    }

    $passedCount = @($Suites | Where-Object { $_.result -eq "PASS" }).Count
    $failedCount = @($Suites | Where-Object { $_.result -eq "FAIL" }).Count

    $report = [pscustomobject]@{
        schemaVersion = 1
        reportType = "unit-test"
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ssK")
        startTime = $StartTime.ToString("yyyy-MM-ddTHH:mm:ssK")
        endTime = $EndTime.ToString("yyyy-MM-ddTHH:mm:ssK")
        durationMs = [int]($EndTime - $StartTime).TotalMilliseconds
        result = $OverallResult
        exitCode = $ExitCode
        configuration = $Configuration
        buildDir = $BuildDir
        traceFile = $TraceFile
        suiteCount = $Suites.Count
        suitesPassed = $passedCount
        suitesFailed = $failedCount
        suites = $suiteReports
    }

    $report | ConvertTo-Json -Depth 5 | Set-Content -Path $ReportPath -Encoding UTF8
    return $report
}

Push-Location $repoRoot

try {
    $startTime = Get-Date

    $vsCmakePath = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    if (Test-Path $vsCmakePath) {
        $env:PATH = "$vsCmakePath;$env:PATH"
    }

    $resolvedQtPrefix = Resolve-QtPrefix -RequestedQtPrefix $QtPrefix
    $qtBinDir = Join-Path $resolvedQtPrefix "bin"
    if (Test-Path $qtBinDir -PathType Container) {
        $env:PATH = "$qtBinDir;$env:PATH"
    }

    if (-not $NoBuild) {
        $configureArgs = @(
            "-S", ".",
            "-B", $resolvedBuildDir,
            "-DCMAKE_PREFIX_PATH=$resolvedQtPrefix",
            "-DCMAKE_BUILD_TYPE=$Configuration",
            "-DAUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=OFF",
            "-DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=OFF"
        )

        Write-Host "Configuring..." -ForegroundColor Cyan
        & cmake @configureArgs
        if ($LASTEXITCODE -ne 0) {
            throw "cmake configure failed with exit code $LASTEXITCODE"
        }

        Write-Host "Building AudioPlayerTests..." -ForegroundColor Cyan
        & cmake --build $resolvedBuildDir --config $Configuration --target AudioPlayerTests
        if ($LASTEXITCODE -ne 0) {
            throw "cmake build failed with exit code $LASTEXITCODE"
        }
    }

    $ctestArgs = @(
        "--test-dir", $resolvedBuildDir,
        "--build-config", $Configuration,
        "--output-on-failure"
    )
    if ($Verbose) {
        $ctestArgs += "--verbose"
    }

    Write-Host "`nRunning tests..." -ForegroundColor Cyan
    & ctest @ctestArgs
    $ctestExit = $LASTEXITCODE
    $endTime = Get-Date

    $traceFile = Join-Path $resolvedBuildDir "test-trace.txt"
    $suites = Parse-TestTrace -TracePath $traceFile
    $overallResult = if ($ctestExit -eq 0) { "PASS" } else { "FAIL" }

    if ([string]::IsNullOrWhiteSpace($ReportFile)) {
        $ReportFile = Join-Path $resolvedBuildDir "test-report.json"
    }

    $report = Write-TestReport `
        -ReportPath $ReportFile `
        -OverallResult $overallResult `
        -Suites $suites `
        -Configuration $Configuration `
        -BuildDir $resolvedBuildDir `
        -TraceFile $traceFile `
        -ExitCode $ctestExit `
        -StartTime $startTime `
        -EndTime $endTime

    if ($ctestExit -eq 0) {
        Write-Host "`nAll tests passed ($($suites.Count) suites)." -ForegroundColor Green
    } else {
        Write-Host "`nTests failed (exit code $ctestExit)." -ForegroundColor Red
    }

    Write-Host "Report: $ReportFile" -ForegroundColor Gray

    if ($Verbose -and (Test-Path $traceFile -PathType Leaf)) {
        Write-Host "`nTrace:" -ForegroundColor Gray
        Get-Content $traceFile
    }

    exit $ctestExit
}
finally {
    Pop-Location
}
