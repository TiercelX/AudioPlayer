function Resolve-AudioPlayerBuildDir {
    param(
        [AllowEmptyString()]
        [string]$BuildDir
    )

    if (-not [string]::IsNullOrWhiteSpace($BuildDir)) {
        return $BuildDir
    }

    $environmentBuildDir = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_BUILD_DIR")
    if (-not [string]::IsNullOrWhiteSpace($environmentBuildDir)) {
        return $environmentBuildDir
    }

    return "build-mm"
}

function Resolve-AudioPlayerRepoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$Path,
        [AllowEmptyString()]
        [string]$DefaultPath = ""
    )

    $candidate = if ([string]::IsNullOrWhiteSpace($Path)) {
        $DefaultPath
    } else {
        $Path
    }

    if ([System.IO.Path]::IsPathRooted($candidate)) {
        return [System.IO.Path]::GetFullPath($candidate)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $candidate))
}

function Resolve-AudioPlayerCacheDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$BuildDir
    )

    $environmentCacheDir = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_CACHE_DIR")
    if (-not [string]::IsNullOrWhiteSpace($environmentCacheDir)) {
        return Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $environmentCacheDir
    }

    return Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path (Join-Path $BuildDir "cache")
}

function Resolve-AudioPlayerLogDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$BuildDir
    )

    $environmentLogDir = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_LOG_DIR")
    if (-not [string]::IsNullOrWhiteSpace($environmentLogDir)) {
        return Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $environmentLogDir
    }

    return Join-Path (Resolve-AudioPlayerCacheDir -RepoRoot $RepoRoot -BuildDir $BuildDir) "logs"
}

function Resolve-AudioPlayerCacheSubdir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return Join-Path (Resolve-AudioPlayerCacheDir -RepoRoot $RepoRoot -BuildDir $BuildDir) $Name
}

function Resolve-AudioPlayerLatestPlayableDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
    $playableRoot = Resolve-AudioPlayerRepoPath `
        -RepoRoot $RepoRoot `
        -Path (Join-Path (Join-Path $BuildDir "playable") $Configuration)
    $latestFile = Join-Path $playableRoot "LATEST.txt"
    if (Test-Path $latestFile -PathType Leaf) {
        $latestPath = (Get-Content -Path $latestFile -TotalCount 1).Trim()
        if (-not [string]::IsNullOrWhiteSpace($latestPath)) {
            $resolvedLatestPath = Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $latestPath
            if (Test-Path $resolvedLatestPath -PathType Container) {
                return $resolvedLatestPath
            }
        }
    }

    if (Test-Path $playableRoot -PathType Container) {
        $latestDirectory = Get-ChildItem -Path $playableRoot -Directory |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $latestDirectory) {
            return $latestDirectory.FullName
        }
    }

    return ""
}

function Test-AudioPlayerRuntimeDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    if (-not (Test-Path $Directory -PathType Container)) {
        return $false
    }

    return $null -ne (Get-ChildItem -Path $Directory -Filter "Qt6*.dll" -File -ErrorAction SilentlyContinue |
        Select-Object -First 1)
}

function Assert-AudioPlayerPlayableDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    if (-not (Test-AudioPlayerRuntimeDirectory -Directory $Directory)) {
        throw "AudioPlayer deployed bundle is missing Qt runtime DLLs: $Directory. Re-run scripts\build-app.ps1 -Configuration $Configuration."
    }

    foreach ($toolName in @("ffmpeg.exe", "ffprobe.exe")) {
        $toolPath = Join-Path $Directory $toolName
        if (-not (Test-Path $toolPath -PathType Leaf)) {
            throw "AudioPlayer deployed bundle is missing required self-built audio tool $toolName`: $Directory. Re-run scripts\build-app.ps1 -Configuration $Configuration after building the runtime-with-ffprobe audio core."
        }
    }
}

function Resolve-AudioPlayerAppExePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
    $playableDir = Resolve-AudioPlayerLatestPlayableDir `
        -RepoRoot $RepoRoot `
        -BuildDir $BuildDir `
        -Configuration $Configuration
    if (-not [string]::IsNullOrWhiteSpace($playableDir)) {
        $playableExe = Join-Path $playableDir "AudioPlayer.exe"
        if (Test-Path $playableExe -PathType Leaf) {
            Assert-AudioPlayerPlayableDirectory -Directory $playableDir -Configuration $Configuration
            return $playableExe
        }
    }

    $directExe = Resolve-AudioPlayerRepoPath `
        -RepoRoot $RepoRoot `
        -Path (Join-Path (Join-Path $BuildDir $Configuration) "AudioPlayer.exe")
    if ((Test-Path $directExe -PathType Leaf) -and
        -not (Test-AudioPlayerRuntimeDirectory -Directory (Split-Path -Parent $directExe))) {
        throw "AudioPlayer executable exists but its directory is missing Qt runtime DLLs: $directExe. Run scripts\build-app.ps1 -Configuration $Configuration to deploy a playable bundle under $BuildDir\playable\$Configuration."
    }

    return $directExe
}
