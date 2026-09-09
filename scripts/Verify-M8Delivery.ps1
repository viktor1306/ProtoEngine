[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $BuildRoot,
    [string] $SourceRoot = (Join-Path $PSScriptRoot '..')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildRoot)
$source = [IO.Path]::GetFullPath($SourceRoot)
$descriptorPath = Join-Path $build 'test-results/m8-acceptance-details.json'
$editorReportPath = Join-Path $build 'test-results/m8-editor.json'
$acceptance = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
$editorReport = Get-Content -LiteralPath $editorReportPath -Raw | ConvertFrom-Json
if (-not $acceptance.passed -or -not $editorReport.passed -or -not $editorReport.acceptance_workflow) {
    throw 'A successful M8 native acceptance run is required before verifying the delivery.'
}
$runRoot = Join-Path $build ('test-results/m8-delivery/r-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$distribution = Join-Path $runRoot 'ProtoEngine-v0.1'
$results = Join-Path $runRoot 'check'
New-Item -ItemType Directory -Path $runRoot | Out-Null
& (Join-Path $source 'scripts/Package-M8Distribution.ps1') -BuildRoot $build -SourceRoot $source `
    -DemoProject ([string]$acceptance.project_root) -DemoPackage ([string]$acceptance.package_root) `
    -OutputRoot $distribution
& (Join-Path $source 'tests/M8DistributionSmoke.ps1') -DistributionRoot $distribution -OutputRoot $results
$receipt = [ordered]@{
    format = 'proto.m8.delivery'; version = 1; passed = $true; completed = (Get-Date -Format o)
    acceptance = $descriptorPath; distribution = $distribution; results = $results
    distributionManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $distribution 'distribution-manifest.json') -Algorithm SHA256).Hash
}
[IO.File]::WriteAllText((Join-Path $build 'test-results/m8-delivery.json'),
    ($receipt | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
Write-Output ('M8 delivery verified: ' + $distribution)
