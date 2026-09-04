param(
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$QtPrefix = "",
    [string]$FfmpegAudioCoreRoot = "",
    [string]$DeployFfmpegExecutable = "",
    [string]$DeployFfprobeExecutable = ""
)

$ErrorActionPreference = "Stop"
$script:FfmpegVersion = "9.0.1"

function Resolve-FirstExistingPath {
    param(
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path $candidate -PathType Leaf)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ""
}

function Resolve-DirectoryPath {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    $expandedPath = [Environment]::ExpandEnvironmentVariables($Path.Trim())
    if ([System.IO.Path]::IsPathRooted($expandedPath)) {
        return [System.IO.Path]::GetFullPath($expandedPath)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $expandedPath))
}

function Assert-AudioCoreRuntime {
    param(
        [string]$Root
    )

    $requiredBinaries = @(
        "bin\ffmpeg.exe",
        "bin\ffprobe.exe"
    )

    $requiredHeaders = @(
        "include\libavformat\avformat.h"
    )

    $requiredLibraries = @(
        "avformat",
        "avcodec",
        "avutil",
        "swresample"
    )

    $missingFiles = @()

    foreach ($binary in $requiredBinaries) {
        $fullPath = Join-Path $Root $binary
        if (-not (Test-Path $fullPath -PathType Leaf)) {
            $missingFiles += $fullPath
        }
    }

    foreach ($header in $requiredHeaders) {
        $fullPath = Join-Path $Root $header
        if (-not (Test-Path $fullPath -PathType Leaf)) {
            $missingFiles += $fullPath
        }
    }

    foreach ($libName in $requiredLibraries) {
        $libPath = Join-Path $Root "lib\$libName.lib"
        $aPath = Join-Path $Root "lib\lib$libName.a"
        if (-not (Test-Path $libPath -PathType Leaf) -and -not (Test-Path $aPath -PathType Leaf)) {
            $missingFiles += "$libPath or $aPath"
        }
    }

    if ($missingFiles.Count -gt 0) {
        $missingText = $missingFiles -join [Environment]::NewLine
        throw "Self-built FFmpeg audio-core runtime is incomplete under $Root. Missing:$([Environment]::NewLine)$missingText$([Environment]::NewLine)Run scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild, or pass -FfmpegAudioCoreRoot pointing to a complete self-built runtime."
    }

    foreach ($toolName in @("ffmpeg", "ffprobe")) {
        $toolPath = Join-Path $Root "bin\$toolName.exe"
        $versionLine = [string](@(& $toolPath -version 2>$null) | Select-Object -First 1)
        if ($versionLine -notmatch "^(ffmpeg|ffprobe) version n?$([regex]::Escape($script:FfmpegVersion))(\s|$)") {
            throw "Self-built FFmpeg audio-core $toolName.exe must report version $script:FfmpegVersion, got: $versionLine"
        }
    }
}

function Test-QtPrefix {
    param(
        [string]$Candidate
    )

    if ([string]::IsNullOrWhiteSpace($Candidate)) {
        return $false
    }

    $expandedCandidate = [Environment]::ExpandEnvironmentVariables($Candidate.Trim())
    if (-not [System.IO.Path]::IsPathRooted($expandedCandidate)) {
        return $false
    }

    $qtConfig = Join-Path $expandedCandidate "lib\cmake\Qt6\Qt6Config.cmake"
    return Test-Path $qtConfig -PathType Leaf
}

function Split-CMakePrefixPath {
    param(
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return @()
    }

    return $Value -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}

function Get-PresetQtPrefix {
    param(
        [string]$RepoRoot
    )

    $presetPath = Join-Path $RepoRoot "CMakePresets.json"
    if (-not (Test-Path $presetPath -PathType Leaf)) {
        return ""
    }

    try {
        $presets = Get-Content -Path $presetPath -Raw | ConvertFrom-Json
        foreach ($preset in $presets.configurePresets) {
            $prefixPath = $preset.cacheVariables.CMAKE_PREFIX_PATH
            if (Test-QtPrefix -Candidate $prefixPath) {
                return $prefixPath
            }
        }
    } catch {
        Write-Warning "Unable to read Qt prefix from CMakePresets.json: $($_.Exception.Message)"
    }

    return ""
}

function Find-CommonQtPrefix {
    $roots = @("D:\Qt", "C:\Qt") | Where-Object { Test-Path $_ -PathType Container }
    foreach ($root in $roots) {
        $matches = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                Get-ChildItem -Path $_.FullName -Directory -Filter "msvc*_64" -ErrorAction SilentlyContinue
            } |
            Where-Object { Test-QtPrefix -Candidate $_.FullName } |
            Select-Object -First 1

        if ($null -ne $matches) {
            return $matches.FullName
        }
    }

    return ""
}

function Resolve-QtPrefix {
    param(
        [string]$RepoRoot,
        [string]$RequestedQtPrefix
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedQtPrefix)) {
        if (Test-QtPrefix -Candidate $RequestedQtPrefix) {
            return [System.IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($RequestedQtPrefix.Trim()))
        }
        throw "QtPrefix does not contain lib\cmake\Qt6\Qt6Config.cmake: $RequestedQtPrefix"
    }

    foreach ($candidate in (Split-CMakePrefixPath -Value ([System.Environment]::GetEnvironmentVariable("CMAKE_PREFIX_PATH")))) {
        if (Test-QtPrefix -Candidate $candidate) {
            return [System.IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($candidate.Trim()))
        }
    }

    $presetQtPrefix = Get-PresetQtPrefix -RepoRoot $RepoRoot
    if (Test-QtPrefix -Candidate $presetQtPrefix) {
        return [System.IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($presetQtPrefix.Trim()))
    }

    $commonQtPrefix = Find-CommonQtPrefix
    if (Test-QtPrefix -Candidate $commonQtPrefix) {
        return [System.IO.Path]::GetFullPath($commonQtPrefix)
    }

    throw "Unable to locate Qt 6. Set -QtPrefix, CMAKE_PREFIX_PATH, or update CMakePresets.json with a Qt MSVC prefix."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$resolvedBuildDir = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $BuildDir
Push-Location $repoRoot

try {
    if (-not [string]::IsNullOrWhiteSpace($DeployFfmpegExecutable) -or
        -not [string]::IsNullOrWhiteSpace($DeployFfprobeExecutable)) {
        throw "-DeployFfmpegExecutable and -DeployFfprobeExecutable are no longer supported by the default app build. Build or point -FfmpegAudioCoreRoot at the self-built runtime-with-ffprobe audio-core root instead."
    }

    $resolvedQtPrefix = Resolve-QtPrefix -RepoRoot $repoRoot -RequestedQtPrefix $QtPrefix
    $resolvedAudioCoreRoot = if ([string]::IsNullOrWhiteSpace($FfmpegAudioCoreRoot)) {
        Join-Path $resolvedBuildDir "ffmpeg-audio-core\runtime-with-ffprobe-msvc"
    } else {
        Resolve-DirectoryPath -RepoRoot $repoRoot -Path $FfmpegAudioCoreRoot
    }
    Assert-AudioCoreRuntime -Root $resolvedAudioCoreRoot
    $slimWithProbeDir = Join-Path $resolvedAudioCoreRoot "bin"

    $defaultSlimFfmpeg = Resolve-FirstExistingPath @(
        (Join-Path $slimWithProbeDir "ffmpeg.exe")
    )
    $defaultSlimFfprobe = Resolve-FirstExistingPath @(
        (Join-Path $slimWithProbeDir "ffprobe.exe")
    )

    $configureArgs = @(
        "-S", ".",
        "-B", $resolvedBuildDir,
        "-DCMAKE_PREFIX_PATH=$resolvedQtPrefix",
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=$resolvedAudioCoreRoot",
        "-DAUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=ON",
        "-DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=ON"
    )
    if (-not [string]::IsNullOrWhiteSpace($defaultSlimFfmpeg)) {
        $configureArgs += "-DAUDIOPLAYER_DEPLOY_FFMPEG_EXECUTABLE=$defaultSlimFfmpeg"
    } else {
        throw "Self-built ffmpeg.exe not found under $slimWithProbeDir. Run scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild."
    }
    if (-not [string]::IsNullOrWhiteSpace($defaultSlimFfprobe)) {
        $configureArgs += "-DAUDIOPLAYER_DEPLOY_FFPROBE_EXECUTABLE=$defaultSlimFfprobe"
    } else {
        throw "Self-built ffprobe.exe not found under $slimWithProbeDir. Run scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild."
    }

    $vsCmakePath = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    if (Test-Path $vsCmakePath) {
        $env:PATH = "$vsCmakePath;$env:PATH"
    }

    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE"
    }

    & cmake --build $resolvedBuildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }

    Write-Output "audioCoreRoot:$resolvedAudioCoreRoot"
    Write-Output "deployFfmpeg:$defaultSlimFfmpeg"
    Write-Output "deployFfprobe:$defaultSlimFfprobe"
    Write-Output "qtPrefix:$resolvedQtPrefix"

    $exePath = Resolve-AudioPlayerAppExePath `
        -RepoRoot $repoRoot `
        -BuildDir $resolvedBuildDir `
        -Configuration $Configuration
    if (Test-Path $exePath) {
        Write-Output "exe:$exePath"
    }
}
finally {
    Pop-Location
}
