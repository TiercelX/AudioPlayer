param(
    [string]$BuildDir = "",
    [int]$Tail = 120,
    [switch]$PathOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
if (-not (Test-Path $logDir -PathType Container)) {
    throw "Log directory not found: $logDir"
}

$latestLog = Get-ChildItem -Path $logDir -Filter "player-*.log" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($null -eq $latestLog) {
    throw "No player log files found in $logDir"
}

if ($PathOnly) {
    Write-Output $latestLog.FullName
    return
}

Get-Content -Path $latestLog.FullName -Tail $Tail
