param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$FfmpegPath = "",
    [ValidateRange(0, 600000)]
    [int]$RenderMilliseconds = 0,
    [switch]$MediaEngine,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        $BuildDir = Join-Path $repoRoot "build-mm"
    } else {
        $BuildDir = Join-Path $repoRoot $BuildDir
    }
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

if (-not [System.IO.Path]::IsPathRooted($Source)) {
    $Source = Join-Path $repoRoot $Source
}
$Source = [System.IO.Path]::GetFullPath($Source)
if (-not (Test-Path $Source -PathType Leaf)) {
    throw "Source file does not exist: $Source"
}

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-app.ps1") -BuildDir $BuildDir -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

$probeName = if ($MediaEngine) { "MfMediaEngineProbe.exe" } else { "MfAtmosProbe.exe" }
$probe = Join-Path $BuildDir "$Configuration\$probeName"
if (-not (Test-Path $probe -PathType Leaf)) {
    throw "MF probe executable is missing: $probe. Build without -NoBuild first."
}

$extension = [System.IO.Path]::GetExtension($Source).ToLowerInvariant()
$expected = switch ($extension) {
    ".eb3" { "eac3" }
    ".ec3" { "eac3" }
    ".mlp" { "truehd" }
    default { "" }
}

$sidecar = $Source
if ($expected) {
    $ffmpegCandidates = @(
        $FfmpegPath,
        (Join-Path $BuildDir "ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffmpeg.exe"),
        (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path $_ -PathType Leaf) }
    $ffmpeg = $ffmpegCandidates | Select-Object -First 1
    if (-not $ffmpeg) {
        throw "No ffmpeg.exe is available to remux the raw Dolby stream. Pass -FfmpegPath."
    }

    $probeCache = Join-Path $BuildDir "mf-atmos-probe"
    New-Item -ItemType Directory -Force -Path $probeCache | Out-Null
    $sourceInfo = Get-Item $Source
    $cacheKeyText = "$Source|$($sourceInfo.Length)|$($sourceInfo.LastWriteTimeUtc.Ticks)|mf-atmos-probe-v1"
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $cacheKey = ([System.BitConverter]::ToString($sha256.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($cacheKeyText)))).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
    $sidecar = Join-Path $probeCache "$cacheKey.mka"
    if (-not (Test-Path $sidecar -PathType Leaf) -or (Get-Item $sidecar).Length -eq 0) {
        & $ffmpeg -y -v error -f $expected -i $Source -map 0:a:0 -c copy -cluster_time_limit 100 -f matroska $sidecar
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $sidecar -PathType Leaf)) {
            throw "FFmpeg sidecar remux failed with exit code $LASTEXITCODE"
        }
    }
}

Write-Host "mfProbeKind=$(if ($MediaEngine) { 'media-engine' } else { 'mfplay-source-reader' })"
Write-Host "mfProbeSource=$Source"
Write-Host "mfProbeSidecar=$sidecar"
if ($expected -and -not $MediaEngine) {
    $probeArguments = @($sidecar, "--expect", $expected)
} else {
    $probeArguments = @($sidecar)
}
if ($RenderMilliseconds -gt 0) {
    $probeArguments += @("--render-ms", $RenderMilliseconds)
}
& $probe @probeArguments
if ($LASTEXITCODE -ne 0) {
    throw "Media Foundation probe failed with exit code $LASTEXITCODE"
}
