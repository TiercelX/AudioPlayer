param(
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$QtPrefix = "",
    [string]$VcVars64Path = "",
    [string]$Msys2ShellPath = "",
    [string]$FfmpegSourceDir = "",
    [string]$FfmpegPrefixDir = "",
    [string]$FfmpegAudioCoreRoot = "",
    [string]$DeployFfmpegExecutable = "",
    [string]$DeployFfprobeExecutable = "",
    [string]$SmokeSource = "",
    [switch]$CheckOnly,
    [switch]$BuildFfmpeg,
    [switch]$BuildApp,
    [switch]$EnsureFixtures,
    [switch]$RunSmoke,
    [switch]$RunTests,
    [switch]$RunRegression,
    [switch]$ForceFfmpegRebuild
)

$ErrorActionPreference = "Stop"

function New-CheckResult {
    param(
        [string]$Name,
        [string]$Status,
        [string]$Detail
    )

    [pscustomobject]@{
        Name = $Name
        Status = $Status
        Detail = $Detail
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

    return Test-Path (Join-Path $expandedCandidate "lib\cmake\Qt6\Qt6Config.cmake") -PathType Leaf
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
            $candidate = $preset.cacheVariables.CMAKE_PREFIX_PATH
            if (Test-QtPrefix -Candidate $candidate) {
                return $candidate
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
        $match = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                Get-ChildItem -Path $_.FullName -Directory -Filter "msvc*_64" -ErrorAction SilentlyContinue
            } |
            Where-Object { Test-QtPrefix -Candidate $_.FullName } |
            Select-Object -First 1

        if ($null -ne $match) {
            return $match.FullName
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
        return ""
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

    return ""
}

function Resolve-VcVars64Path {
    param(
        [string]$RequestedPath
    )

    $candidates = [System.Collections.ArrayList]@(
        @(
            $RequestedPath,
            [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_VCVARS64_PATH")
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )

    $programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles")
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    $vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere -PathType Leaf) {
        foreach ($vswhereArgList in @(
            @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"),
            @("-latest", "-products", "*", "-property", "installationPath"),
            @("-latest", "-property", "installationPath"),
            @("-all", "-property", "installationPath")
        )) {
            $installationPath = & $vswhere @vswhereArgList
            if (-not [string]::IsNullOrWhiteSpace($installationPath)) {
                $candidates += Join-Path $installationPath "VC\Auxiliary\Build\vcvars64.bat"
                break
            }
        }
    }

    foreach ($root in @($programFiles, $programFilesX86, "D:\Program Files", "D:\Program Files (x86)")) {
        if ([string]::IsNullOrWhiteSpace($root)) {
            continue
        }
        $candidates += @(
            (Join-Path $root "Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"),
            (Join-Path $root "Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"),
            (Join-Path $root "Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"),
            (Join-Path $root "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
        )
    }

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path $candidate -PathType Leaf)) {
            return (Resolve-Path $candidate).Path
        }
    }

    foreach ($root in @($programFiles, $programFilesX86, "D:\Program Files", "D:\Program Files (x86)")) {
        if ([string]::IsNullOrWhiteSpace($root)) {
            continue
        }
        $visualStudioRoot = Join-Path $root "Microsoft Visual Studio"
        if (-not (Test-Path $visualStudioRoot -PathType Container)) {
            continue
        }
        $match = Get-ChildItem -Path $visualStudioRoot -Filter "vcvars64.bat" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) {
            return $match.FullName
        }
    }

    return ""
}

function Resolve-Msys2ShellPath {
    param(
        [string]$RequestedPath
    )

    $candidates = [System.Collections.ArrayList]@(
        @(
            $RequestedPath,
            [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_MSYS2_SHELL_PATH")
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )

    $msysRoot = [System.Environment]::GetEnvironmentVariable("MSYS2_ROOT")
    if (-not [string]::IsNullOrWhiteSpace($msysRoot)) {
        $candidates += Join-Path $msysRoot "msys2_shell.cmd"
    }
    $candidates += "C:\msys64\msys2_shell.cmd"

    $command = Get-Command msys2_shell.cmd -ErrorAction SilentlyContinue
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        $candidates += $command.Source
    }

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path $candidate -PathType Leaf)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ""
}

function Test-Msys2Command {
    param(
        [string]$Msys2Shell,
        [string]$CommandName
    )

    if ([string]::IsNullOrWhiteSpace($Msys2Shell)) {
        return $false
    }

    & $Msys2Shell -msys -defterm -no-start -use-full-path -here -shell bash -lc "command -v $CommandName >/dev/null"
    return $LASTEXITCODE -eq 0
}

function Get-CommandPath {
    param(
        [string]$Name
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command -or [string]::IsNullOrWhiteSpace($command.Source)) {
        return ""
    }

    return $command.Source
}

function Resolve-HostFfmpegPath {
    $override = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_FFMPEG_PATH")
    if (-not [string]::IsNullOrWhiteSpace($override)) {
        if ($override -ieq "disabled" -or $override -ieq "none") {
            return ""
        }
        if (Test-Path $override -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($override)
        }
    }

    return Get-CommandPath -Name "ffmpeg"
}

function Resolve-OptionalRepoPath {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $expandedPath = [Environment]::ExpandEnvironmentVariables($Path.Trim())
    if ([System.IO.Path]::IsPathRooted($expandedPath)) {
        return [System.IO.Path]::GetFullPath($expandedPath)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $expandedPath))
}

function Test-AudioCoreRuntimeFileSet {
    param(
        [string]$Root
    )

    $requiredFiles = @(
        "bin\ffmpeg.exe",
        "bin\ffprobe.exe",
        "include\libavformat\avformat.h",
        "lib\avformat.lib",
        "lib\avcodec.lib",
        "lib\avutil.lib",
        "lib\swresample.lib"
    )

    foreach ($relativePath in $requiredFiles) {
        if (-not (Test-Path (Join-Path $Root $relativePath) -PathType Leaf)) {
            return $false
        }
    }

    return $true
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$resolvedQtPrefix = Resolve-QtPrefix -RepoRoot $repoRoot -RequestedQtPrefix $QtPrefix
$resolvedVcVars64Path = Resolve-VcVars64Path -RequestedPath $VcVars64Path
$resolvedMsys2ShellPath = Resolve-Msys2ShellPath -RequestedPath $Msys2ShellPath
$requestedAudioCoreRoot = if (-not [string]::IsNullOrWhiteSpace($FfmpegAudioCoreRoot)) {
    $FfmpegAudioCoreRoot
} else {
    $FfmpegPrefixDir
}
$resolvedAudioCoreRoot = Resolve-OptionalRepoPath -RepoRoot $repoRoot -Path $requestedAudioCoreRoot
if ([string]::IsNullOrWhiteSpace($resolvedAudioCoreRoot)) {
    $resolvedAudioCoreRoot = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path (Join-Path $BuildDir "ffmpeg-audio-core\runtime-with-ffprobe-msvc")
}
$runtimeBin = Join-Path $resolvedAudioCoreRoot "bin"
$runtimeFfmpeg = Join-Path $runtimeBin "ffmpeg.exe"
$runtimeFfprobe = Join-Path $runtimeBin "ffprobe.exe"

$checks = @()
$gitPath = Get-CommandPath -Name "git"
$cmakePath = Get-CommandPath -Name "cmake"
$ffmpegPath = Resolve-HostFfmpegPath
$checks += New-CheckResult -Name "git" -Status $(if ($gitPath) { "PASS" } else { "MISSING" }) -Detail $(if ($gitPath) { $gitPath } else { "Install Git or put git.exe on PATH." })
$checks += New-CheckResult -Name "cmake" -Status $(if ($cmakePath) { "PASS" } else { "MISSING" }) -Detail $(if ($cmakePath) { $cmakePath } else { "Install CMake 3.19+ or put cmake.exe on PATH." })
$checks += New-CheckResult -Name "Qt prefix" -Status $(if ($resolvedQtPrefix) { "PASS" } else { "MISSING" }) -Detail $(if ($resolvedQtPrefix) { $resolvedQtPrefix } else { "Set -QtPrefix or CMAKE_PREFIX_PATH to a Qt MSVC prefix." })
$checks += New-CheckResult -Name "MSVC vcvars64" -Status $(if ($resolvedVcVars64Path) { "PASS" } else { "MISSING" }) -Detail $(if ($resolvedVcVars64Path) { $resolvedVcVars64Path } else { "Install Visual Studio C++ tools or set -VcVars64Path / AUDIOPLAYER_VCVARS64_PATH." })
$checks += New-CheckResult -Name "MSYS2 shell" -Status $(if ($resolvedMsys2ShellPath) { "PASS" } else { "MISSING" }) -Detail $(if ($resolvedMsys2ShellPath) { $resolvedMsys2ShellPath } else { "Install MSYS2 or set -Msys2ShellPath / AUDIOPLAYER_MSYS2_SHELL_PATH." })
foreach ($toolName in @("make", "nasm", "pkgconf")) {
    $available = Test-Msys2Command -Msys2Shell $resolvedMsys2ShellPath -CommandName $toolName
    $checks += New-CheckResult -Name "MSYS2 $toolName" -Status $(if ($available) { "PASS" } else { "MISSING" }) -Detail $(if ($available) { "Available in MSYS2." } else { "Install $toolName in MSYS2 before building FFmpeg." })
}
$checks += New-CheckResult -Name "host ffmpeg" -Status $(if ($ffmpegPath) { "PASS" } else { "MISSING" }) -Detail $(if ($ffmpegPath) { $ffmpegPath } else { "Needed for fixture generation unless AUDIOPLAYER_FFMPEG_PATH points to ffmpeg.exe." })
$checks += New-CheckResult -Name "audio-core ffmpeg" -Status $(if (Test-Path $runtimeFfmpeg -PathType Leaf) { "PASS" } else { "MISSING" }) -Detail $runtimeFfmpeg
$checks += New-CheckResult -Name "audio-core ffprobe" -Status $(if (Test-Path $runtimeFfprobe -PathType Leaf) { "PASS" } else { "MISSING" }) -Detail $runtimeFfprobe
$checks += New-CheckResult -Name "audio-core libav" -Status $(if (Test-AudioCoreRuntimeFileSet -Root $resolvedAudioCoreRoot) { "PASS" } else { "MISSING" }) -Detail $resolvedAudioCoreRoot

Write-Output "AudioPlayer development environment check:"
$checks | Format-Table -AutoSize | Out-String -Width 220 | Write-Output

if ($CheckOnly) {
    return
}

function Assert-CheckPassed {
    param(
        [string[]]$Names
    )

    $failed = $checks | Where-Object { $Names -contains $_.Name -and $_.Status -ne "PASS" }
    if ($failed.Count -gt 0) {
        $summary = ($failed | ForEach-Object { "$($_.Name): $($_.Detail)" }) -join [Environment]::NewLine
        throw "Missing required development environment prerequisites:$([Environment]::NewLine)$summary"
    }
}

if ($BuildFfmpeg) {
    Assert-CheckPassed -Names @("cmake", "MSVC vcvars64", "MSYS2 shell", "MSYS2 make", "MSYS2 nasm", "MSYS2 pkgconf")
    $ffmpegArgs = @{
        Profile = "runtime-with-ffprobe"
        Toolchain = "msvc"
        RunBuild = $true
        VcVars64Path = $resolvedVcVars64Path
        Msys2ShellPath = $resolvedMsys2ShellPath
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegSourceDir)) {
        $ffmpegArgs.SourceDir = $FfmpegSourceDir
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegPrefixDir)) {
        $ffmpegArgs.PrefixDir = $FfmpegPrefixDir
    }
    if ($ForceFfmpegRebuild) {
        $ffmpegArgs.ForceRebuild = $true
    }
    & (Join-Path $PSScriptRoot "build-ffmpeg-audio-core.ps1") @ffmpegArgs
    if ($LASTEXITCODE -ne 0) {
        throw "build-ffmpeg-audio-core.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ($BuildApp) {
    Assert-CheckPassed -Names @("cmake", "Qt prefix")
    if (-not [string]::IsNullOrWhiteSpace($DeployFfmpegExecutable) -or
        -not [string]::IsNullOrWhiteSpace($DeployFfprobeExecutable)) {
        throw "-DeployFfmpegExecutable and -DeployFfprobeExecutable are no longer supported by the default app build. Use -FfmpegAudioCoreRoot or build the runtime-with-ffprobe audio core under the build directory."
    }
    $buildArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        QtPrefix = $resolvedQtPrefix
        FfmpegAudioCoreRoot = $resolvedAudioCoreRoot
    }
    & (Join-Path $PSScriptRoot "build-app.ps1") @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "build-app.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ($EnsureFixtures) {
    Assert-CheckPassed -Names @("host ffmpeg")
    & (Join-Path $PSScriptRoot "ensure-playback-fixtures.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "ensure-playback-fixtures.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ($RunTests) {
    Assert-CheckPassed -Names @("cmake", "Qt prefix")
    $testArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        QtPrefix = $resolvedQtPrefix
    }
    & (Join-Path $PSScriptRoot "run-tests.ps1") @testArgs
    if ($LASTEXITCODE -ne 0) {
        throw "run-tests.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ($RunSmoke) {
    $sourcePath = $SmokeSource
    if ([string]::IsNullOrWhiteSpace($sourcePath)) {
        $sourcePath = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path (Join-Path $BuildDir "fixtures\smoke.wav")
    }
    if (-not (Test-Path $sourcePath -PathType Leaf)) {
        throw "Smoke source not found: $sourcePath. Run with -EnsureFixtures or pass -SmokeSource."
    }
    & (Join-Path $PSScriptRoot "run-playback-smoke.ps1") `
        -Source $sourcePath `
        -BuildDir $BuildDir `
        -Configuration $Configuration `
        -RequirePlaying `
        -RejectPlaybackErrors
    if ($LASTEXITCODE -ne 0) {
        throw "run-playback-smoke.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ($RunRegression) {
    & (Join-Path $PSScriptRoot "run-playback-regression.ps1") `
        -BuildDir $BuildDir `
        -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "run-playback-regression.ps1 failed with exit code $LASTEXITCODE"
    }
}
