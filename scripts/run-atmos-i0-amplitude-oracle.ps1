param(
    [string]$InputPath = "media\POWDER SNOW Live V9.8.6.eb3",
    [ValidateRange(1, 1000000)]
    [int]$MaxAUs = 21,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$OutputDir = "tmp\i0-oracle",
    [switch]$NoNative
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Parse-Number([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, "(?:^|\s)" + [regex]::Escape($Name) + "=([^\s]+)")
    if (-not $match.Success) { throw "Native R2C output is missing $Name" }
    return [double]::Parse($match.Groups[1].Value,
        [Globalization.CultureInfo]::InvariantCulture)
}

function Parse-Integer([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, "(?:^|\s)" + [regex]::Escape($Name) + "=([^\s]+)")
    if (-not $match.Success) { throw "Native R2C output is missing $Name" }
    return [long]::Parse($match.Groups[1].Value,
        [Globalization.CultureInfo]::InvariantCulture)
}

function Parse-Token([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, "(?:^|\s)" + [regex]::Escape($Name) + "=([^\s]+)")
    if (-not $match.Success) { throw "Native output is missing $Name" }
    return $match.Groups[1].Value
}

$resolvedInput = Resolve-RepoPath $InputPath
$resolvedBuild = Resolve-RepoPath $BuildDir
$resolvedOutput = Resolve-RepoPath $OutputDir
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) {
    throw "Input does not exist: $resolvedInput"
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

$runtimeBin = Join-Path $resolvedBuild "ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin"
$ffmpeg = Join-Path $runtimeBin "ffmpeg.exe"
$ffprobe = Join-Path $runtimeBin "ffprobe.exe"
if (-not (Test-Path -LiteralPath $ffmpeg -PathType Leaf) -or
    -not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
    throw "The project runtime-with-ffprobe tools are missing under $runtimeBin"
}

$probeJsonText = (& $ffprobe -v error -show_streams -show_format -of json $resolvedInput) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed with exit code $LASTEXITCODE" }
$probe = $probeJsonText | ConvertFrom-Json
$stream = @($probe.streams | Where-Object { $_.codec_type -eq "audio" })[0]
if ($null -eq $stream) { throw "ffprobe did not return an audio stream" }
$channels = [int]$stream.channels
$sampleRate = [int]$stream.sample_rate
if ($channels -le 0 -or $sampleRate -le 0) { throw "Invalid decoded stream shape" }

$frames = [long]$MaxAUs * 1536L
$rawPath = Join-Path $resolvedOutput "ffmpeg-first-$MaxAUs-au-f32le.raw"
& $ffmpeg -hide_banner -loglevel warning -y -drc_scale 0 -target_level 0 `
    -i $resolvedInput -map 0:a:0 `
    -frames:a $MaxAUs -c:a pcm_f32le -f f32le $rawPath
if ($LASTEXITCODE -ne 0) { throw "ffmpeg decode failed with exit code $LASTEXITCODE" }
$expectedBytes = $frames * [long]$channels * 4L
$actualBytes = (Get-Item -LiteralPath $rawPath).Length
if ($actualBytes -ne $expectedBytes) {
    throw "Decoded byte count mismatch: expected $expectedBytes, got $actualBytes"
}

$channelNames = @([string]$stream.channel_layout -split '\+')
if ($channelNames.Count -ne $channels) {
    if ([string]$stream.channel_layout -eq "5.1.2" -and $channels -eq 8) {
        $channelNames = @("FL", "FR", "FC", "LFE", "SL", "SR", "TFL", "TFR")
    } else {
        $channelNames = 0..($channels - 1) | ForEach-Object { "ch$_" }
    }
}

$peaks = New-Object double[] $channels
$squareSums = New-Object double[] $channels
$firstNonzero = New-Object long[] $channels
for ($channel = 0; $channel -lt $channels; ++$channel) { $firstNonzero[$channel] = -1L }
$globalPeak = 0.0
$globalSquareSum = 0.0
$globalFirstNonzero = -1L
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($rawPath))
try {
    for ($frame = 0L; $frame -lt $frames; ++$frame) {
        for ($channel = 0; $channel -lt $channels; ++$channel) {
            $sample = [double]$reader.ReadSingle()
            $magnitude = [Math]::Abs($sample)
            if ($magnitude -gt $peaks[$channel]) { $peaks[$channel] = $magnitude }
            $squareSums[$channel] += $sample * $sample
            if ($magnitude -gt 0.0 -and $firstNonzero[$channel] -lt 0) {
                $firstNonzero[$channel] = $frame
            }
            if ($magnitude -gt $globalPeak) { $globalPeak = $magnitude }
            $globalSquareSum += $sample * $sample
            if ($magnitude -gt 0.0 -and $globalFirstNonzero -lt 0) {
                $globalFirstNonzero = $frame
            }
        }
    }
} finally {
    $reader.Dispose()
}

$perChannel = for ($channel = 0; $channel -lt $channels; ++$channel) {
    [ordered]@{
        name = $channelNames[$channel]
        peak = $peaks[$channel]
        rms = [Math]::Sqrt($squareSums[$channel] / [double]$frames)
        firstNonzeroFrame = $firstNonzero[$channel]
        firstNonzeroSeconds = if ($firstNonzero[$channel] -ge 0) {
            $firstNonzero[$channel] / [double]$sampleRate
        } else { $null }
    }
}

$native = $null
$nativeJ0a6 = $null
if (-not $NoNative) {
    $j0a6Probe = Join-Path $resolvedBuild "$Configuration\Eac3NativeConfig4JocSessionProbe.exe"
    $nativeProbe = Join-Path $resolvedBuild "$Configuration\Eac3NativeConfig4R2CProbe.exe"
    if (-not (Test-Path -LiteralPath $j0a6Probe -PathType Leaf) -or
        -not (Test-Path -LiteralPath $nativeProbe -PathType Leaf)) {
        throw "Native J0A6/R2C probes are missing under $(Join-Path $resolvedBuild $Configuration)"
    }
    $j0a6Text = (& $j0a6Probe --max-aus $MaxAUs $resolvedInput) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $j0a6Text -notmatch 'j0a6Layers=PASS') {
        throw "Native J0A6 layer probe failed"
    }
    $j0a6Log = Join-Path $resolvedOutput "native-j0a6-layers-$MaxAUs-au.txt"
    [IO.File]::WriteAllText($j0a6Log, $j0a6Text + "`r`n", [Text.UTF8Encoding]::new($false))
    $nativeJ0a6 = [ordered]@{
        mappedPcmPeak = Parse-Number $j0a6Text "mappedPcmPeak"
        mappedPcmRms = Parse-Number $j0a6Text "mappedPcmRms"
        mappedPcmFirstNonzeroFrame = Parse-Integer $j0a6Text "mappedPcmFirstNonzero"
        matrixPeak = Parse-Number $j0a6Text "matrixPeak"
        matrixRms = Parse-Number $j0a6Text "matrixRms"
        matrixFirstNonzeroTimeslotStart = Parse-Integer $j0a6Text "matrixFirstNonzeroTimeslotStart"
        objectQmfPeak = Parse-Number $j0a6Text "objectQmfPeak"
        objectQmfRms = Parse-Number $j0a6Text "objectQmfRms"
        objectQmfFirstNonzeroTimeslotStart = Parse-Integer $j0a6Text "objectQmfFirstNonzeroTimeslotStart"
        objectPcmPeak = Parse-Number $j0a6Text "objectPcmPeak"
        objectPcmRms = Parse-Number $j0a6Text "objectPcmRms"
        objectPcmFirstNonzeroFrame = Parse-Integer $j0a6Text "objectPcmFirstNonzero"
        forwardMatrixConsistency = Parse-Token $j0a6Text "forwardMatrixConsistency"
        programCompleteness = Parse-Token $j0a6Text "programCompleteness"
        log = $j0a6Log
    }
    $nativeText = (& $nativeProbe --max-aus $MaxAUs $resolvedInput) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $nativeText -notmatch 'r2c=PASS') {
        throw "Native R2C probe failed"
    }
    $nativeLog = Join-Path $resolvedOutput "native-r2c-$MaxAUs-au.txt"
    [IO.File]::WriteAllText($nativeLog, $nativeText + "`r`n", [Text.UTF8Encoding]::new($false))
    $native = [ordered]@{
        objectPcmPeak = Parse-Number $nativeText "objectPcmPeak"
        objectPcmRms = Parse-Number $nativeText "objectPcmRms"
        objectPcmFirstNonzeroFrame = Parse-Integer $nativeText "objectPcmFirstNonzero"
        evaluatedGainPeak = Parse-Number $nativeText "evaluatedGainPeak"
        speakerBusPeak = Parse-Number $nativeText "speakerGlobalPeak"
        speakerBusRms = Parse-Number $nativeText "speakerGlobalRms"
        speakerBusFirstNonzeroFrame = Parse-Integer $nativeText "speakerFirstNonzero"
        lfePeak = Parse-Number $nativeText "lfePeak"
        lfeRms = Parse-Number $nativeText "lfeRms"
        lfeFirstNonzeroFrame = Parse-Integer $nativeText "lfeFirstNonzero"
        log = $nativeLog
    }
}

$ffmpegVersion = (& $ffmpeg -version | Select-Object -First 1)
$report = [ordered]@{
    schema = "audioplayer.i0-amplitude-oracle.v1"
    result = "PASS_SCALE_REFERENCE_PROGRAM_COMPLETENESS_BLOCKED"
    productionAcceptance = "INCONCLUSIVE"
    amplitudeScaleReference = "PASS_NO_LARGE_FIXED_GAIN_MISMATCH_OBSERVED"
    programCompleteness = "BLOCKED_NO_INDEPENDENT_OBJECT_RECONSTRUCTION_ORACLE"
    comparisonClass = "amplitude-scale-and-timeline-reference-only-not-object-render-equivalence"
    conclusion = "SCALE_PLAUSIBLE_BUT_COMPLETE_PROGRAM_NOT_PROVEN"
    input = $resolvedInput
    maxAUs = $MaxAUs
    decodedFrames = $frames
    sampleRate = $sampleRate
    codec = [string]$stream.codec_name
    sampleFormat = [string]$stream.sample_fmt
    channels = $channels
    channelLayout = [string]$stream.channel_layout
    ffmpeg = [ordered]@{
        path = $ffmpeg
        ffprobePath = $ffprobe
        version = $ffmpegVersion
        rawF32le = $rawPath
        bytes = $actualBytes
        globalPeak = $globalPeak
        globalRms = [Math]::Sqrt($globalSquareSum / ([double]$frames * $channels))
        firstNonzeroFrame = $globalFirstNonzero
        firstNonzeroSeconds = $globalFirstNonzero / [double]$sampleRate
        perChannel = $perChannel
    }
    nativeR2c = $native
    nativeJ0a6 = $nativeJ0a6
    limitations = @(
        "FFmpeg 5.1.2 output is not an object-rendering oracle for native JOC/System H/binaural output.",
        "FFmpeg FBW activity before the first native object sample cannot prove a missing bed; it can include the backward-compatible downmix and decoder dither.",
        "The native chain currently has no independent oracle proving that the reconstructed JOC objects preserve the complete non-LFE program.",
        "This report can compare order of magnitude and source timeline only.",
        "No normalization, DRC, limiter, or postgain is applied by this script."
    )
}
$reportPath = Join-Path $resolvedOutput "amplitude-oracle-$MaxAUs-au.json"
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 8) + "`r`n",
    [Text.UTF8Encoding]::new($false))
Write-Host "i0AmplitudeOracle=PASS report=$reportPath"
Write-Host ("ffmpegPeak={0:R} ffmpegRms={1:R} ffmpegFirstNonzero={2}" -f `
    $globalPeak, [Math]::Sqrt($globalSquareSum / ([double]$frames * $channels)),
    $globalFirstNonzero)
if ($null -ne $native) {
    Write-Host ("nativeObjectPeak={0:R} nativeSpeakerPeak={1:R} nativeObjectFirstNonzero={2}" -f `
        $native.objectPcmPeak, $native.speakerBusPeak, $native.objectPcmFirstNonzeroFrame)
}
