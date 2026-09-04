<#
.SYNOPSIS
Submit decoded JOC objects to the default Windows Spatial Audio endpoint.
.EXAMPLE
  .\scripts\play-atmos-spatial.ps1 -InputPath media\03. iPad.m4a
#>
param(
    [string]$InputPath = "",
    [string]$OutputDir = "tmp\atmos-spatial",
    [ValidateRange(0, 1000000)] [int]$MaxAccessUnits = 0,
    [ValidateRange(-1, 128)] [int]$AudioStreamIndex = -1,
    [ValidateRange(2, 64)] [int]$QueueBatches = 8,
    [ValidateRange(1, 64)] [int]$PrebufferBatches = 4,
    [ValidateRange(100, 10000)] [int]$PushTimeoutMs = 2000,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$Force,
    [switch]$NoBuild,
    [switch]$DisableLfe,
    [ValidateSet("source", "unit")]
    [string]$PositionRadiusMode = "source",
    [ValidateSet("metadata", "front")]
    [string]$PositionDirectionMode = "metadata",
    [ValidateRange(0.0, 1.0)]
    [double]$AzimuthFocus = 0.0,
    [ValidateSet("standard", "hrtf")]
    [string]$SpatialRenderer = "standard",
    [ValidateSet("small", "outdoors")]
    [string]$HrtfEnvironment = "small",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-InputKind([string]$Path) {
    $extension = [IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($extension -in @('.m4a', '.mp4', '.mka', '.mkv')) { return 'container' }
    if ($extension -in @('.eb3', '.ec3', '.eac3')) { return 'raw-eac3' }
    throw "Unsupported input extension '$extension'; expected .m4a/.mp4/.mka/.mkv/.eb3/.ec3/.eac3"
}

function Find-File([string[]]$Candidates, [string]$Description) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $path = Resolve-RepoPath $candidate
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    throw "Could not find $Description"
}

function Get-SafeStem([string]$Path) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($Path) -replace '[^A-Za-z0-9._-]+', '-'
    $stem = $stem.Trim('-')
    if ([string]::IsNullOrWhiteSpace($stem)) { return 'atmos-input' }
    return $stem
}

function Get-FileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $hash = $algorithm.ComputeHash($stream)
        return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        if ($stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function Invoke-Captured([string]$Executable, [string[]]$Arguments, [string]$LogPath) {
    $previousErrorAction = $ErrorActionPreference
    try {
        # Native ffprobe/probe diagnostics on stderr are expected and must not
        # become terminating PowerShell errors under the wrapper's Stop policy.
        $ErrorActionPreference = 'Continue'
        $text = (& $Executable @Arguments 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    [IO.File]::WriteAllText($LogPath, $text, (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
    return [pscustomobject]@{ Text = $text; ExitCode = $exitCode }
}

function Get-FfprobeJson([string]$Ffprobe, [string]$Path, [bool]$Raw) {
    $arguments = @('-v', 'error')
    if ($Raw) { $arguments += @('-f', 'eac3') }
    $arguments += @('-show_streams', '-show_format', '-of', 'json', $Path)
    $text = (& $Ffprobe @arguments 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for $Path`n$text" }
    return $text | ConvertFrom-Json
}

function New-SpatialProbeArgs([string]$RawInput, [int]$Queue, [int]$Prebuffer,
                              [int]$TimeoutMs, [bool]$DisableLfeFlag,
                              [string]$RadiusMode = "source",
                              [string]$DirectionMode = "metadata",
                              [double]$Focus = 0.0,
                              [string]$Renderer = "standard",
                              [string]$Environment = "small") {
    $result = @($RawInput, '--joc-gate7c', '--summary',
                '--gate7c-queue-batches', [string]$Queue,
                '--gate7c-prebuffer-batches', [string]$Prebuffer,
                '--gate7c-push-timeout-ms', [string]$TimeoutMs,
                '--eac3-drc-scale', '0', '--eac3-cons-noisegen', '0')
    if ($DisableLfeFlag) { $result += '--spatial-disable-lfe' }
    $result += @('--spatial-position-radius', $RadiusMode)
    $result += @('--spatial-position-direction', $DirectionMode)
    $result += @('--spatial-azimuth-focus', $Focus.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
    $result += @('--spatial-renderer', $Renderer)
    $result += @('--spatial-hrtf-environment', $Environment)
    return ,$result
}

function Get-LfeProbeContract([string]$ProbeText) {
    $policyMatch = [regex]::Match($ProbeText, '(?m)^gate7cLfePolicy=(DISABLED_BY_USER|PROGRAM_HEADROOM)')
    $volumeMatch = [regex]::Match($ProbeText, '(?m)^gate7cLfeVolume=([-+0-9.eE]+)')
    $headroomMatch = [regex]::Match($ProbeText, '(?m)^gate7cDynamicGainHeadroomDb=([-+0-9.eE]+)')
    return [pscustomobject]@{
        Policy = if ($policyMatch.Success) { $policyMatch.Groups[1].Value } else { 'UNKNOWN' }
        Volume = if ($volumeMatch.Success) { [double]$volumeMatch.Groups[1].Value } else { $null }
        DynamicHeadroomDb = if ($headroomMatch.Success) { [double]$headroomMatch.Groups[1].Value } else { $null }
    }
}

function Get-ProbeScalar([string]$ProbeText, [string]$Name) {
    $match = [regex]::Match($ProbeText, ('(?m)^' + [regex]::Escape($Name) + '=([-+0-9.eE]+)'))
    if ($match.Success) { return [double]$match.Groups[1].Value }
    return $null
}

function Get-ProbeText([string]$ProbeText, [string]$Name) {
    $match = [regex]::Match($ProbeText, ('(?m)^' + [regex]::Escape($Name) + '=([^\r\n]+)'))
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $null
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "self-test: $Message" }
}

if ($SelfTest) {
    Assert-True ((Get-InputKind 'input.m4a') -eq 'container') 'm4a container detection'
    Assert-True ((Get-InputKind 'input.eb3') -eq 'raw-eac3') 'eb3 raw detection'
    Assert-True ((Get-SafeStem 'A file!.ec3') -eq 'A-file') 'safe stem'
    $caught = $false
    try { Get-InputKind 'input.wav' | Out-Null } catch { $caught = $true }
    Assert-True $caught 'unsupported extension rejection'
    $shaSelfTestPath = Join-Path ([IO.Path]::GetTempPath()) ('audioplayer-spatial-sha-{0}.bin' -f [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllBytes($shaSelfTestPath, [Text.Encoding]::ASCII.GetBytes('abc'))
        Assert-True ((Get-FileSha256 $shaSelfTestPath) -eq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'pure .NET SHA-256 implementation'
    } finally {
        if (Test-Path -LiteralPath $shaSelfTestPath) { Remove-Item -LiteralPath $shaSelfTestPath -Force }
    }
    $defaultArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$false
    $disabledArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$true
    $unitArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$false -RadiusMode 'unit'
    $frontArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$false -DirectionMode 'front'
    $focusArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$false -Focus 0.5
    $hrtfArgs = New-SpatialProbeArgs 'input.ec3' 8 4 2000 -DisableLfeFlag:$true -Renderer 'hrtf' -Environment 'outdoors'
    Assert-True (-not ($defaultArgs -contains '--spatial-disable-lfe')) 'default LFE delegate policy'
    Assert-True ($disabledArgs -contains '--spatial-disable-lfe') 'disabled LFE delegate flag'
    Assert-True (($unitArgs -contains '--spatial-position-radius') -and
                 ($unitArgs -contains 'unit')) 'unit position-radius delegate flag'
    Assert-True (-not ($defaultArgs -contains 'unit')) 'default source position-radius policy'
    Assert-True (($frontArgs -contains '--spatial-position-direction') -and
                 ($frontArgs -contains 'front')) 'front position-direction delegate flag'
    Assert-True (-not ($defaultArgs -contains 'front')) 'default metadata position-direction policy'
    Assert-True (($focusArgs -contains '--spatial-azimuth-focus') -and
                 ($focusArgs -contains '0.5')) 'azimuth-focus delegate flag'
    Assert-True (-not ($defaultArgs -contains '0.5')) 'default zero azimuth-focus policy'
    Assert-True (($hrtfArgs -contains '--spatial-renderer') -and ($hrtfArgs -contains 'hrtf') -and
                 ($hrtfArgs -contains '--spatial-hrtf-environment') -and ($hrtfArgs -contains 'outdoors')) 'HRTF delegate contract'
    $defaultContract = Get-LfeProbeContract "gate7cLfePolicy=PROGRAM_HEADROOM`ngate7cDynamicGainHeadroomDb=15`ngate7cLfeVolume=0.177828"
    $disabledContract = Get-LfeProbeContract "gate7cLfePolicy=DISABLED_BY_USER`ngate7cDynamicGainHeadroomDb=15`ngate7cLfeVolume=0.0"
    Assert-True ($defaultContract.Policy -eq 'PROGRAM_HEADROOM' -and
                 [math]::Abs($defaultContract.Volume - 0.177827941) -lt 0.000001 -and
                 $defaultContract.DynamicHeadroomDb -eq 15) 'default LFE report fields'
    Assert-True ($disabledContract.Policy -eq 'DISABLED_BY_USER' -and
                 $disabledContract.Volume -eq 0.0 -and
                 $disabledContract.DynamicHeadroomDb -eq 15) 'disabled LFE report fields'
    Assert-True ((Get-ProbeScalar 'gate7cPreScaleSamples=12' 'gate7cPreScaleSamples') -eq 12) 'probe scalar parser'
    Write-Host 'playAtmosSpatialSelfTest=PASS cases=16'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputPath)) { throw 'InputPath is required unless -SelfTest is used' }
if ($SpatialRenderer -eq 'hrtf' -and -not $DisableLfe) {
    throw 'SpatialRenderer hrtf requires -DisableLfe; HRTF backend does not submit a static LFE object'
}
$resolvedInput = Resolve-RepoPath $InputPath
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) { throw "Input file does not exist: $resolvedInput" }
$inputKind = Get-InputKind $resolvedInput
$resolvedBuild = Resolve-RepoPath $BuildDir
$resolvedOutput = Resolve-RepoPath $OutputDir
if (-not $NoBuild) {
    & cmake --build $resolvedBuild --config $Configuration --target Eac3AccessUnitProbe -- /m:4
    if ($LASTEXITCODE -ne 0) { throw "Native Atmos probe build failed" }
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$reportPath = Join-Path $resolvedOutput 'spatial-provenance.json'
if ((Test-Path -LiteralPath $reportPath -PathType Leaf) -and -not $Force) {
    throw "Output directory already has spatial-provenance.json; choose a new -OutputDir or pass -Force"
}

$probe = Find-File @(
    (Join-Path $resolvedBuild "$Configuration\Eac3AccessUnitProbe.exe"),
    "build-mm\$Configuration\Eac3AccessUnitProbe.exe"
) 'Eac3AccessUnitProbe.exe'
$ffprobe = Find-File @(
    (Join-Path $resolvedBuild 'ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffprobe.exe'),
    ((Get-Command ffprobe.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source))
) 'ffprobe.exe'
$inputProbe = Get-FfprobeJson $ffprobe $resolvedInput ($inputKind -eq 'raw-eac3')
$selectedStream = $null
$safeStem = Get-SafeStem $resolvedInput
$rawInput = $resolvedInput
$sidecar = $null
$prepLog = Join-Path $resolvedOutput "$safeStem-prep.txt"
if ($inputKind -eq 'container') {
    $eac3Streams = @($inputProbe.streams | Where-Object {
        $_.codec_type -eq 'audio' -and $_.codec_name -eq 'eac3'
    })
    if ($AudioStreamIndex -ge 0) {
        $selectedStream = @($eac3Streams | Where-Object { [int]$_.index -eq $AudioStreamIndex })[0]
        if ($null -eq $selectedStream) { throw "Requested stream $AudioStreamIndex is not E-AC-3" }
    } elseif ($eac3Streams.Count -eq 1) {
        $selectedStream = $eac3Streams[0]
    } elseif ($eac3Streams.Count -eq 0) {
        throw 'Container has no E-AC-3 audio stream; transcoding is forbidden'
    } else {
        throw 'Container has multiple E-AC-3 streams; specify -AudioStreamIndex'
    }
    $sidecar = Join-Path $resolvedOutput "$safeStem-native-copy.ec3"
    $prepArgs = @($resolvedInput, '--audio-stream-index', [string]$selectedStream.index,
                  '--dump-eac3', $sidecar, '--summary')
    if ($MaxAccessUnits -gt 0) { $prepArgs += @('--max-units', [string]$MaxAccessUnits) }
    $prep = Invoke-Captured $probe $prepArgs $prepLog
    if ($prep.ExitCode -ne 0 -or $prep.Text -notmatch '(?m)^dumpEac3=PASS ' -or
        -not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
        throw "Lossless E-AC-3 sidecar extraction failed; see $prepLog"
    }
    $rawInput = $sidecar
}

$spatialLog = Join-Path $resolvedOutput "$safeStem-spatial.txt"
$disableLfeFlag = $false
if ($DisableLfe) { $disableLfeFlag = $true }
$spatialArgs = @($rawInput, '--joc-gate7c', '--summary',
                 '--gate7c-queue-batches', [string]$QueueBatches,
                 '--gate7c-prebuffer-batches', [string]$PrebufferBatches,
                 '--gate7c-push-timeout-ms', [string]$PushTimeoutMs,
                 '--eac3-drc-scale', '0', '--eac3-cons-noisegen', '0')
if ($disableLfeFlag) { $spatialArgs += '--spatial-disable-lfe' }
$spatialArgs += @('--spatial-position-radius', $PositionRadiusMode)
$spatialArgs += @('--spatial-position-direction', $PositionDirectionMode)
$spatialArgs += @('--spatial-azimuth-focus', $AzimuthFocus.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
$spatialArgs += @('--spatial-renderer', $SpatialRenderer)
$spatialArgs += @('--spatial-hrtf-environment', $HrtfEnvironment)
if ($MaxAccessUnits -gt 0) { $spatialArgs += @('--max-units', [string]$MaxAccessUnits) }
$spatial = Invoke-Captured $probe $spatialArgs $spatialLog
$accessMatch = [regex]::Match($spatial.Text, '(?m)^accessUnits=(\d+)\r?$')
$accessUnits = if ($accessMatch.Success) { [long]$accessMatch.Groups[1].Value } else { $null }
$endpointMatch = [regex]::Match($spatial.Text, '(?m)^gate7cResult=(PASS|FAIL|INCONCLUSIVE)')
$endpointResult = if ($endpointMatch.Success) { $endpointMatch.Groups[1].Value } else { 'UNKNOWN' }
$gate6cMatch = [regex]::Match($spatial.Text, '(?m)^gate6cResult=(PASS|FAIL|INCONCLUSIVE)')
$gate6cResult = if ($gate6cMatch.Success) { $gate6cMatch.Groups[1].Value } else { 'UNKNOWN' }
$gate7bMatch = [regex]::Match($spatial.Text, '(?m)^gate7bResult=(PASS|FAIL|INCONCLUSIVE)')
$gate7bResult = if ($gate7bMatch.Success) { $gate7bMatch.Groups[1].Value } else { 'UNKNOWN' }
$lfeContract = Get-LfeProbeContract $spatial.Text
$lfePolicy = $lfeContract.Policy
$lfeVolume = $lfeContract.Volume
$expectedLfePolicy = if ($disableLfeFlag) { 'DISABLED_BY_USER' } else { 'PROGRAM_HEADROOM' }
$expectedLfeVolume = if ($disableLfeFlag) { 0.0 } else { 0.177827941 }
if ($endpointResult -ne 'PASS' -or $gate6cResult -ne 'PASS' -or $gate7bResult -ne 'PASS') {
    throw "Spatial submission did not PASS (gate6c=$gate6cResult gate7b=$gate7bResult gate7c=$endpointResult probeExitCode=$($spatial.ExitCode)); see $spatialLog"
}
if (($lfePolicy -ne $expectedLfePolicy) -or ($null -eq $lfeVolume) -or
    ([math]::Abs($lfeVolume - $expectedLfeVolume) -gt 0.000001)) {
    throw "Spatial LFE policy mismatch (expected=$expectedLfePolicy/$expectedLfeVolume actual=$lfePolicy/$lfeVolume); see $spatialLog"
}

$report = [ordered]@{
    schema = 'audioplayer.atmos-spatial-submission.v1'
    result = 'PASS_ENDPOINT_SUBMISSION_ONLY'
    input = $resolvedInput
    inputKind = $inputKind
    inputSha256 = Get-FileSha256 $resolvedInput
    selectedAudioStreamIndex = if ($selectedStream) { [int]$selectedStream.index } else { $null }
    sidecar = $sidecar
    sidecarPolicy = if ($sidecar) { 'PACKET_COPY_NO_TRANSCODE' } else { 'NOT_REQUIRED_RAW_INPUT' }
    maxAccessUnits = if ($MaxAccessUnits -gt 0) { $MaxAccessUnits } else { $null }
    accessUnits = $accessUnits
    queueBatches = $QueueBatches
    prebufferBatches = $PrebufferBatches
    pushTimeoutMs = $PushTimeoutMs
    endpointResult = $endpointResult
    gate6cResult = $gate6cResult
    gate7bResult = $gate7bResult
    probeExitCode = $spatial.ExitCode
    lfePolicy = $lfePolicy
    lfeVolume = $lfeVolume
    dynamicGainHeadroomDb = $lfeContract.DynamicHeadroomDb
    positionRadiusMode = $PositionRadiusMode
    positionDirectionMode = $PositionDirectionMode
    azimuthFocus = $AzimuthFocus
    spatialRenderer = $SpatialRenderer
    hrtfEnvironment = $HrtfEnvironment
    hrtfPolicy = if ($SpatialRenderer -eq 'hrtf') { 'DISABLE_LFE_ONLY_DIAGNOSTIC' } else { 'NOT_APPLICABLE_STANDARD' }
    staticObjectTypeMask = if ($SpatialRenderer -eq 'hrtf') { 'None' } else { 'LowFrequency' }
    hrtfObjectPolicy = if ($SpatialRenderer -eq 'hrtf') { '15_DYNAMIC_ONLY' } else { 'STANDARD_DYNAMIC_PLUS_STATIC_LFE' }
    preScalePolicy = if ($SpatialRenderer -eq 'hrtf') { 'EVALUATED_VOLUME_IN_PCM_BEFORE_GETBUFFER_SUBMISSION' } else { 'NATIVE_SET_VOLUME' }
    interfaceAvailable = Get-ProbeScalar $spatial.Text 'gate7cInterfaceAvailable'
    hrtfDistanceDecay = Get-ProbeText $spatial.Text 'gate7cHrtfDistanceDecay'
    preScaleSamples = Get-ProbeScalar $spatial.Text 'gate7cPreScaleSamples'
    preScaleRange = Get-ProbeText $spatial.Text 'gate7cPreScaleRange'
    inputDurationSeconds = if ($inputProbe.format.duration) { [double]$inputProbe.format.duration } else { $null }
    logs = @($prepLog, $spatialLog)
    note = if ($SpatialRenderer -eq 'hrtf') { '15 dynamic objects submitted to the HRTF Windows Spatial Audio endpoint; object PCM is pre-scaled by evaluated volume including the 15 dB headroom; static LFE is disabled by contract; no SetGain/CustomDecay; no file export or loopback proof.' } else { '15 dynamic objects plus static LFE submitted to the default Windows Spatial Audio endpoint; dynamicGainHeadroomDb is the positive headroom amount (15 dB), applied as -15 dB gain; LFE can be disabled by user without removing the static object; no file export or loopback proof.' }
}
$reportJson = $report | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($reportPath, $reportJson,
    (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
Write-Host 'playAtmosSpatial=PASS'
Write-Host "accessUnits=$accessUnits maxAccessUnits=$(if ($MaxAccessUnits -gt 0) { $MaxAccessUnits } else { 'FULL' })"
Write-Host 'evidenceLimit=endpoint-submission-only;manual-listening-or-loopback-required'
Write-Host "report=$reportPath"
