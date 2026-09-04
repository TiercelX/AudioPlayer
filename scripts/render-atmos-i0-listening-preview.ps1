param(
    [string]$InputPath = "media\POWDER SNOW Live V9.8.6.eb3",
    [ValidateRange(1, 1000000)] [int]$MaxAUs = 157,
    [switch]$FullFile,
    [ValidateRange(-1, 1024)] [int]$AudioStreamIndex = -1,
    [ValidateRange(0, 64)] [int]$Jobs = 0,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$BrirCache = "tmp\r2a-system-h-brir.cache",
    [string]$OutputDir = "tmp\i0-listen",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Invoke-Captured([string]$Executable, [string[]]$Arguments) {
    $text = (& $Executable @Arguments 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Executable`n$text"
    }
    return $text
}
function Parse-Count([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, "(?m)^" + [regex]::Escape($Name) + "=(\d+)$")
    if (-not $match.Success) { throw "Probe output is missing $Name" }
    return [long]::Parse($match.Groups[1].Value,
        [Globalization.CultureInfo]::InvariantCulture)
}
function Write-MonitorGainCopy([string]$Source, [string]$Destination,
                               [double]$Gain) {
    if (-not [double]::IsFinite($Gain) -or $Gain -le 0.0 -or $Gain -gt 1.0) {
        throw "Invalid monitor gain"
    }
    $reader = [IO.BinaryReader]::new([IO.File]::Open(
        $Source, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read), [Text.Encoding]::UTF8, $false)
    $writer = [IO.BinaryWriter]::new([IO.File]::Open(
        $Destination, [IO.FileMode]::Create, [IO.FileAccess]::Write,
        [IO.FileShare]::None), [Text.Encoding]::UTF8, $false)
    try {
        if ($reader.BaseStream.Length -lt 44L) { throw "WAV is too small" }
        $header = $reader.ReadBytes(44)
        if ([Text.Encoding]::ASCII.GetString($header, 0, 4) -ne 'RIFF' `
            -or [Text.Encoding]::ASCII.GetString($header, 8, 4) -ne 'WAVE' `
            -or [BitConverter]::ToUInt16($header, 20) -ne 3 `
            -or [BitConverter]::ToUInt16($header, 34) -ne 32) {
            throw "Expected 44-byte IEEE float32 WAV"
        }
        $writer.Write($header)
        while ($reader.BaseStream.Position -lt $reader.BaseStream.Length) {
            $value = [double]$reader.ReadSingle() * $Gain
            if (-not [double]::IsFinite($value)) { throw "Nonfinite gained sample" }
            $writer.Write([float]$value)
        }
    } finally {
        $reader.Dispose()
        $writer.Dispose()
    }
}

$resolvedInput = Resolve-RepoPath $InputPath
$resolvedBuild = Resolve-RepoPath $BuildDir
$resolvedCache = Resolve-RepoPath $BrirCache
$resolvedOutput = Resolve-RepoPath $OutputDir
$effectiveJobs = if ($Jobs -eq 0) {
    [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount))
} else { $Jobs }
foreach ($required in @($resolvedInput, $resolvedCache)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required input does not exist: $required"
    }
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
if (-not $NoBuild) {
    & cmake --build $resolvedBuild --config $Configuration `
        --target Eac3AccessUnitProbe Eac3NativeConfig4R2CProbe -- /m:4
    if ($LASTEXITCODE -ne 0) { throw "Native Atmos probe build failed" }
}

$accessProbe = Join-Path $resolvedBuild "$Configuration\Eac3AccessUnitProbe.exe"
$renderProbe = Join-Path $resolvedBuild "$Configuration\Eac3NativeConfig4R2CProbe.exe"
$ffprobe = Join-Path $resolvedBuild `
    "ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffprobe.exe"
foreach ($required in @($accessProbe, $renderProbe, $ffprobe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required executable does not exist: $required"
    }
}

$inputProbeText = (& $ffprobe -v error -show_streams -show_format -of json `
    $resolvedInput) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for input" }
$inputProbe = $inputProbeText | ConvertFrom-Json
$extension = [IO.Path]::GetExtension($resolvedInput).ToLowerInvariant()
$containerInput = $extension -in @('.m4a', '.mp4', '.mka', '.mkv')
$selectedStream = $null
if ($containerInput) {
    $eac3Streams = @($inputProbe.streams | Where-Object {
        $_.codec_type -eq 'audio' -and $_.codec_name -eq 'eac3'
    })
    if ($AudioStreamIndex -ge 0) {
        $selectedStream = @($eac3Streams | Where-Object {
            [int]$_.index -eq $AudioStreamIndex
        })[0]
        if ($null -eq $selectedStream) {
            throw "Requested stream $AudioStreamIndex is not E-AC-3"
        }
    } elseif ($eac3Streams.Count -eq 1) {
        $selectedStream = $eac3Streams[0]
    } elseif ($eac3Streams.Count -eq 0) {
        throw "Container has no E-AC-3 audio stream; transcoding is forbidden"
    } else {
        throw "Container has multiple E-AC-3 streams; specify -AudioStreamIndex"
    }
}

$rawStem = [IO.Path]::GetFileNameWithoutExtension($resolvedInput)
$safeStem = ($rawStem -replace '[^A-Za-z0-9._-]+', '-').Trim('-')
if ([string]::IsNullOrWhiteSpace($safeStem)) { $safeStem = 'atmos-input' }
$rawInput = $resolvedInput
$sidecar = $null
$accessArguments = @($resolvedInput, '--summary')
if ($containerInput) {
    $sidecar = Join-Path $resolvedOutput "$safeStem-native-copy.ec3"
    $accessArguments += @('--audio-stream-index', [string]$selectedStream.index,
                          '--dump-eac3', $sidecar)
}
if (-not $FullFile) { $accessArguments += @('--max-units', [string]$MaxAUs) }
$accessText = Invoke-Captured $accessProbe $accessArguments
$accessUnits = Parse-Count $accessText 'accessUnits'
if ($accessUnits -le 0 -or (-not $FullFile -and $accessUnits -ne $MaxAUs)) {
    throw "Access-unit count mismatch"
}
if ($containerInput) {
    if ($accessText -notmatch '(?m)^dumpEac3=PASS ' `
        -or -not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
        throw "Lossless E-AC-3 sidecar extraction failed"
    }
    $rawInput = $sidecar
}

$configText = Invoke-Captured $accessProbe @(
    $rawInput, '--max-units', '1', '--summary', '--joc')
$configMatch = [regex]::Match($configText, '(?m)^jocDownmixConfigs=([34]):1$')
if (-not $configMatch.Success) {
    throw "Input is not a supported config-3/config-4 JOC stream"
}
$jocConfig = [int]$configMatch.Groups[1].Value
$mode = if ($FullFile) { 'full' } else { "$accessUnits-au" }
$stem = "$safeStem-$mode-config$jocConfig-native-object-only"
$stereoWav = Join-Path $resolvedOutput "$stem-stereo-f32.wav"
$lfeWav = Join-Path $resolvedOutput "$stem-lfe-diagnostic-f32.wav"
$nativeReport = Join-Path $resolvedOutput "$stem-native-report.json"
$probeLog = Join-Path $resolvedOutput "$stem-probe.txt"
$accessLog = Join-Path $resolvedOutput "$stem-access.txt"
$listeningReport = Join-Path $resolvedOutput "$stem-listening-report.json"
[IO.File]::WriteAllText($accessLog, $accessText + "`r`n" + $configText + "`r`n",
    [Text.UTF8Encoding]::new($false))

$renderText = Invoke-Captured $renderProbe @(
    '--joc-config', [string]$jocConfig, '--max-aus', [string]$accessUnits,
    '--jobs', [string]$effectiveJobs,
    '--i0p-cache', $resolvedCache, '--i0p-stereo-wav', $stereoWav,
    '--i0p-lfe-wav', $lfeWav, '--i0p-report', $nativeReport, $rawInput)
if ($renderText -notmatch '(?m)^r2c=PASS ' `
    -or $renderText -notmatch '(?m)^i0p=PASS ') {
    throw "Native I0 listening render failed"
}
[IO.File]::WriteAllText($probeLog, $renderText + "`r`n",
    [Text.UTF8Encoding]::new($false))

$native = Get-Content -LiteralPath $nativeReport -Raw | ConvertFrom-Json
$wavProbeText = (& $ffprobe -v error -show_streams -show_format -of json `
    $stereoWav) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for rendered stereo WAV" }
$wavProbe = $wavProbeText | ConvertFrom-Json
$stream = @($wavProbe.streams | Where-Object { $_.codec_type -eq 'audio' })[0]
$expectedSourceFrames = $accessUnits * 1536L
$wavContract = $null -ne $stream `
    -and $stream.codec_name -eq 'pcm_f32le' `
    -and [int]$stream.channels -eq 2 `
    -and [int]$stream.sample_rate -eq 48000 `
    -and [long]$stream.duration_ts -eq [long]$native.stereoFrames `
    -and [long]$native.sourceFrames -eq $expectedSourceFrames `
    -and [double]$native.stereo.peak -ge 0.0 `
    -and [double]::IsFinite([double]$native.stereo.peak) `
    -and [double]::IsFinite([double]$native.stereo.rms)
if (-not $wavContract) { throw "Rendered stereo WAV contract mismatch" }

$naturalPeak = [double]$native.stereo.peak
$naturalRms = [double]$native.stereo.rms
$listeningWav = $stereoWav
$listeningPeak = $naturalPeak
$listeningRms = $naturalRms
$monitorGain = 1.0
$monitorGainApplied = $false
if ($naturalPeak -gt 0.98) {
    $monitorGain = 0.95 / $naturalPeak
    $listeningWav = Join-Path $resolvedOutput `
        "$stem-preview-monitor-gain-f32.wav"
    Write-MonitorGainCopy $stereoWav $listeningWav $monitorGain
    $listeningPeak = $naturalPeak * $monitorGain
    $listeningRms = $naturalRms * $monitorGain
    $monitorGainApplied = $true
}

$containerDuration = if ($null -ne $inputProbe.format.duration) {
    [double]::Parse([string]$inputProbe.format.duration,
        [Globalization.CultureInfo]::InvariantCulture)
} else { $null }
$report = [ordered]@{
    schema = 'audioplayer.i0-listening-preview.v2'
    result = 'PASS_INTERNAL_LISTENING_ARTIFACT'
    input = $resolvedInput
    inputContainer = $containerInput
    inputContainerDurationSeconds = $containerDuration
    selectedAudioStreamIndex = if ($selectedStream) { [int]$selectedStream.index } else { $null }
    selectedAudioCodec = if ($selectedStream) { [string]$selectedStream.codec_name } else { 'raw-eac3' }
    sidecar = $sidecar
    sidecarPolicy = if ($containerInput) { 'PACKET_COPY_NO_TRANSCODE' } else { 'NOT_REQUIRED_RAW_INPUT' }
    fullFile = [bool]$FullFile
    accessUnits = $accessUnits
    jocConfig = $jocConfig
    programmeCarrierPolicy = 'JOC_RECONSTRUCTED_OBJECTS_ONLY_NO_ADDITIVE_DOWNMIX'
    programmeCarrierAdded = $false
    externalJocEquivalence = 'INCONCLUSIVE_NO_INDEPENDENT_OBJECT_REFERENCE'
    containerTrimPolicy = if ($containerInput) {
        'PACKET_COMPLETE_CONTAINER_PRIMING_AND_END_TRIM_NOT_APPLIED'
    } else { 'NOT_APPLICABLE_RAW_INPUT' }
    processingModel = 'OFFLINE_PREPARED_BATCH_RETENTION_STREAMED_SPEAKER_BUS'
    jobs = $effectiveJobs
    monitorGainApplied = $monitorGainApplied
    monitorGainLinear = $monitorGain
    monitorGainDb = 20.0 * [Math]::Log10($monitorGain)
    normalization = 'NO'
    drc = 'NO'
    limiter = 'NO'
    sampleRate = [int]$stream.sample_rate
    sourceFrames = [long]$native.sourceFrames
    sourceDurationSeconds = [long]$native.sourceFrames / [double]$stream.sample_rate
    stereoFrames = [long]$native.stereoFrames
    stereoDurationSeconds = [long]$native.stereoFrames / [double]$stream.sample_rate
    brirTailFrames = [long]$native.tailFrames
    stereo = [ordered]@{
        path = $listeningWav; codec = [string]$stream.codec_name
        channels = [int]$stream.channels; peak = $listeningPeak
        rms = $listeningRms
    }
    naturalStereoEvidence = [ordered]@{
        path = $stereoWav; peak = $naturalPeak; rms = $naturalRms
        digest = [string]$native.stereo.digest
    }
    lfeDiagnosticSidecar = [ordered]@{
        path = $lfeWav; policy = [string]$native.lfePolicy
        peak = [double]$native.lfe.peak; rms = [double]$native.lfe.rms
    }
    nativeReport = $nativeReport; accessLog = $accessLog; probeLog = $probeLog
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $listeningReport `
    -Encoding utf8NoBOM
Write-Host "i0ListeningPreview=PASS"
Write-Host "jocConfig=$jocConfig accessUnits=$accessUnits fullFile=$([bool]$FullFile)"
Write-Host "jobs=$effectiveJobs"
Write-Host "stereoWav=$listeningWav"
Write-Host "durationSeconds=$($report.stereoDurationSeconds)"
Write-Host "stereoPeak=$($report.stereo.peak) stereoRms=$($report.stereo.rms)"
Write-Host "monitorGainApplied=$monitorGainApplied monitorGainDb=$($report.monitorGainDb)"
Write-Host "sidecarPolicy=$($report.sidecarPolicy)"
Write-Host "report=$listeningReport"
