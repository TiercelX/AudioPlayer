param(
    [string]$Matrix = (Join-Path $PSScriptRoot '..\docs\dev\eac3-joc-property-coverage.json')
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$matrixPath = (Resolve-Path $Matrix).Path
$document = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
$allowed = @('implemented', 'partial', 'missing', 'not-present', 'not-normative')
$sampleAllowed = @('observed', 'not-observed', 'not-a-bitstream-property', 'unknown')

if ($document.schema -ne 'audioplayer.eac3-joc-property-coverage.v1') {
    throw 'coverage-schema-mismatch'
}
if (-not $document.rows -or $document.rows.Count -lt 1) {
    throw 'coverage-rows-empty'
}
$ids = @{}
foreach ($row in $document.rows) {
    if ([string]::IsNullOrWhiteSpace($row.id) -or $ids.ContainsKey($row.id)) {
        throw "coverage-id-invalid-or-duplicate:$($row.id)"
    }
    $ids[$row.id] = $true
    foreach ($layer in @('decoder', 'scene', 'renderer')) {
        if ($allowed -notcontains $row.$layer) {
            throw "coverage-status-invalid:$($row.id):${layer}:$($row.$layer)"
        }
    }
    if ($sampleAllowed -notcontains $row.samplePresence) {
        throw "coverage-sample-status-invalid:$($row.id):$($row.samplePresence)"
    }
    if ([string]::IsNullOrWhiteSpace($row.standard) -or -not $row.evidence) {
        throw "coverage-source-empty:$($row.id)"
    }
    foreach ($relative in $row.evidence) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $relative))) {
            throw "coverage-evidence-missing:$($row.id):$relative"
        }
    }
}

Write-Output "AtmosPropertyCoverage=PASS rows=$($document.rows.Count) matrix=$matrixPath"
