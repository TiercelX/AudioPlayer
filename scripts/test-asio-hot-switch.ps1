param(
    [string]$BuildDir = "build-mimo-asio",
    [string]$Configuration = "Debug",
    [string]$Source = "build-codex-asio3\fixtures\sine-1khz-minus18db-48k-stereo.wav",
    [int]$AsioOutputIndex = 0,
    [int]$OccupierOutputDeviceIndex = 2,
    [int]$OccupierPlayMs = 5000,
    [int]$RecoveryWaitMs = 3000,
    [int]$AsioQuitAfterMs = 15000
)

$ErrorActionPreference = "Stop"

# Resolve paths
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$buildRoot = Join-Path $repoRoot $BuildDir
$latestTxt = Join-Path $buildRoot "playable\$Configuration\LATEST.txt"
if (Test-Path $latestTxt) {
    $latestDir = (Get-Content $latestTxt -TotalCount 1).Trim()
    if ([System.IO.Path]::IsPathRooted($latestDir)) {
        $appExe = Join-Path $latestDir "AudioPlayer.exe"
    } else {
        $appExe = Join-Path $buildRoot "playable\$Configuration\$latestDir\AudioPlayer.exe"
    }
} else {
    $appExe = Join-Path $buildRoot "playable\$Configuration\AudioPlayer.exe"
}
if (-not (Test-Path $appExe)) { throw "App not found: $appExe" }

$sourcePath = Join-Path $repoRoot $Source
if (-not (Test-Path $sourcePath)) { throw "Source not found: $sourcePath" }

$logDir = Join-Path $buildRoot "cache\logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"

Write-Host "=== ASIO Hot-Switch Test ==="
Write-Host "App: $appExe"
Write-Host "Source: $sourcePath"
Write-Host ""

# --- Step 1: Start WASAPI shared occupier on the same endpoint as ASIO ---
Write-Host "[1/5] Starting WASAPI shared occupier on output device $OccupierOutputDeviceIndex..."
$occupierLog = Join-Path $logDir "hot-switch-occupier-$timestamp.log"
$env:AUDIOPLAYER_LOG_FILE = $occupierLog
$occupierArgs = @("--source", $sourcePath, "--output-device-index", $OccupierOutputDeviceIndex, "--quit-after-ms", 120000)
$occupier = Start-Process -FilePath $appExe -ArgumentList $occupierArgs -PassThru -WindowStyle Hidden
Remove-Item Env:AUDIOPLAYER_LOG_FILE -ErrorAction SilentlyContinue
Write-Host "  Occupier PID=$($occupier.Id), waiting 8s for playback to stabilize..."
Start-Sleep -Seconds 8

if ($occupier.HasExited) {
    Write-Host "  WARNING: Occupier exited early (exit=$($occupier.ExitCode))"
} else {
    Write-Host "  Occupier still running (good)"
}

# --- Step 2: Try ASIO while occupied (wait for retry detection) ---
Write-Host ""
Write-Host "[2/5] Attempting ASIO while occupied..."
$asioLog1 = Join-Path $logDir "hot-switch-asio-occupied-$timestamp.log"
$asioReport1 = Join-Path $logDir "hot-switch-asio-occupied-$timestamp.report.json"
$env:AUDIOPLAYER_LOG_FILE = $asioLog1
$asioArgs1 = @(
    "--source", $sourcePath,
    "--report-file", $asioReport1,
    "--asio-output-index", $AsioOutputIndex,
    "--quit-after-ms", 20000
)
$asio1 = Start-Process -FilePath $appExe -ArgumentList $asioArgs1 -PassThru -WindowStyle Hidden
Remove-Item Env:AUDIOPLAYER_LOG_FILE -ErrorAction SilentlyContinue

# Wait for retry detection (up to 8 seconds)
$retryDetected = $false
for ($i = 0; $i -lt 16; $i++) {
    Start-Sleep -Milliseconds 500
    if ($asio1.HasExited) { break }
    $logContent = Get-Content $asioLog1 -Raw -ErrorAction SilentlyContinue
    if ($logContent -match "session-retry") {
        $retryDetected = $true
        Write-Host "  Retry detected after $($i * 500)ms, stopping ASIO..."
        $asio1.Kill()
        $asio1.WaitForExit(3000) | Out-Null
        break
    }
}
if (-not $asio1.HasExited) {
    $asio1.Kill()
    $asio1.WaitForExit(3000) | Out-Null
}
$asio1Exit = if ($asio1.HasExited) { $asio1.ExitCode } else { -1 }
Write-Host "  ASIO occupied exit=$asio1Exit"

$asioLog1Content = Get-Content $asioLog1 -Raw -ErrorAction SilentlyContinue
$sessionBlocked = $retryDetected -or ($asioLog1Content -match "session-retry|active-external-session|设备仍被其他应用占用")
Write-Host "  Session blocked: $sessionBlocked"

# --- Step 3: Stop occupier ---
Write-Host ""
Write-Host "[3/5] Stopping occupier..."
if (-not $occupier.HasExited) {
    $occupier.Kill()
    $occupier.WaitForExit(3000) | Out-Null
}
Write-Host "  Occupier stopped"

# --- Step 4: Wait ---
Write-Host ""
Write-Host "[4/5] Waiting ${RecoveryWaitMs}ms for endpoint release..."
Start-Sleep -Milliseconds $RecoveryWaitMs

# --- Step 5: Try ASIO after recovery ---
Write-Host ""
Write-Host "[5/5] Attempting ASIO after recovery..."
$asioLog2 = Join-Path $logDir "hot-switch-asio-recovery-$timestamp.log"
$asioReport2 = Join-Path $logDir "hot-switch-asio-recovery-$timestamp.report.json"
$env:AUDIOPLAYER_LOG_FILE = $asioLog2
$asioArgs2 = @(
    "--source", $sourcePath,
    "--report-file", $asioReport2,
    "--asio-output-index", $AsioOutputIndex,
    "--quit-after-ms", $AsioQuitAfterMs
)
$asio2 = Start-Process -FilePath $appExe -ArgumentList $asioArgs2 -PassThru -WindowStyle Hidden
Remove-Item Env:AUDIOPLAYER_LOG_FILE -ErrorAction SilentlyContinue
$asio2.WaitForExit($AsioQuitAfterMs + 5000) | Out-Null
$asio2Exit = if ($asio2.HasExited) { $asio2.ExitCode } else { $asio2.Kill(); -1 }
Write-Host "  ASIO recovery exit=$asio2Exit"

$asioLog2Content = Get-Content $asioLog2 -Raw -ErrorAction SilentlyContinue
$initSucceeded = $asioLog2Content -match "init succeeded"
$firstBuffer = $asioLog2Content -match "firstBufferSwitch"
$noOccupied = $asioLog2Content -notmatch "active-external-session"

Write-Host ""
Write-Host "=== Results ==="
Write-Host "  Occupied-blocked:   $sessionBlocked"
Write-Host "  Recovery-init-ok:   $initSucceeded"
Write-Host "  Recovery-buffer:    $firstBuffer"
Write-Host "  Recovery-no-occupy: $noOccupied"

if ($sessionBlocked -and $initSucceeded -and $firstBuffer) {
    Write-Host ""
    Write-Host "PASS: Hot-switch recovery works!" -ForegroundColor Green
    $exitCode = 0
} else {
    Write-Host ""
    Write-Host "FAIL: Hot-switch recovery incomplete" -ForegroundColor Red
    $exitCode = 1
}

Write-Host ""
Write-Host "Logs:"
Write-Host "  Occupier:       $occupierLog"
Write-Host "  ASIO-occupied:  $asioLog1"
Write-Host "  ASIO-recovery:  $asioLog2"

exit $exitCode
