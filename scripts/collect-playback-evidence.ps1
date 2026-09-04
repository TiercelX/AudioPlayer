param(
    [string]$BuildDir = "",
    [string]$LogPath = "",
    [string]$HarnessReportFile = "",
    [string]$AppReportFile = "",
    [string]$RegressionReportFile = "",
    [string]$LoopbackReportFile = "",
    [string]$OutputDir = "",
    [switch]$IncludeLoopbackWavs,
    [switch]$NoAnalyzer
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common-paths.ps1")

function Resolve-ExistingRepoFile {
    param(
        [string]$RepoRoot,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $resolved = Resolve-AudioPlayerRepoPath -RepoRoot $RepoRoot -Path $Path
    if (Test-Path $resolved -PathType Leaf) {
        return $resolved
    }
    return ""
}

function Resolve-LatestFile {
    param(
        [string]$Directory,
        [string]$Filter
    )

    if (-not (Test-Path $Directory -PathType Container)) {
        return ""
    }

    $file = Get-ChildItem -Path $Directory -Filter $Filter -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $file) {
        return ""
    }
    return $file.FullName
}

function Add-EvidenceFile {
    param(
        [string]$Path,
        [string]$DestinationRoot,
        [System.Collections.Generic.List[object]]$CopiedFiles,
        [System.Collections.Generic.List[string]]$MissingFiles,
        [string]$Role
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path $resolved -PathType Leaf)) {
        $MissingFiles.Add($resolved) | Out-Null
        return
    }

    $fileName = [System.IO.Path]::GetFileName($resolved)
    $destination = Join-Path $DestinationRoot $fileName
    if ($resolved -ne [System.IO.Path]::GetFullPath($destination)) {
        Copy-Item -Path $resolved -Destination $destination -Force
    }
    $CopiedFiles.Add([ordered]@{
        role = $Role
        source = $resolved
        copiedTo = $destination
        bytes = (Get-Item -Path $destination).Length
    }) | Out-Null
}

function Read-JsonFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path -PathType Leaf)) {
        return $null
    }

    try {
        return Get-Content -Path $Path -Encoding UTF8 -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Get-JsonProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Invoke-GitCapture {
    param(
        [string]$RepoRoot,
        [string[]]$Arguments
    )

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        return @(& git -C $RepoRoot @Arguments 2>$null)
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
}

function Add-ReportLinkedFiles {
    param(
        [object]$Report,
        [string]$DestinationRoot,
        [System.Collections.Generic.List[object]]$CopiedFiles,
        [System.Collections.Generic.List[string]]$MissingFiles
    )

    $files = Get-JsonProperty -Object $Report -Name "files"
    if ($null -eq $files) {
        return
    }

    foreach ($entry in @(
            @{ Name = "textLogFile"; Role = "text-log" },
            @{ Name = "jsonlDiagnosticFile"; Role = "jsonl-diagnostics" },
            @{ Name = "appReportFile"; Role = "app-report" },
            @{ Name = "harnessReportFile"; Role = "harness-report" }
        )) {
        $path = [string](Get-JsonProperty -Object $files -Name $entry.Name)
        Add-EvidenceFile -Path $path -DestinationRoot $DestinationRoot -CopiedFiles $CopiedFiles -MissingFiles $MissingFiles -Role $entry.Role
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Resolve-AudioPlayerBuildDir -BuildDir $BuildDir
$cacheDir = Resolve-AudioPlayerCacheDir -RepoRoot $repoRoot -BuildDir $BuildDir
$logDir = Resolve-AudioPlayerLogDir -RepoRoot $repoRoot -BuildDir $BuildDir
$loopbackDir = Resolve-AudioPlayerCacheSubdir -RepoRoot $repoRoot -BuildDir $BuildDir -Name "loopback"
$runId = Get-Date -Format "yyyyMMdd-HHmmss"

$sourceLogPath = Resolve-ExistingRepoFile -RepoRoot $repoRoot -Path $LogPath
if ([string]::IsNullOrWhiteSpace($sourceLogPath)) {
    $sourceLogPath = Resolve-LatestFile -Directory $logDir -Filter "player-*.log"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path (Resolve-AudioPlayerCacheSubdir -RepoRoot $repoRoot -BuildDir $BuildDir -Name "evidence") "playback-evidence-$runId"
}
$evidenceDir = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $OutputDir
if (-not (Test-Path $evidenceDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $evidenceDir -Force
}

$copiedFiles = [System.Collections.Generic.List[object]]::new()
$missingFiles = [System.Collections.Generic.List[string]]::new()
$notes = [System.Collections.Generic.List[string]]::new()

Add-EvidenceFile -Path $sourceLogPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "source-log"

$harnessReportPath = Resolve-ExistingRepoFile -RepoRoot $repoRoot -Path $HarnessReportFile
if ([string]::IsNullOrWhiteSpace($harnessReportPath) -and -not [string]::IsNullOrWhiteSpace($sourceLogPath)) {
    $candidate = [System.IO.Path]::ChangeExtension($sourceLogPath, ".harness.json")
    if (Test-Path $candidate -PathType Leaf) {
        $harnessReportPath = $candidate
    }
}
Add-EvidenceFile -Path $harnessReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "harness-report"
$harnessReport = Read-JsonFile -Path $harnessReportPath
Add-ReportLinkedFiles -Report $harnessReport -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles
$jsonlDiagnosticPath = ""
if ($null -ne $harnessReport) {
    $harnessFiles = Get-JsonProperty -Object $harnessReport -Name "files"
    $jsonlDiagnosticPath = [string](Get-JsonProperty -Object $harnessFiles -Name "jsonlDiagnosticFile")
}
if ([string]::IsNullOrWhiteSpace($jsonlDiagnosticPath) -and -not [string]::IsNullOrWhiteSpace($sourceLogPath)) {
    $candidate = [System.IO.Path]::ChangeExtension($sourceLogPath, ".jsonl")
    if (Test-Path $candidate -PathType Leaf) {
        $jsonlDiagnosticPath = $candidate
    }
}

$appReportPath = Resolve-ExistingRepoFile -RepoRoot $repoRoot -Path $AppReportFile
if ([string]::IsNullOrWhiteSpace($appReportPath) -and -not [string]::IsNullOrWhiteSpace($sourceLogPath)) {
    $candidate = [System.IO.Path]::ChangeExtension($sourceLogPath, ".report.json")
    if (Test-Path $candidate -PathType Leaf) {
        $appReportPath = $candidate
    }
}
Add-EvidenceFile -Path $appReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "app-report"

$regressionReportPath = Resolve-ExistingRepoFile -RepoRoot $repoRoot -Path $RegressionReportFile
if ([string]::IsNullOrWhiteSpace($regressionReportPath)) {
    $regressionReportPath = Resolve-LatestFile -Directory $logDir -Filter "playback-regression-*.json"
}
Add-EvidenceFile -Path $regressionReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "regression-report"

$loopbackReportPath = Resolve-ExistingRepoFile -RepoRoot $repoRoot -Path $LoopbackReportFile
if ([string]::IsNullOrWhiteSpace($loopbackReportPath)) {
    $loopbackReportPath = Resolve-LatestFile -Directory $loopbackDir -Filter "loopback-*.report.json"
}
Add-EvidenceFile -Path $loopbackReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-report"
$loopbackReport = Read-JsonFile -Path $loopbackReportPath
if ($null -ne $loopbackReport) {
    foreach ($metadataPath in @($loopbackReport.segmentMetadataFiles)) {
        Add-EvidenceFile -Path ([string]$metadataPath) -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-segment-report"
    }
    if ($IncludeLoopbackWavs) {
        foreach ($wavPath in @($loopbackReport.wavFiles)) {
            Add-EvidenceFile -Path ([string]$wavPath) -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-wav"
        }
    } else {
        $notes.Add("Loopback WAV files were not copied. Re-run with -IncludeLoopbackWavs when raw endpoint capture audio is needed.") | Out-Null
    }
}

$alignmentReportPath = Join-Path $evidenceDir "loopback-alignment.report.json"
$alignmentExit = "skipped"
if ($null -ne $loopbackReport -and
    -not [string]::IsNullOrWhiteSpace($loopbackReportPath) -and
    -not [string]::IsNullOrWhiteSpace($jsonlDiagnosticPath)) {
    try {
        $alignmentOutput = & (Join-Path $PSScriptRoot "summarize-loopback-alignment.ps1") `
            -JsonlDiagnosticFile $jsonlDiagnosticPath `
            -HarnessReportFile $harnessReportPath `
            -LoopbackReportFile $loopbackReportPath `
            -ReportFile $alignmentReportPath 2>&1
        $alignmentOutput | Set-Content -Path (Join-Path $evidenceDir "loopback-alignment.txt") -Encoding UTF8
        Add-EvidenceFile -Path $alignmentReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-alignment-report"
        Add-EvidenceFile -Path (Join-Path $evidenceDir "loopback-alignment.txt") -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-alignment-text"
        $alignmentExit = "ok"
    } catch {
        $alignmentExit = "failed"
        $alignmentErrorPath = Join-Path $evidenceDir "loopback-alignment.txt"
        ("loopback alignment failed: {0}" -f $_.Exception.Message) | Set-Content -Path $alignmentErrorPath -Encoding UTF8
        Add-EvidenceFile -Path $alignmentErrorPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "loopback-alignment-error"
    }
}

$analyzerTextPath = Join-Path $evidenceDir "analyzer.txt"
$analyzerReportPath = Join-Path $evidenceDir "analyzer.report.json"
$analyzerExit = "skipped"
if (-not $NoAnalyzer -and -not [string]::IsNullOrWhiteSpace($sourceLogPath)) {
    try {
        $analyzerOutput = & (Join-Path $PSScriptRoot "analyze-audio-artifacts.ps1") `
            -BuildDir $BuildDir `
            -LogPath $sourceLogPath `
            -ReportFile $analyzerReportPath 2>&1
        $analyzerOutput | Set-Content -Path $analyzerTextPath -Encoding UTF8
        Add-EvidenceFile -Path $analyzerTextPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "analyzer-text"
        Add-EvidenceFile -Path $analyzerReportPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "analyzer-report"
        $analyzerExit = "ok"
    } catch {
        $analyzerExit = "failed"
        ("analyzer failed: {0}" -f $_.Exception.Message) | Set-Content -Path $analyzerTextPath -Encoding UTF8
        Add-EvidenceFile -Path $analyzerTextPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "analyzer-error"
    }
}

$gitInfoPath = Join-Path $evidenceDir "git-info.json"
$gitStatusPath = Join-Path $evidenceDir "git-status.txt"
$gitBranch = Invoke-GitCapture -RepoRoot $repoRoot -Arguments @("rev-parse", "--abbrev-ref", "HEAD")
$gitCommit = Invoke-GitCapture -RepoRoot $repoRoot -Arguments @("rev-parse", "HEAD")
$gitStatus = Invoke-GitCapture -RepoRoot $repoRoot -Arguments @("status", "--short")
$gitDiffStat = Invoke-GitCapture -RepoRoot $repoRoot -Arguments @("diff", "--stat")
$gitInfo = [ordered]@{
    branch = [string]$gitBranch
    commit = [string]$gitCommit
    diffStat = @($gitDiffStat)
}
$gitInfo | ConvertTo-Json -Depth 5 | Set-Content -Path $gitInfoPath -Encoding UTF8
@($gitStatus) | Set-Content -Path $gitStatusPath -Encoding UTF8
Add-EvidenceFile -Path $gitInfoPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "git-info"
Add-EvidenceFile -Path $gitStatusPath -DestinationRoot $evidenceDir -CopiedFiles $copiedFiles -MissingFiles $missingFiles -Role "git-status"

$manifestPath = Join-Path $evidenceDir "manifest.json"
$manifest = [ordered]@{
    createdAt = (Get-Date).ToUniversalTime().ToString("o")
    repoRoot = $repoRoot
    buildDir = $BuildDir
    evidenceDir = $evidenceDir
    sourceLog = $sourceLogPath
    harnessReport = $harnessReportPath
    appReport = $appReportPath
    regressionReport = $regressionReportPath
    loopbackReport = $loopbackReportPath
    analyzer = [ordered]@{
        status = $analyzerExit
        text = $analyzerTextPath
        report = $analyzerReportPath
    }
    loopbackAlignment = [ordered]@{
        status = $alignmentExit
        report = $alignmentReportPath
    }
    includeLoopbackWavs = [bool]$IncludeLoopbackWavs
    copiedFiles = @($copiedFiles)
    missingFiles = @($missingFiles)
    notes = @($notes)
}
$manifest | ConvertTo-Json -Depth 12 | Set-Content -Path $manifestPath -Encoding UTF8

Write-Output "evidenceDir:$evidenceDir"
Write-Output "manifest:$manifestPath"
Write-Output "copiedFiles:$($copiedFiles.Count)"
Write-Output "missingFiles:$($missingFiles.Count)"
Write-Output "analyzer:$analyzerExit"
Write-Output "loopbackAlignment:$alignmentExit"
