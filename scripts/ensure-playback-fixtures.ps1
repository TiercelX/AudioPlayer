param(
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

function Resolve-FfmpegExecutable {
    param(
        [string]$RepoRoot
    )

    $override = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_FFMPEG_PATH")
    if (-not [string]::IsNullOrWhiteSpace($override)) {
        if ($override -ieq "disabled" -or $override -ieq "none") {
            throw "AUDIOPLAYER_FFMPEG_PATH disables ffmpeg; cannot generate playback fixtures"
        }
        if (Test-Path $override -PathType Leaf) {
            return $override
        }
    }

    $bundled = Join-Path $RepoRoot "ffmpeg.exe"
    if (Test-Path $bundled -PathType Leaf) {
        return $bundled
    }

    $command = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    throw "Unable to locate ffmpeg for playback fixture generation"
}

function Ensure-Fixture {
    param(
        [string]$Path,
        [string[]]$Arguments,
        [switch]$ForceRegenerate
    )

    if (Test-Path $Path -PathType Leaf) {
        if (-not $ForceRegenerate) {
            $existingItem = Get-Item $Path
            if ($existingItem.Length -gt 0) {
                return
            }
        }
    }

    & $script:ffmpegExecutable "-v" "error" @Arguments
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Path -PathType Leaf) -or (Get-Item $Path).Length -le 0) {
        throw "Failed to generate playback fixture: $Path"
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$fixturesRoot = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path (Join-Path $BuildDir "fixtures")
$null = New-Item -ItemType Directory -Force -Path $fixturesRoot
$script:ffmpegExecutable = Resolve-FfmpegExecutable -RepoRoot $repoRoot

$wavPath = Join-Path $fixturesRoot "smoke.wav"
$flacPath = Join-Path $fixturesRoot "smoke.flac"
$mp3Path = Join-Path $fixturesRoot "smoke.mp3"
$aacPath = Join-Path $fixturesRoot "smoke.aac"
$m4aPath = Join-Path $fixturesRoot "smoke.m4a"
$alacPath = Join-Path $fixturesRoot "smoke-alac.m4a"
$seekResumeAlacPath = Join-Path $fixturesRoot "sine-1khz-minus18db-48k-stereo-alac.m4a"
$ac3Path = Join-Path $fixturesRoot "smoke.ac3"
$ec3Path = Join-Path $fixturesRoot "smoke.ec3"
$pinkNoiseWavPath = Join-Path $fixturesRoot "ab-pink-noise-48k-stereo-minus18db.wav"
$sine997WavPath = Join-Path $fixturesRoot "ab-sine-997hz-48k-stereo-minus18db.wav"
$silenceWavPath = Join-Path $fixturesRoot "silence-48k-stereo.wav"
$lowToneWavPath = Join-Path $fixturesRoot "sine-1khz-minus30db-48k-stereo.wav"
$mediumTone18WavPath = Join-Path $fixturesRoot "sine-1khz-minus18db-48k-stereo.wav"
$mediumTone12WavPath = Join-Path $fixturesRoot "sine-1khz-minus12db-48k-stereo.wav"
$s24WavPath = Join-Path $fixturesRoot "sine-1khz-48k-stereo-s24.wav"
$s32WavPath = Join-Path $fixturesRoot "sine-1khz-48k-stereo-s32.wav"

Ensure-Fixture -Path $wavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=660:sample_rate=48000:duration=12",
    "-c:a", "pcm_s16le",
    $wavPath
)

Ensure-Fixture -Path $flacPath -Arguments @(
    "-y",
    "-i", $wavPath,
    "-c:a", "flac",
    $flacPath
)

Ensure-Fixture -Path $mp3Path -Arguments @(
    "-y",
    "-i", $wavPath,
    "-c:a", "libmp3lame",
    "-b:a", "192k",
    $mp3Path
)

Ensure-Fixture -Path $aacPath -Arguments @(
    "-y",
    "-i", $wavPath,
    "-c:a", "aac",
    "-b:a", "128k",
    "-f", "adts",
    $aacPath
)

Ensure-Fixture -Path $m4aPath -Arguments @(
    "-y",
    "-i", $wavPath,
    "-c:a", "aac",
    "-b:a", "128k",
    $m4aPath
)

Ensure-Fixture -Path $alacPath -Arguments @(
    "-y",
    "-i", $wavPath,
    "-c:a", "alac",
    $alacPath
)

Ensure-Fixture -Path $ac3Path -ForceRegenerate -Arguments @(
    "-y",
    "-i", $wavPath,
    "-filter:a", "pan=5.1(side)|FL=c0|FR=c0|FC=c0|LFE=0.5*c0|SL=c0|SR=c0",
    "-c:a", "ac3",
    "-b:a", "640k",
    "-f", "ac3",
    $ac3Path
)

Ensure-Fixture -Path $ec3Path -ForceRegenerate -Arguments @(
    "-y",
    "-i", $wavPath,
    "-filter:a", "pan=5.1(side)|FL=c0|FR=c0|FC=c0|LFE=0.5*c0|SL=c0|SR=c0",
    "-c:a", "eac3",
    "-b:a", "640k",
    "-f", "eac3",
    $ec3Path
)

Ensure-Fixture -Path $pinkNoiseWavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "anoisesrc=color=pink:sample_rate=48000:duration=60:amplitude=1:seed=20260422",
    "-filter:a", "volume=-18dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s16le",
    $pinkNoiseWavPath
)

Ensure-Fixture -Path $sine997WavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=997:sample_rate=48000:duration=60",
    "-filter:a", "pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s16le",
    $sine997WavPath
)

Ensure-Fixture -Path $silenceWavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "anullsrc=channel_layout=stereo:sample_rate=48000",
    "-t", "60",
    "-c:a", "pcm_s16le",
    $silenceWavPath
)

Ensure-Fixture -Path $lowToneWavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=1000:sample_rate=48000:duration=60",
    "-filter:a", "volume=-30dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s16le",
    $lowToneWavPath
)

Ensure-Fixture -Path $mediumTone18WavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=1000:sample_rate=48000:duration=60",
    "-filter:a", "volume=-18dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s16le",
    $mediumTone18WavPath
)

Ensure-Fixture -Path $seekResumeAlacPath -Arguments @(
    "-y",
    "-i", $mediumTone18WavPath,
    "-c:a", "alac",
    $seekResumeAlacPath
)

Ensure-Fixture -Path $mediumTone12WavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=1000:sample_rate=48000:duration=60",
    "-filter:a", "volume=-12dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s16le",
    $mediumTone12WavPath
)

Ensure-Fixture -Path $s24WavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=1000:sample_rate=48000:duration=12",
    "-filter:a", "volume=-18dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s24le",
    $s24WavPath
)

Ensure-Fixture -Path $s32WavPath -Arguments @(
    "-y",
    "-f", "lavfi",
    "-i", "sine=frequency=1000:sample_rate=48000:duration=12",
    "-filter:a", "volume=-18dB,pan=stereo|c0=c0|c1=c0",
    "-c:a", "pcm_s32le",
    $s32WavPath
)

Write-Output "fixture:$wavPath"
Write-Output "fixture:$flacPath"
Write-Output "fixture:$mp3Path"
Write-Output "fixture:$aacPath"
Write-Output "fixture:$m4aPath"
Write-Output "fixture:$alacPath"
Write-Output "fixture:$seekResumeAlacPath"
Write-Output "fixture:$ac3Path"
Write-Output "fixture:$ec3Path"
Write-Output "fixture:$pinkNoiseWavPath"
Write-Output "fixture:$sine997WavPath"
Write-Output "fixture:$silenceWavPath"
Write-Output "fixture:$lowToneWavPath"
Write-Output "fixture:$mediumTone18WavPath"
Write-Output "fixture:$mediumTone12WavPath"
Write-Output "fixture:$s24WavPath"
Write-Output "fixture:$s32WavPath"
