<#
.SYNOPSIS
Export a full-file stereo float32 WAV through the local Gate6C and official BEAR bundle path.
.EXAMPLE
  .\scripts\export-atmos-binaural.ps1 -InputPath media\03. iPad.m4a
#>
param(
    [string]$InputPath = "",
    [string]$OutputDir = "tmp\atmos-binaural",
    [ValidateRange(0, 1000000)] [int]$MaxAccessUnits = 0,
    [ValidateRange(-1, 128)] [int]$AudioStreamIndex = -1,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$BearPython = "",
    [string]$BearRoot = "",
    [string]$BearData = "tmp\reference\bear-main-6127e897\data\default_v1.1.tf",
    [switch]$Force,
    [switch]$NoBuild,
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

function Find-Directory([string[]]$Candidates) {
    $result = @()
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $path = Resolve-RepoPath $candidate
        if ((Test-Path -LiteralPath $path -PathType Container) -and $result -notcontains $path) {
            $result += $path
        }
    }
    return $result
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

function Invoke-Captured([string]$Executable, [string[]]$Arguments, [string]$LogPath,
                         [switch]$AllowDiagnosticFailure) {
    # PowerShell 7 materialises native stderr as an ErrorRecord. Keep the
    # command's diagnostics in the captured log and use its exit code below;
    # a harmless ffprobe warning must not abort the wrapper under Stop mode.
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $text = (& $Executable @Arguments 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $script:lastCapturedExitCode = $LASTEXITCODE
    [IO.File]::WriteAllText($LogPath, $text, (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
    if ($script:lastCapturedExitCode -ne 0 -and -not $AllowDiagnosticFailure) {
        throw "Command failed ($script:lastCapturedExitCode): $Executable; see $LogPath"
    }
    return $text
}

function Get-FfprobeJson([string]$Ffprobe, [string]$Path, [bool]$Raw) {
    $arguments = @('-v', 'error')
    if ($Raw) { $arguments += @('-f', 'eac3') }
    $arguments += @('-show_streams', '-show_format', '-of', 'json', $Path)
    $text = (& $Ffprobe @arguments 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for $Path`n$text" }
    return $text | ConvertFrom-Json
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "self-test: $Message" }
}

if ($SelfTest) {
    Assert-True ((Get-InputKind 'input.m4a') -eq 'container') 'm4a container detection'
    Assert-True ((Get-InputKind 'input.ec3') -eq 'raw-eac3') 'ec3 raw detection'
    Assert-True ((Get-SafeStem 'A file!.m4a') -eq 'A-file') 'safe stem'
    $caught = $false
    try { Get-InputKind 'input.wav' | Out-Null } catch { $caught = $true }
    Assert-True $caught 'unsupported extension rejection'
    $shaSelfTestPath = Join-Path ([IO.Path]::GetTempPath()) ('audioplayer-export-sha-{0}.bin' -f [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllBytes($shaSelfTestPath, [Text.Encoding]::ASCII.GetBytes('abc'))
        Assert-True ((Get-FileSha256 $shaSelfTestPath) -eq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'pure .NET SHA-256 implementation'
    } finally {
        if (Test-Path -LiteralPath $shaSelfTestPath) { Remove-Item -LiteralPath $shaSelfTestPath -Force }
    }
    Write-Host 'exportAtmosBinauralSelfTest=PASS cases=5'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputPath)) {
    throw 'InputPath is required unless -SelfTest is used'
}
$resolvedInput = Resolve-RepoPath $InputPath
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) {
    throw "Input file does not exist: $resolvedInput"
}
$inputKind = Get-InputKind $resolvedInput
$resolvedBuild = Resolve-RepoPath $BuildDir
$resolvedOutput = Resolve-RepoPath $OutputDir
$resolvedData = Resolve-RepoPath $BearData
if (-not (Test-Path -LiteralPath $resolvedData -PathType Leaf)) {
    throw "BEAR data file does not exist: $resolvedData"
}
if (-not $NoBuild) {
    & cmake --build $resolvedBuild --config $Configuration --target Eac3AccessUnitProbe -- /m:4
    if ($LASTEXITCODE -ne 0) { throw "Native Atmos probe build failed" }
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$provenancePath = Join-Path $resolvedOutput 'export-provenance.json'
if ((Test-Path -LiteralPath $provenancePath -PathType Leaf) -and -not $Force) {
    throw "Output directory already has export-provenance.json; choose a new -OutputDir or pass -Force"
}

$probe = Find-File @(
    (Join-Path $resolvedBuild "$Configuration\Eac3AccessUnitProbe.exe"),
    "build-mm\$Configuration\Eac3AccessUnitProbe.exe"
) 'Eac3AccessUnitProbe.exe'
$ffprobe = Find-File @(
    (Join-Path $resolvedBuild 'ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffprobe.exe'),
    ((Get-Command ffprobe.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source))
) 'ffprobe.exe'

$bearPythonPath = if ([string]::IsNullOrWhiteSpace($BearPython)) {
    Find-File @('tmp\reference\ear-2.1.0\venv\Scripts\python.exe') 'BEAR Python runtime'
} else { Find-File @($BearPython) 'BEAR Python runtime' }
$bearRootWasDefault = [string]::IsNullOrWhiteSpace($BearRoot)
$bearRootPath = if ($bearRootWasDefault) {
    Resolve-RepoPath 'tmp\reference\bear-main-6127e897'
} else { Resolve-RepoPath $BearRoot }
if (-not (Test-Path -LiteralPath $bearRootPath -PathType Container)) {
    throw "BEAR Python source directory does not exist: $bearRootPath"
}
$pythonPaths = Find-Directory @(
    $bearRootPath,
    'tmp\reference\ear-2.1.0\ebu_adm_renderer-2.1.0',
    'tmp\reference\VISR-install\python',
    'tmp\reference\bear-main-6127e897\build-visr-bear-6\python\Release',
    'tmp\reference\bear-git-build-shared\python\Release',
    'tools\atmos-render'
)
$dllDirs = Find-Directory @(
    'tmp\reference\bear-main-6127e897\build-visr-bear-6\python\Release',
    'tmp\reference\bear-install\bin',
    'tmp\reference\VISR-install\lib',
    'tmp\reference\VISR-install\3rd',
    'tmp\reference\VISR-0.13.0-build-shared\lib\Release',
    'tmp\reference\bear-git-build-shared\python\Release'
)
$safeStem = Get-SafeStem $resolvedInput
$bearSourceCommit = (& git -C $bearRootPath rev-parse HEAD 2>$null | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($bearSourceCommit)) { $bearSourceCommit = $null }
$bearSourceVersion = if ($bearSourceCommit -eq '6127e897b941211051c2ad135ee09b00be2e6ae0') { 'main@6127e897' } elseif ($bearSourceCommit) { "git@$bearSourceCommit" } else { 'explicit-source-without-git' }
$bearDataVersion = if ([IO.Path]::GetFileName($resolvedData) -match 'default_v1\.1') { 'default_v1.1' } elseif ([IO.Path]::GetFileName($resolvedData) -eq 'bear-default.tf') { 'default' } else { [IO.Path]::GetFileNameWithoutExtension($resolvedData) }
$bearImportPath = if ($bearRootWasDefault) { Join-Path $bearRootPath 'build-visr-bear-6\python\Release' } else { $bearRootPath }
$sidecar = $null
$rawInput = $resolvedInput
$inputProbe = Get-FfprobeJson $ffprobe $resolvedInput ($inputKind -eq 'raw-eac3')
$selectedStream = $null
$accessLog = Join-Path $resolvedOutput "$safeStem-access.txt"
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
    $copyArgs = @($resolvedInput, '--audio-stream-index', [string]$selectedStream.index,
                  '--dump-eac3', $sidecar, '--summary', '--eac3-drc-scale', '0',
                  '--eac3-cons-noisegen', '0')
    if ($MaxAccessUnits -gt 0) { $copyArgs += @('--max-units', [string]$MaxAccessUnits) }
    $copyText = Invoke-Captured $probe $copyArgs $accessLog
    if ($copyText -notmatch '(?m)^dumpEac3=PASS ' -or
        -not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
        throw "Lossless E-AC-3 sidecar extraction failed; see $accessLog"
    }
    $rawInput = $sidecar
}

$bundle = Join-Path $resolvedOutput 'bundle'
$nativeLog = Join-Path $resolvedOutput "$safeStem-native-bundle.txt"
$nativeArgs = @($rawInput, '--joc-gate6c', '--joc-bear-export', $bundle,
                '--summary', '--eac3-drc-scale', '0', '--eac3-cons-noisegen', '0')
if ($MaxAccessUnits -gt 0) { $nativeArgs += @('--max-units', [string]$MaxAccessUnits) }
$nativeText = Invoke-Captured $probe $nativeArgs $nativeLog -AllowDiagnosticFailure
$nativeExitCode = $script:lastCapturedExitCode
if ($nativeText -notmatch '(?m)^bearExport=PASS ') {
    throw "Gate6C/BEAR bundle export failed; see $nativeLog"
}
$accessMatch = [regex]::Match($nativeText, '(?m)^accessUnits=(\d+)\r?$')
if (-not $accessMatch.Success) { throw "Native bundle log has no accessUnits count; see $nativeLog" }
$accessUnits = [long]$accessMatch.Groups[1].Value
if ($MaxAccessUnits -gt 0 -and $accessUnits -ne $MaxAccessUnits) {
    throw "Native access-unit count $accessUnits does not match requested $MaxAccessUnits"
}

$bearLog = Join-Path $resolvedOutput "$safeStem-bear-render.txt"
$bearArgs = @(
    (Resolve-RepoPath 'tools\atmos-render\run_bear_montero_bundle.py'),
    $bundle, '--bear-python', $bearImportPath,
    '--bear-source', $bearRootPath, '--data', $resolvedData,
    '--output-dir', $resolvedOutput, '--full-file', '--output-stem', $safeStem,
    '--source-input', $resolvedInput
)
foreach ($path in $pythonPaths) { $bearArgs += @('--python-path', $path) }
foreach ($path in $dllDirs) { $bearArgs += @('--dll-dir', $path) }
$bearText = Invoke-Captured $bearPythonPath $bearArgs $bearLog
$bearExitCode = $script:lastCapturedExitCode
$bearProvenancePath = Join-Path $resolvedOutput 'provenance.json'
if (-not (Test-Path -LiteralPath $bearProvenancePath -PathType Leaf)) {
    throw "BEAR renderer did not produce provenance.json; see $bearLog"
}
$bearProvenance = Get-Content -LiteralPath $bearProvenancePath -Raw | ConvertFrom-Json
$rawOutput = [string]$bearProvenance.outputs[0].raw.path
if (-not (Test-Path -LiteralPath $rawOutput -PathType Leaf)) {
    throw "BEAR raw output is missing: $rawOutput"
}
$auditionOutput = [string]$bearProvenance.outputs[0].audition.path
if (-not (Test-Path -LiteralPath $auditionOutput -PathType Leaf)) {
    throw "BEAR audition output is missing: $auditionOutput"
}
$outputProbe = Get-FfprobeJson $ffprobe $rawOutput $false
$outputStream = @($outputProbe.streams | Where-Object { $_.codec_type -eq 'audio' })[0]
if ($null -eq $outputStream -or $outputStream.codec_name -ne 'pcm_f32le' -or
    [int]$outputStream.channels -ne 2 -or [int]$outputStream.sample_rate -ne 48000) {
    throw "BEAR output is not 48 kHz stereo float32 WAV: $rawOutput"
}
$auditionProbe = Get-FfprobeJson $ffprobe $auditionOutput $false
$auditionStream = @($auditionProbe.streams | Where-Object { $_.codec_type -eq 'audio' })[0]
if ($null -eq $auditionStream -or $auditionStream.codec_name -ne 'pcm_s24le' -or
    [int]$auditionStream.channels -ne 2 -or [int]$auditionStream.sample_rate -ne 48000) {
    throw "BEAR audition output is not 48 kHz stereo 24-bit PCM WAV: $auditionOutput"
}
$expectedFrames = [long]$bearProvenance.outputs[0].raw.frames
$auditionFrames = if ($null -ne $auditionStream.duration_ts) {
    [long]$auditionStream.duration_ts
} elseif ($null -ne $bearProvenance.outputs[0].audition.frames) {
    [long]$bearProvenance.outputs[0].audition.frames
} else { -1L }
if ($auditionFrames -ne $expectedFrames) {
    throw "BEAR audition frame count $auditionFrames does not match raw output $expectedFrames"
}

$report = [ordered]@{
    schema = 'audioplayer.atmos-binaural-export.v1'
    result = 'PASS_OFFICIAL_BEAR_FULL_FILE'
    input = $resolvedInput
    inputKind = $inputKind
    inputSha256 = Get-FileSha256 $resolvedInput
    selectedAudioStreamIndex = if ($selectedStream) { [int]$selectedStream.index } else { $null }
    sidecar = $sidecar
    sidecarPolicy = if ($sidecar) { 'PACKET_COPY_NO_TRANSCODE' } else { 'NOT_REQUIRED_RAW_INPUT' }
    maxAccessUnits = if ($MaxAccessUnits -gt 0) { $MaxAccessUnits } else { $null }
    accessUnits = $accessUnits
    nativeProbeExitCode = $nativeExitCode
    bearRendererExitCode = $bearExitCode
    bundle = (Resolve-RepoPath $bundle)
    bearPython = $bearPythonPath
    bearSourceVersion = $bearSourceVersion
    bearSourceCommit = $bearSourceCommit
    bearData = $resolvedData
    bearDataVersion = $bearDataVersion
    bearDataSha256 = Get-FileSha256 $resolvedData
    output = $rawOutput
    outputSha256 = Get-FileSha256 $rawOutput
    auditionOutput = $auditionOutput
    auditionSha256 = Get-FileSha256 $auditionOutput
    auditionCodec = [string]$auditionStream.codec_name
    auditionFrames = $auditionFrames
    inputDurationSeconds = if ($inputProbe.format.duration) { [double]$inputProbe.format.duration } else { $null }
    outputDurationSeconds = if ($outputProbe.format.duration) { [double]$outputProbe.format.duration } else { $null }
    outputFrames = if ($outputStream.duration_ts) { [long]$outputStream.duration_ts } else { [long]$bearProvenance.outputs[0].raw.frames }
    sampleRate = [int]$outputStream.sample_rate
    channels = [int]$outputStream.channels
    codec = [string]$outputStream.codec_name
    normalization = 'NO'
    algorithmicLatencySamples = [int]$bearProvenance.algorithmicLatencySamples
    fullFileInputFrames = [long]$bearProvenance.fullFileInputFrames
    fullFileOutputFrames = [long]$bearProvenance.fullFileOutputFrames
    lfeExcluded = [bool]$bearProvenance.lfeExcluded
    lfePolicy = [string]$bearProvenance.lfePolicy
    fixedAuditionGainDb = -2.0
    logs = @($accessLog, $nativeLog, $bearLog)
    bearProvenance = $bearProvenancePath
    note = 'Official BEAR bundle consumer; no Dolby-equivalence claim and no input ADM authoring claim.'
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $provenancePath -Encoding UTF8
Write-Host 'exportAtmosBinaural=PASS'
Write-Host "accessUnits=$accessUnits maxAccessUnits=$(if ($MaxAccessUnits -gt 0) { $MaxAccessUnits } else { 'FULL' })"
Write-Host 'lfeExcluded=True policy=bundle-LFE-parsed-for-validation-not-mixed-into-stereo-BEAR'
Write-Host "output=$rawOutput"
Write-Host "recommendedListeningOutput=$auditionOutput"
Write-Host "outputDurationSeconds=$($report.outputDurationSeconds)"
Write-Host "report=$provenancePath"
