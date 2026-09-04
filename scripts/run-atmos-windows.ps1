<#
.SYNOPSIS
  Safe single-entry Windows helper for Atmos validation, BEAR export, or
  Windows Spatial Audio endpoint submission.

.DESCRIPTION
  Validate is the default and never submits audio. Render delegates to the
  existing offline BEAR export wrapper. Spatial delegates to the existing
  Gate7C endpoint-submission wrapper and does not use BEAR TensorFiles.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$InputPath = "",
    [ValidateSet("Validate", "Render", "Spatial")]
    [string]$Mode = "Validate",
    [ValidateSet("OfficialMain", "SystemHV6")]
    [string]$BearProfile = "OfficialMain",
    [string]$OutputRoot = "tmp\windows-atmos",
    [ValidateRange(0, 1000000)]
    [int]$MaxAccessUnits = 0,
    [ValidateRange(-1, 128)]
    [int]$AudioStreamIndex = -1,
    [string]$BuildDir = "build-mm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$NoBuild,
    [switch]$Force,
    [switch]$OpenOutput,
    [switch]$DisableLfe,
    [ValidateSet("standard", "hrtf")]
    [string]$SpatialRenderer = "standard",
    [ValidateSet("small", "outdoors")]
    [string]$HrtfEnvironment = "small",
    [ValidateRange(0.0, 1.0)]
    [double]$AzimuthFocus = 0.0,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
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

function Get-SafeStem([string]$Path) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($Path) -replace '[^A-Za-z0-9._-]+', '-'
    $stem = $stem.Trim('-')
    if ([string]::IsNullOrWhiteSpace($stem)) { return 'atmos-input' }
    return $stem
}

function Get-InputKind([string]$Path) {
    $extension = [IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($extension -in @('.m4a', '.mp4', '.mka', '.mkv')) { return 'container' }
    if ($extension -in @('.eb3', '.ec3', '.eac3')) { return 'raw-eac3' }
    throw "Unsupported input extension '$extension'; expected .m4a/.mp4/.mka/.mkv/.eb3/.ec3/.eac3 (ADM WAV is not accepted by this entry)"
}

function Select-AtmosInput {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select an Atmos E-AC-3 file'
    $dialog.Filter = 'Atmos E-AC-3|*.m4a;*.mp4;*.mka;*.mkv;*.eb3;*.ec3;*.eac3'
    $dialog.Multiselect = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw 'No input file selected'
    }
    return $dialog.FileName
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "self-test: $Message" }
}

function Get-UniqueOutputDirectory([string]$Root, [string]$Stem, [string]$SelectedMode) {
    $rootPath = Resolve-RepoPath $Root
    New-Item -ItemType Directory -Force -Path $rootPath | Out-Null
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
    $base = Join-Path $rootPath ("{0}-{1}-{2}" -f $Stem, $SelectedMode.ToLowerInvariant(), $stamp)
    $candidate = $base
    $suffix = 0
    while (Test-Path -LiteralPath $candidate) {
        $suffix++
        $candidate = "{0}-{1}" -f $base, $suffix
    }
    New-Item -ItemType Directory -Path $candidate | Out-Null
    return $candidate
}

function Get-NativeProbe([string]$ResolvedBuild, [string]$SelectedConfiguration) {
    $candidates = @(
        (Join-Path $ResolvedBuild "$SelectedConfiguration\Eac3AccessUnitProbe.exe"),
        (Resolve-RepoPath "build-mm\$SelectedConfiguration\Eac3AccessUnitProbe.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

function Get-Ffprobe {
    $command = Get-Command ffprobe.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $candidate = Resolve-RepoPath "build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffprobe.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    return $null
}

function Get-PowerShellExecutable {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pwsh) { return $pwsh.Source }
    $powershell = Get-Command powershell.exe -ErrorAction Stop | Select-Object -First 1
    return $powershell.Source
}

function Get-FfprobeJson([string]$Ffprobe, [string]$Path, [bool]$Raw) {
    $arguments = @('-v', 'error')
    if ($Raw) { $arguments += @('-f', 'eac3') }
    $arguments += @('-show_streams', '-show_format', '-of', 'json', $Path)
    $text = (& $Ffprobe @arguments 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed: $text" }
    return $text | ConvertFrom-Json
}

function Get-Profile([string]$SelectedProfile) {
    $source = Resolve-RepoPath 'tmp\reference\bear-main-6127e897'
    $officialData = Resolve-RepoPath 'tmp\reference\bear-main-6127e897\data\default_v1.1.tf'
    if ($SelectedProfile -eq 'OfficialMain') {
        return [pscustomobject]@{
            name = 'OfficialMain'
            source = $source
            sourceCommit = '6127e897b941211051c2ad135ee09b00be2e6ae0'
            data = $officialData
            dataSha256 = '171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8'
            python = $null
            runtimePolicy = 'export-wrapper-default-ear-2.1.0-venv-and-main-build-visr-import'
            topology = 'official default_v1.1'
            limitation = 'Offline BEAR export only; no Dolby-equivalence claim'
        }
    }
    return [pscustomobject]@{
        name = 'SystemHV6'
        source = $source
        sourceCommit = '6127e897b941211051c2ad135ee09b00be2e6ae0'
        data = Resolve-RepoPath 'tmp\reference\bear-main-6127e897\ir-processing-system-h\v6\layout-9+10+3\system-h-22-v6.tf'
        dataSha256 = '8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9'
        python = $null
        runtimePolicy = 'export-wrapper-default-ear-2.1.0-venv-and-main-build-visr-import; py312 IR environment is historical only'
        topology = 'System H 22ch explicit 9+10+3; missing B+135/B-135'
        limitation = 'Experimental offline TensorFile; not the default 24ch layout and not a Windows Spatial input'
    }
}

function Assert-Profile([object]$Profile) {
    Assert-True (Test-Path -LiteralPath $Profile.source -PathType Container) "BEAR source missing: $($Profile.source)"
    $actualCommit = (& git -C $Profile.source rev-parse HEAD 2>$null | Out-String).Trim()
    Assert-True ($actualCommit -eq $Profile.sourceCommit) "BEAR source commit mismatch: expected $($Profile.sourceCommit), got $actualCommit"
    Assert-True (Test-Path -LiteralPath $Profile.data -PathType Leaf) "BEAR data missing: $($Profile.data)"
    $actual = Get-FileSha256 $Profile.data
    Assert-True ($actual -eq $Profile.dataSha256) "BEAR data SHA-256 mismatch: expected $($Profile.dataSha256), got $actual"
}

function New-RenderDelegateArgs([object]$Profile, [string]$ResolvedInput,
                                [string]$OutputDirectory, [string]$ResolvedBuild,
                                [string]$SelectedConfiguration) {
    $resultArgs = @('-InputPath', $ResolvedInput, '-OutputDir', $OutputDirectory,
                    '-BuildDir', $ResolvedBuild, '-Configuration', $SelectedConfiguration,
                    '-BearData', $Profile.data)
    # Keep the pinned main source implicit for both profiles.  The export
    # wrapper then selects its verified build-visr-bear-6 import directory.
    # SystemHV6 changes only the TensorFile; it does not use a second binding.
    return $resultArgs
}

function New-SpatialDelegateArgs([string]$ResolvedInput, [string]$OutputDirectory,
                                 [string]$ResolvedBuild, [string]$SelectedConfiguration,
                                 [int]$SelectedAudioStreamIndex, [switch]$NoBuildSwitch,
                                 [switch]$ForceSwitch, [switch]$DisableLfeSwitch,
                                 [double]$Focus = 0.0,
                                 [string]$Renderer = "standard",
                                 [string]$Environment = "small") {
    $resultArgs = @('-InputPath', $ResolvedInput, '-OutputDir', $OutputDirectory,
                    '-BuildDir', $ResolvedBuild, '-Configuration', $SelectedConfiguration)
    if ($SelectedAudioStreamIndex -ge 0) { $resultArgs += @('-AudioStreamIndex', [string]$SelectedAudioStreamIndex) }
    if ($NoBuildSwitch) { $resultArgs += '-NoBuild' }
    if ($ForceSwitch) { $resultArgs += '-Force' }
    if ($DisableLfeSwitch) { $resultArgs += '-DisableLfe' }
    $resultArgs += @('-AzimuthFocus', $Focus.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
    $resultArgs += @('-SpatialRenderer', $Renderer, '-HrtfEnvironment', $Environment)
    return $resultArgs
}

function Invoke-Validate([string]$ResolvedInput, [string]$InputKind, [string]$OutputDirectory,
                          [object]$Profile, [string]$ResolvedBuild, [string]$SelectedConfiguration,
                          [int]$SelectedAudioStreamIndex) {
    $probe = Get-NativeProbe $ResolvedBuild $SelectedConfiguration
    $ffprobe = Get-Ffprobe
    $sourceExists = Test-Path -LiteralPath $Profile.source -PathType Container
    $dataExists = Test-Path -LiteralPath $Profile.data -PathType Leaf
    $dataSha = if ($dataExists) { Get-FileSha256 $Profile.data } else { $null }
    $inputProbe = $null
    $inputProbeError = $null
    $inputContentResult = 'INCONCLUSIVE_FFPROBE_UNAVAILABLE'
    $eac3Streams = @()
    if ($ffprobe) {
        try {
            $inputProbe = Get-FfprobeJson $ffprobe $ResolvedInput ($InputKind -eq 'raw-eac3')
            $eac3Streams = @($inputProbe.streams | Where-Object {
                $_.codec_type -eq 'audio' -and $_.codec_name -eq 'eac3'
            })
            if ($InputKind -eq 'raw-eac3') {
                $inputContentResult = if ($eac3Streams.Count -ge 1) { 'PASS_EAC3_DETECTED' } else { 'FAIL_NO_EAC3' }
            } elseif ($SelectedAudioStreamIndex -ge 0) {
                $selected = @($eac3Streams | Where-Object { [int]$_.index -eq $SelectedAudioStreamIndex })[0]
                $inputContentResult = if ($selected) { 'PASS_SELECTED_EAC3' } else { 'FAIL_SELECTED_STREAM_NOT_EAC3' }
            } elseif ($eac3Streams.Count -eq 1) {
                $inputContentResult = 'PASS_EAC3_DETECTED'
            } elseif ($eac3Streams.Count -eq 0) {
                $inputContentResult = 'FAIL_NO_EAC3'
            } else {
                $inputContentResult = 'INCONCLUSIVE_MULTIPLE_EAC3_STREAMS'
            }
        } catch {
            $inputProbeError = $_.Exception.Message
            $inputContentResult = 'FAIL_FFPROBE'
        }
    }
    $report = [ordered]@{
        schema = 'audioplayer.atmos-windows-validation.v1'
        result = if ($probe -and $ffprobe -and $sourceExists -and $dataExists -and $dataSha -eq $Profile.dataSha256 -and $inputContentResult -like 'PASS_*') { 'PASS_DEPENDENCY_DRY_CHECK' } else { 'INCONCLUSIVE_DEPENDENCY_CHECK' }
        mode = 'Validate'
        input = $ResolvedInput
        inputKind = $InputKind
        inputSha256 = Get-FileSha256 $ResolvedInput
        selectedAudioStreamIndex = if ($SelectedAudioStreamIndex -ge 0) { $SelectedAudioStreamIndex } else { $null }
        inputContentResult = $inputContentResult
        inputContentError = $inputProbeError
        eac3StreamCount = $eac3Streams.Count
        buildDir = $ResolvedBuild
        configuration = $SelectedConfiguration
        eac3AccessUnitProbe = $probe
        ffprobe = $ffprobe
        bearProfile = $Profile.name
        bearSource = $Profile.source
        bearSourceCommit = $Profile.sourceCommit
        bearData = $Profile.data
        bearDataSha256 = $dataSha
        bearDataSha256Expected = $Profile.dataSha256
        runtimePolicy = $Profile.runtimePolicy
        topology = $Profile.topology
        endpointCheck = 'DRY_ONLY_NOT_PROBED'
        endpointSubmission = $false
        audioSubmitted = $false
        note = 'No Eac3AccessUnitProbe, SpatialDynamicProbe, or audio submission was invoked by Validate.'
    }
    $path = Join-Path $OutputDirectory 'validation-provenance.json'
    # Windows PowerShell 5.1 is the cmd launcher default; UTF8 is portable there.
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Host "runAtmosWindows=VALIDATE result=$($report.result)"
    Write-Host "report=$path"
    return $report.result
}

if ($SelfTest) {
    Assert-True ((Get-InputKind 'input.m4a') -eq 'container') 'm4a extension'
    Assert-True ((Get-InputKind 'input.eb3') -eq 'raw-eac3') 'eb3 extension'
    Assert-True ((Get-InputKind 'input.eac3') -eq 'raw-eac3') 'eac3 extension'
    $caught = $false
    try { Get-InputKind 'input.wav' | Out-Null } catch { $caught = $true }
    Assert-True $caught 'ADM WAV rejection'
    Assert-True ((Get-Profile 'OfficialMain').dataSha256.Length -eq 64) 'OfficialMain data contract'
    $v6 = Get-Profile 'SystemHV6'
    Assert-True ($v6.topology -match '22ch') 'SystemHV6 topology contract'
    Assert-True ($v6.topology -match 'B\+135/B-135') 'SystemHV6 missing emitter contract'
    Assert-True ($null -eq $v6.python) 'SystemHV6 does not pin unused py312 runtime'
    Assert-True ($v6.runtimePolicy -match 'export-wrapper-default') 'SystemHV6 runtime policy contract'
    $shaSelfTestPath = Join-Path $repoRoot ('tmp\windows-atmos-selftest-sha-{0}.bin' -f [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllBytes($shaSelfTestPath, [Text.Encoding]::ASCII.GetBytes('abc'))
        Assert-True ((Get-FileSha256 $shaSelfTestPath) -eq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'pure .NET SHA-256 implementation'
    } finally {
        if (Test-Path -LiteralPath $shaSelfTestPath) { Remove-Item -LiteralPath $shaSelfTestPath -Force }
    }
    $renderArgs = New-RenderDelegateArgs $v6 'input.ec3' 'output' 'build-mm' 'Release'
    Assert-True ($renderArgs -contains '-BearData') 'Render delegate data argument'
    Assert-True (-not ($renderArgs -contains '-BearRoot')) 'SystemHV6 uses default source import'
    Assert-True (-not ($renderArgs -contains '-BearPython')) 'SystemHV6 uses default verified binding runtime'
    $spatialDefaultArgs = New-SpatialDelegateArgs 'input.ec3' 'output' 'build-mm' 'Release' -1 -NoBuildSwitch:$false -ForceSwitch:$false -DisableLfeSwitch:$false
    $spatialDisabledArgs = New-SpatialDelegateArgs 'input.ec3' 'output' 'build-mm' 'Release' -1 -NoBuildSwitch:$false -ForceSwitch:$false -DisableLfeSwitch:$true
    $spatialFocusArgs = New-SpatialDelegateArgs 'input.ec3' 'output' 'build-mm' 'Release' -1 -NoBuildSwitch:$false -ForceSwitch:$false -DisableLfeSwitch:$false -Focus 0.5
    $spatialHrtfArgs = New-SpatialDelegateArgs 'input.ec3' 'output' 'build-mm' 'Release' -1 -NoBuildSwitch:$false -ForceSwitch:$false -DisableLfeSwitch:$true -Renderer 'hrtf' -Environment 'outdoors'
    Assert-True (-not ($spatialDefaultArgs -contains '-DisableLfe')) 'default Spatial LFE policy'
    Assert-True ($spatialDisabledArgs -contains '-DisableLfe') 'disabled Spatial LFE delegate flag'
    Assert-True (($spatialFocusArgs -contains '-AzimuthFocus') -and ($spatialFocusArgs -contains '0.5')) 'Spatial azimuth-focus delegate flag'
    Assert-True (($spatialHrtfArgs -contains '-SpatialRenderer') -and ($spatialHrtfArgs -contains 'hrtf') -and ($spatialHrtfArgs -contains '-HrtfEnvironment') -and ($spatialHrtfArgs -contains 'outdoors')) 'Spatial HRTF delegate flags'
    Assert-True (Test-Path -LiteralPath (Resolve-RepoPath 'scripts\export-atmos-binaural.ps1') -PathType Leaf) 'Render delegate exists'
    Assert-True (Test-Path -LiteralPath (Resolve-RepoPath 'scripts\play-atmos-spatial.ps1') -PathType Leaf) 'Spatial delegate exists'
    Assert-True ((Get-UniqueOutputDirectory 'tmp\windows-atmos-selftest' 'selftest' 'Validate') -match 'selftest-validate-') 'unique output path'
    Write-Host 'runAtmosWindowsSelfTest=PASS'
    exit 0
}

if ($Mode -ne 'Render' -and $BearProfile -ne 'OfficialMain') {
    throw 'BearProfile=SystemHV6 is only valid with -Mode Render'
}
if ($DisableLfe -and $Mode -ne 'Spatial') {
    throw 'DisableLfe is only valid with -Mode Spatial'
}
if ($SpatialRenderer -eq 'hrtf' -and $Mode -ne 'Spatial') {
    throw 'SpatialRenderer hrtf is only valid with -Mode Spatial'
}
if ($Mode -eq 'Spatial' -and $SpatialRenderer -eq 'hrtf' -and -not $DisableLfe) {
    throw 'SpatialRenderer hrtf requires -DisableLfe; HRTF backend does not submit a static LFE object'
}
if ([string]::IsNullOrWhiteSpace($InputPath)) {
    $InputPath = Select-AtmosInput
}
$resolvedInput = Resolve-RepoPath $InputPath
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) {
    throw "Input file does not exist: $resolvedInput"
}
$inputKind = Get-InputKind $resolvedInput
$resolvedBuild = Resolve-RepoPath $BuildDir
$profile = $null
if ($Mode -ne 'Spatial') {
    $profile = Get-Profile $BearProfile
    Assert-Profile $profile
}
$outputDirectory = Get-UniqueOutputDirectory $OutputRoot (Get-SafeStem $resolvedInput) $Mode

if ($Mode -eq 'Validate') {
    $result = Invoke-Validate $resolvedInput $inputKind $outputDirectory $profile $resolvedBuild $Configuration $AudioStreamIndex
    if ($result -ne 'PASS_DEPENDENCY_DRY_CHECK') { exit 1 }
    exit 0
}

if ($Mode -eq 'Render') {
    $delegate = Resolve-RepoPath 'scripts\export-atmos-binaural.ps1'
    $delegateArgs = New-RenderDelegateArgs $profile $resolvedInput $outputDirectory $resolvedBuild $Configuration
    if ($MaxAccessUnits -gt 0) { $delegateArgs += @('-MaxAccessUnits', [string]$MaxAccessUnits) }
    if ($AudioStreamIndex -ge 0) { $delegateArgs += @('-AudioStreamIndex', [string]$AudioStreamIndex) }
    if ($NoBuild) { $delegateArgs += '-NoBuild' }
    if ($Force) { $delegateArgs += '-Force' }
    $powershell = Get-PowerShellExecutable
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $delegate @delegateArgs
    if ($LASTEXITCODE -ne 0) { throw "Render delegate failed with exit code $LASTEXITCODE" }
    if ($OpenOutput) {
        Start-Process -FilePath explorer.exe -ArgumentList ('"{0}"' -f $outputDirectory)
    }
    Write-Host "runAtmosWindows=RENDER profile=$BearProfile"
    Write-Host "outputDirectory=$outputDirectory"
    exit 0
}

if ($Mode -eq 'Spatial') {
    $delegate = Resolve-RepoPath 'scripts\play-atmos-spatial.ps1'
    $delegateArgs = New-SpatialDelegateArgs $resolvedInput $outputDirectory $resolvedBuild $Configuration $AudioStreamIndex -NoBuildSwitch:$NoBuild -ForceSwitch:$Force -DisableLfeSwitch:$DisableLfe -Focus:$AzimuthFocus -Renderer:$SpatialRenderer -Environment:$HrtfEnvironment
    if ($MaxAccessUnits -gt 0) { $delegateArgs += @('-MaxAccessUnits', [string]$MaxAccessUnits) }
    $powershell = Get-PowerShellExecutable
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $delegate @delegateArgs
    if ($LASTEXITCODE -ne 0) { throw "Spatial delegate failed with exit code $LASTEXITCODE" }
    if ($OpenOutput) {
        Start-Process -FilePath explorer.exe -ArgumentList ('"{0}"' -f $outputDirectory)
    }
    Write-Host 'runAtmosWindows=SPATIAL'
    Write-Host "outputDirectory=$outputDirectory"
    Write-Host 'note=real Windows ISpatialAudioObjectRenderStream endpoint submission; non-BEAR; no audio file output'
    exit 0
}

throw "Unsupported mode: $Mode"
