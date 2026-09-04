[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AdmPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$ReferenceRoot = "",
    [string]$CachePath = "",
    [string]$NativeExe = "",
    [string]$FfmpegPath = "",
    [string]$ExpectedCacheSourceSha256 = "09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa",
    [double]$StartSec = 0.0,
    [Parameter(Mandatory = $true)]
    [double]$EndSec,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($ReferenceRoot)) {
    $ReferenceRoot = Join-Path $repoRoot "tmp\reference"
}
if ([string]::IsNullOrWhiteSpace($CachePath)) {
    $CachePath = Join-Path $repoRoot "tmp\r2a-system-h-brir.cache"
}
if ([string]::IsNullOrWhiteSpace($NativeExe)) {
    $NativeExe = Join-Path $repoRoot "build-mm\Release\Eac3SystemHBrirOffline.exe"
}
if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    # The self-built audio-core FFmpeg intentionally omits container muxers;
    # prefer a complete local FFmpeg for the WAV audition/export step.
    $ffmpegCommand = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($null -ne $ffmpegCommand) {
        $FfmpegPath = $ffmpegCommand.Source
    } else {
        $candidate = Join-Path $repoRoot "build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffmpeg.exe"
        if (Test-Path -LiteralPath $candidate) { $FfmpegPath = $candidate }
    }
}

foreach ($required in @($AdmPath, $ReferenceRoot, $CachePath)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required path does not exist: $required"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $ReferenceRoot "ear-2.1.0\venv\Scripts\python.exe"))) {
    throw "Pinned EAR Python is missing under $ReferenceRoot"
}
if (-not (Test-Path -LiteralPath $FfmpegPath)) { throw "ffmpeg.exe was not found; pass -FfmpegPath" }
if (-not (Test-Path -LiteralPath $NativeExe)) {
    if ($NoBuild) { throw "Native offline CLI is missing: $NativeExe" }
    cmake --build (Join-Path $repoRoot "build-mm") --config Release --target Eac3SystemHBrirOffline -- /m:2
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $NativeExe)) {
        throw "Failed to build native offline CLI: $NativeExe"
    }
}
if ($StartSec -lt 0 -or $EndSec -le $StartSec) { throw "Require 0 <= StartSec < EndSec" }

$resolvedOut = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOut -Force | Out-Null
$python = Join-Path $ReferenceRoot "ear-2.1.0\venv\Scripts\python.exe"
$helper = Join-Path $PSScriptRoot "render-adm-system-h-binaural.py"
$wrapHelper = Join-Path $PSScriptRoot "wrap-f32-wav.py"
$peakHelper = Join-Path $PSScriptRoot "f32-peak.py"
$earLog = Join-Path $resolvedOut "ear-render.log"
$ffmpegLog = Join-Path $resolvedOut "ffmpeg-convert.log"
$nativeLog = Join-Path $resolvedOut "native-brir.log"
$mainEarRaw = Join-Path $resolvedOut "ear-system-h-22ch-f32.raw"
$mainEarWav = Join-Path $resolvedOut "ear-system-h-22ch-f32.wav"
$mainRaw = Join-Path $resolvedOut "system-h-22ch-f32.raw"
$lfeRaw = Join-Path $resolvedOut "ear-system-h-lfe-2ch-f32.raw"
$binauralRaw = Join-Path $resolvedOut "system-h-binaural-f32.raw"
$binauralWav = Join-Path $resolvedOut "system-h-binaural-f32.wav"
$audition = Join-Path $resolvedOut "system-h-binaural-audition-safe-s24.wav"
$peakJson = Join-Path $resolvedOut "binaural-peak.json"
foreach ($artifact in @($mainEarRaw, $mainEarWav, $mainRaw, $lfeRaw, $binauralRaw, $binauralWav, $audition)) {
    if (Test-Path -LiteralPath $artifact) { throw "Refusing to overwrite existing output: $artifact" }
}

$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $python $helper --adm (Resolve-Path $AdmPath).Path --out-dir $resolvedOut `
    --reference-root (Resolve-Path $ReferenceRoot).Path --start-sec $StartSec --end-sec $EndSec `
    2>&1 | Out-File -LiteralPath $earLog -Encoding utf8
$pythonExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
if ($pythonExitCode -ne 0) { throw "EAR ADM render failed; see $earLog" }

# The bundled FFmpeg build exposes f32le as a muxer but not as a demuxer, so
# wrap the raw bus in a standards-compliant IEEE-float WAV before the explicit
# lossless FFmpeg raw conversion.
& $python $wrapHelper --input $mainEarRaw --output $mainEarWav --channels 22 --rate 48000
if ($LASTEXITCODE -ne 0) { throw "failed to wrap EAR System H raw bus" }

# Keep the explicit ffmpeg step in the provenance chain. This is a raw f32
# format conversion only: no sample-rate, channel-count, or codec change.
$ErrorActionPreference = "Continue"
& $FfmpegPath -hide_banner -loglevel error -y -i $mainEarWav -c:a pcm_f32le -f f32le $mainRaw 2>&1 | Out-File -LiteralPath $ffmpegLog -Encoding utf8
$ffmpegExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
if ($ffmpegExitCode -ne 0) { throw "ffmpeg 22ch raw conversion failed; see $ffmpegLog" }

$cacheHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $CachePath).Hash.ToLowerInvariant()
$ErrorActionPreference = "Continue"
& $NativeExe --cache $CachePath --expected-cache-sha256 $ExpectedCacheSourceSha256 `
    --input $mainRaw --output $binauralRaw 2>&1 | Out-File -LiteralPath $nativeLog -Encoding utf8
$nativeExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
if ($nativeExitCode -ne 0) { throw "System H BRIR CLI failed; see $nativeLog" }

# Convert the native raw result to WAV only to give FFmpeg a portable input
# demuxer for the final fixed-gain S24 audition file.
& $python $wrapHelper --input $binauralRaw --output $binauralWav --channels 2 --rate 48000
if ($LASTEXITCODE -ne 0) { throw "failed to wrap native binaural raw output" }

$peakText = & $python $PSScriptRoot\f32-peak.py --input $binauralRaw --channels 2
if ($LASTEXITCODE -ne 0) { throw "failed to measure native binaural peak" }
$peakText | Set-Content -LiteralPath $peakJson -Encoding utf8
$peak = $peakText | ConvertFrom-Json
$effectiveGainDb = [double]$peak.effectiveGainDb
$gainText = $effectiveGainDb.ToString("R", [Globalization.CultureInfo]::InvariantCulture)

$ErrorActionPreference = "Continue"
& $FfmpegPath -hide_banner -loglevel error -y -i $binauralWav `
    -af "volume=${gainText}dB" -c:a pcm_s24le $audition 2>&1 | Out-File -LiteralPath $ffmpegLog -Append -Encoding utf8
$ffmpegExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
if ($ffmpegExitCode -ne 0) { throw "ffmpeg audition export failed; see $ffmpegLog" }

$earManifest = Get-Content -Raw -LiteralPath (Join-Path $resolvedOut "ear-provenance.json") | ConvertFrom-Json
$nativeText = Get-Content -Raw -LiteralPath $nativeLog
$manifest = [ordered]@{
    renderer = "EAR 2.1.0 ADM -> BS.2051 9+10+3 -> project BBC BRIR"
    adm = (Resolve-Path $AdmPath).Path
    admSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $AdmPath).Hash
    cache = (Resolve-Path $CachePath).Path
    cacheSha256 = $cacheHash
    startSec = $StartSec
    endSec = $EndSec
    ear = $earManifest
    ffmpegRawConversion = "ffmpeg -i ear-system-h-22ch-f32.wav -c:a pcm_f32le -f f32le"
    nativeCommand = "Eac3SystemHBrirOffline --cache --input --output"
    nativeResult = $nativeText.Trim()
    outputs = [ordered]@{
        systemH22chRaw = $mainRaw
        lfe2chRaw = $lfeRaw
        binauralF32Raw = $binauralRaw
        auditionSafeS24 = $audition
    }
    audition = [ordered]@{
        requestedGainDb = -2.0
        effectiveGainDb = $effectiveGainDb
        rawPeakDbfs = [double]$peak.peakDbfs
        postGainPeakDbfs = [double]$peak.postGainPeakDbfs
        headroomDb = [double]$peak.headroomDb
        peakMeasurement = $peakJson
    }
    lfePolicy = "LFE sidecar is preserved but not sent through the 22-emitter BRIR; no undocumented LFE-to-ear downmix is applied."
    sourceUnmodified = $true
}
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resolvedOut "provenance.json") -Encoding UTF8
$manifest | ConvertTo-Json -Depth 10
