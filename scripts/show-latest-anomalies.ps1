param(
    [string]$BuildDir = "",
    [int]$Tail = 200,
    [string]$LogPath = "",
    [switch]$PathOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$latestLog = $null
if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
    $logCandidate = if ([System.IO.Path]::IsPathRooted($LogPath)) {
        $LogPath
    } else {
        Join-Path $repoRoot $LogPath
    }
    $resolvedLogPath = [System.IO.Path]::GetFullPath($logCandidate)
    if (-not (Test-Path $resolvedLogPath -PathType Leaf)) {
        throw "Log file not found: $resolvedLogPath"
    }
    $latestLog = Get-Item -Path $resolvedLogPath
} else {
    $logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
    if (-not (Test-Path $logDir -PathType Container)) {
        throw "Log directory not found: $logDir"
    }

    $latestLog = Get-ChildItem -Path $logDir -Filter "player-*.log" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

if ($null -eq $latestLog) {
    throw "No player log files found"
}

if ($PathOnly) {
    Write-Output $latestLog.FullName
    return
}

$pattern = '\[anomaly\]|wasapiError|outputRecovery|outputDeviceChange|hot-reconfigure|refreshOutputConfiguration|drainBeforeReset|buffer-resumed|freshBufferRequired|audioState .*Stopped|audioState .*Idle'
Select-String -Path $latestLog.FullName -Pattern $pattern |
    Select-Object -Last $Tail |
    ForEach-Object { $_.Line }
