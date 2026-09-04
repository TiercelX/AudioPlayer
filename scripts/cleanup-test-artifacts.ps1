if (-not (Get-Command Resolve-AudioPlayerBuildDir -ErrorAction SilentlyContinue)) {
    . (Join-Path $PSScriptRoot "common-paths.ps1")
}

function Invoke-TestArtifactRetention {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [string]$BuildDir = "",
        [ValidateRange(1, 100000)]
        [int]$KeepRuns = 20,
        [string[]]$PreserveRunTokens = @(),
        [switch]$NoCleanup
    )

    $summary = [ordered]@{
        removedRuns = 0
        removedFiles = 0
        archivedRuns = 0
        archivedFiles = 0
        keptRuns = 0
        archiveRoot = ""
        archiveManifest = ""
        archiveErrors = @()
    }

    $BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
    $logDir = Resolve-AudioPlayerLogDir -RepoRoot $RepoRoot -BuildDir $BuildDir
    $loopbackDir = Resolve-AudioPlayerCacheSubdir -RepoRoot $RepoRoot -BuildDir $BuildDir -Name "loopback"
    $archiveRoot = Resolve-AudioPlayerCacheSubdir -RepoRoot $RepoRoot -BuildDir $BuildDir -Name "test-artifact-archive"
    $archiveManifest = Join-Path $archiveRoot "manifest.jsonl"
    $summary.archiveRoot = $archiveRoot
    $summary.archiveManifest = $archiveManifest
    $groups = @{}

    function ConvertTo-SafeArchiveName {
        param([string]$Name)

        $safeName = if ([string]::IsNullOrWhiteSpace($Name)) {
            "unknown-run"
        } else {
            $Name -replace '[^A-Za-z0-9_.-]', '_'
        }
        if ([string]::IsNullOrWhiteSpace($safeName)) {
            return "unknown-run"
        }
        return $safeName
    }

    function Test-IsChildPath {
        param(
            [string]$Path,
            [string]$Root
        )

        if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Root)) {
            return $false
        }
        $fullPath = [System.IO.Path]::GetFullPath($Path)
        $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
        return $fullPath.StartsWith($fullRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
    }

    function Get-ArtifactArea {
        param([string]$Path)

        if (Test-IsChildPath -Path $Path -Root $logDir) {
            return "logs"
        }
        if (Test-IsChildPath -Path $Path -Root $loopbackDir) {
            return "loopback"
        }
        return "artifacts"
    }

    function Get-UniqueArchivePath {
        param(
            [string]$Directory,
            [string]$FileName
        )

        $candidate = Join-Path $Directory $FileName
        if (-not (Test-Path $candidate -PathType Leaf)) {
            return $candidate
        }

        $extension = [System.IO.Path]::GetExtension($FileName)
        $baseName = [System.IO.Path]::GetFileNameWithoutExtension($FileName)
        $archiveToken = [DateTime]::UtcNow.ToString("yyyyMMddHHmmssfff")
        for ($index = 1; ; ++$index) {
            $candidateName = "{0}-archived-{1}-{2}{3}" -f $baseName, $archiveToken, $index, $extension
            $candidate = Join-Path $Directory $candidateName
            if (-not (Test-Path $candidate -PathType Leaf)) {
                return $candidate
            }
        }
    }

    function Move-RunArtifactsToArchive {
        param(
            [object]$Group
        )

        $archiveErrors = [System.Collections.Generic.List[string]]::new()
        $fileRecords = [System.Collections.Generic.List[object]]::new()
        $archiveMonth = ([DateTime]$Group.newest).ToLocalTime().ToString("yyyyMM")
        $runArchiveDir = Join-Path (Join-Path $archiveRoot $archiveMonth) (ConvertTo-SafeArchiveName -Name $Group.token)

        foreach ($file in $Group.files) {
            if (-not (Test-Path $file.FullName -PathType Leaf)) {
                continue
            }

            $sourcePath = [System.IO.Path]::GetFullPath($file.FullName)
            $area = Get-ArtifactArea -Path $sourcePath
            $destinationDir = Join-Path $runArchiveDir $area
            if (-not (Test-Path $destinationDir -PathType Container)) {
                $null = New-Item -ItemType Directory -Path $destinationDir -Force
            }
            $destinationPath = Get-UniqueArchivePath -Directory $destinationDir -FileName $file.Name

            try {
                Move-Item -LiteralPath $sourcePath -Destination $destinationPath -Force -ErrorAction Stop
                $fileRecords.Add([ordered]@{
                    name = $file.Name
                    sourcePath = $sourcePath
                    archivedPath = [System.IO.Path]::GetFullPath($destinationPath)
                    artifactArea = $area
                    sizeBytes = $file.Length
                    lastWriteTimeUtc = $file.LastWriteTimeUtc.ToString("o")
                }) | Out-Null
            } catch {
                $archiveErrors.Add("$sourcePath`: $($_.Exception.Message)") | Out-Null
            }
        }

        if ($fileRecords.Count -gt 0) {
            if (-not (Test-Path $archiveRoot -PathType Container)) {
                $null = New-Item -ItemType Directory -Path $archiveRoot -Force
            }
            $manifestEntry = [ordered]@{
                schemaVersion = 1
                archivedAtUtc = [DateTime]::UtcNow.ToString("o")
                runToken = $Group.token
                newestUtc = ([DateTime]$Group.newest).ToString("o")
                archiveDirectory = [System.IO.Path]::GetFullPath($runArchiveDir)
                files = @($fileRecords.ToArray())
                errors = @($archiveErrors.ToArray())
            }
            $manifestEntry | ConvertTo-Json -Depth 8 -Compress | Add-Content -Path $archiveManifest -Encoding UTF8
        }

        return [ordered]@{
            movedFiles = $fileRecords.Count
            errors = @($archiveErrors.ToArray())
        }
    }

    function Add-RetentionFile {
        param(
            [System.IO.FileInfo]$File,
            [string]$RunToken
        )

        if ([string]::IsNullOrWhiteSpace($RunToken)) {
            return
        }
        if (-not $groups.ContainsKey($RunToken)) {
            $groups[$RunToken] = [ordered]@{
                token = $RunToken
                newest = $File.LastWriteTimeUtc
                files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
            }
        }
        $groups[$RunToken].files.Add($File) | Out-Null
        if ($File.LastWriteTimeUtc -gt $groups[$RunToken].newest) {
            $groups[$RunToken].newest = $File.LastWriteTimeUtc
        }
    }

    if (Test-Path $logDir -PathType Container) {
        foreach ($file in Get-ChildItem -Path $logDir -File) {
            if ($file.Name -match '^player-(?:loopback-)?smoke-(?<run>.+?)\.(?:log|jsonl|report\.json|harness\.json|summary\.json)$') {
                Add-RetentionFile -File $file -RunToken $Matches.run
            } elseif ($file.Name -match '^player-(?:loopback-)?smoke-(?<run>.+?)-render-mirror-session\d+(?:-(?:pre|post))?\.(?:raw|json)$') {
                Add-RetentionFile -File $file -RunToken $Matches.run
            } elseif ($file.Name -match '^playback-regression-(?<run>\d{8}-\d{6}-\d{3}-[0-9a-fA-F]{8})(?:-.+?\.harness)?\.json$') {
                Add-RetentionFile -File $file -RunToken "regression-$($Matches.run)"
            }
        }
    }

    if (Test-Path $loopbackDir -PathType Container) {
        foreach ($file in Get-ChildItem -Path $loopbackDir -File) {
            if ($file.Name -match '^loopback-(?<run>.+?)(?:-segment\d+)?\.(?:wav|report\.json|ready\.json|stop)$') {
                Add-RetentionFile -File $file -RunToken $Matches.run
            }
        }
    }

    $orderedGroups = @($groups.Values | Sort-Object -Property newest -Descending)
    $preserveSet = @{}
    foreach ($token in $PreserveRunTokens) {
        if (-not [string]::IsNullOrWhiteSpace($token)) {
            $preserveSet[$token] = $true
        }
    }
    $keptRunTokens = @{}
    for ($index = 0; $index -lt $orderedGroups.Count; ++$index) {
        $group = $orderedGroups[$index]
        if ($index -lt $KeepRuns -or $preserveSet.ContainsKey($group.token)) {
            $keptRunTokens[$group.token] = $true
        }
    }
    $summary.keptRuns = $keptRunTokens.Count

    if ($NoCleanup) {
        $summary.keptRuns = $orderedGroups.Count
        return $summary
    }

    if ($orderedGroups.Count -le $KeepRuns) {
        return $summary
    }

    for ($index = $KeepRuns; $index -lt $orderedGroups.Count; ++$index) {
        $group = $orderedGroups[$index]
        if ($preserveSet.ContainsKey($group.token)) {
            continue
        }
        $archiveResult = Move-RunArtifactsToArchive -Group $group
        if ($archiveResult.movedFiles -gt 0) {
            $summary.archivedRuns += 1
            $summary.archivedFiles += $archiveResult.movedFiles
        }
        if ($archiveResult.errors.Count -gt 0) {
            $summary.archiveErrors = @($summary.archiveErrors + $archiveResult.errors)
        }
    }

    return $summary
}
