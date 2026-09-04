param(
    [Parameter(Mandatory = $true)]
    [string]$Query,
    [string]$BuildDir = "",
    [switch]$CurrentOnly,
    [switch]$ArchiveOnly
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common-paths.ps1")

$repoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
$loopbackDir = Resolve-AudioPlayerCacheSubdir -RepoRoot $repoRoot -BuildDir $BuildDir -Name "loopback"
$archiveRoot = Resolve-AudioPlayerCacheSubdir -RepoRoot $repoRoot -BuildDir $BuildDir -Name "test-artifact-archive"
$archiveManifest = Join-Path $archiveRoot "manifest.jsonl"

function Test-ContainsQuery {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return $false
    }
    return $Text.IndexOf($Query, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
}

function Write-CurrentMatches {
    param(
        [string]$Directory,
        [string]$Role
    )

    if (-not (Test-Path $Directory -PathType Container)) {
        return
    }

    Get-ChildItem -Path $Directory -File -ErrorAction SilentlyContinue |
        Where-Object { (Test-ContainsQuery -Text $_.Name) -or (Test-ContainsQuery -Text $_.FullName) } |
        Sort-Object LastWriteTime -Descending |
        ForEach-Object {
            Write-Output ("current:{0}:{1}" -f $Role, $_.FullName)
        }
}

function Write-ArchiveMatches {
    if (-not (Test-Path $archiveManifest -PathType Leaf)) {
        return
    }

    foreach ($line in Get-Content -Path $archiveManifest -Encoding UTF8) {
        if ([string]::IsNullOrWhiteSpace($line) -or -not (Test-ContainsQuery -Text $line)) {
            continue
        }

        $entry = $line | ConvertFrom-Json
        Write-Output ("archive:run={0}:newestUtc={1}:dir={2}" -f $entry.runToken, $entry.newestUtc, $entry.archiveDirectory)
        foreach ($file in @($entry.files)) {
            if ((Test-ContainsQuery -Text ([string]$file.name)) -or
                (Test-ContainsQuery -Text ([string]$file.sourcePath)) -or
                (Test-ContainsQuery -Text ([string]$file.archivedPath)) -or
                (Test-ContainsQuery -Text ([string]$entry.runToken))) {
                Write-Output ("archiveFile:{0}:source={1}" -f $file.archivedPath, $file.sourcePath)
            }
        }
    }
}

if (-not $ArchiveOnly) {
    Write-CurrentMatches -Directory $logDir -Role "logs"
    Write-CurrentMatches -Directory $loopbackDir -Role "loopback"
}

if (-not $CurrentOnly) {
    Write-ArchiveMatches
}
