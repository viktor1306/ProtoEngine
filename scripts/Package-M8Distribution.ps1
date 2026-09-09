# Package assembly uses PowerShell 7/.NET 6 for GetRelativePath and ToHexString.
# The generated template creator intentionally remains Windows PowerShell 5.1 compatible.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $BuildRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $DemoProject,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputRoot,

    [Parameter(Mandatory = $false)]
    [string] $SourceRoot,

    [Parameter(Mandatory = $false)]
    [string] $DemoPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Join-Path $PSScriptRoot '..'
}
$source = [System.IO.Path]::GetFullPath($SourceRoot)
$build = [System.IO.Path]::GetFullPath($BuildRoot)
$output = [System.IO.Path]::GetFullPath($OutputRoot)
$bin = Join-Path $build 'bin'
$sdk = Join-Path $build 'sdk'
$projectInput = [System.IO.Path]::GetFullPath($DemoProject)
$demo = if ((Test-Path -LiteralPath $projectInput -PathType Leaf) -and
           ([System.IO.Path]::GetFileName($projectInput) -ieq 'project.proto.json')) {
    Split-Path -Parent $projectInput
} else {
    $projectInput
}

function Require-Directory {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "$Label directory is missing: $Path" }
}
function Require-File {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label file is missing: $Path" }
}
function Assert-NoReparse {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    $root = Get-Item -LiteralPath $Path -Force
    if (($root.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label is a reparse point: $Path"
    }
    foreach ($item in @(Get-ChildItem -LiteralPath $Path -Force -Recurse -ErrorAction Stop)) {
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Label contains a reparse point: $($item.FullName)"
        }
    }
}
function Copy-Tree {
    param([Parameter(Mandatory = $true)][string] $Source, [Parameter(Mandatory = $true)][string] $Destination,
          [Parameter(Mandatory = $true)][string] $Label)
    Require-Directory $Source $Label
    Assert-NoReparse $Source $Label
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath $Source -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
    }
}
function Copy-One {
    param([Parameter(Mandatory = $true)][string] $Source, [Parameter(Mandatory = $true)][string] $Destination,
          [Parameter(Mandatory = $true)][string] $Label)
    Require-File $Source $Label
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}
function Write-Utf8 {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}
function Write-Json {
    param([Parameter(Mandatory = $true)] $Value, [Parameter(Mandatory = $true)][string] $Path)
    Write-Utf8 $Path ($Value | ConvertTo-Json -Depth 32)
}
function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Relative-Path {
    param([Parameter(Mandatory = $true)][string] $Root, [Parameter(Mandatory = $true)][string] $Path)
    return ([System.IO.Path]::GetRelativePath($Root, $Path)).Replace('\', '/')
}
function Get-StringProperty {
    param([Parameter(Mandatory = $true)] $Object, [Parameter(Mandatory = $true)][string[]] $Names)
    if ($null -eq $Object) { return $null }
    foreach ($property in @($Object.PSObject.Properties)) {
        if ($property.Value -is [string] -and $Names -contains $property.Name.ToLowerInvariant()) {
            return [string]$property.Value
        }
        if ($null -ne $property.Value -and $property.Value -isnot [string] -and $property.Value -isnot [ValueType]) {
            $nested = Get-StringProperty $property.Value $Names
            if ($nested) { return $nested }
        }
    }
    return $null
}
function Assert-OwnedStagePath {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $ExpectedParent,
          [Parameter(Mandatory = $true)][string] $ExpectedOutput)
    $full = [System.IO.Path]::GetFullPath($Path)
    $parent = [System.IO.Path]::GetFullPath((Split-Path -Parent $full))
    $expected = [System.IO.Path]::GetFullPath($ExpectedParent)
    $prefix = (Split-Path -Leaf ([System.IO.Path]::GetFullPath($ExpectedOutput))) + '.tmp-'
    if ($parent -ne $expected -or -not (Split-Path -Leaf $full).StartsWith($prefix, [System.StringComparison]::Ordinal)) {
        throw "Refusing to clean an unowned staging path: $full"
    }
}
function Normalize-PackageRoot {
    param([Parameter(Mandatory = $true)][string] $Candidate)
    $path = [System.IO.Path]::GetFullPath($Candidate)
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        if ([System.IO.Path]::GetExtension($path) -ieq '.json') {
            $descriptor = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            $value = Get-StringProperty $descriptor @('package', 'copiedpackage', 'packageroot', 'package_root', 'root', 'path', 'executable')
            if (-not $value) { return $null }
            return Normalize-PackageRoot ([System.IO.Path]::GetFullPath($value))
        }
        $path = Split-Path -Parent $path
    }
    if ((Test-Path -LiteralPath (Join-Path $path 'Data/runtime.json') -PathType Leaf) -and
        (@(Get-ChildItem -LiteralPath $path -Filter '*.exe' -File).Count -gt 0)) {
        return $path
    }
    return $null
}
function Resolve-DemoPackage {
    if (-not [string]::IsNullOrWhiteSpace($DemoPackage)) {
        $explicit = Normalize-PackageRoot $DemoPackage
        if (-not $explicit) { throw "Demo package is not a valid runtime package: $DemoPackage" }
        return $explicit
    }
    $descriptor = Join-Path $build 'test-results/m8-acceptance-details.json'
    if (Test-Path -LiteralPath $descriptor -PathType Leaf) {
        $found = Normalize-PackageRoot $descriptor
        if ($found) { return $found }
    }
    throw 'No M8 acceptance demo package was found. Pass -DemoPackage with the standalone package directory or acceptance descriptor.'
}

Require-Directory $source 'SourceRoot'
Require-Directory $build 'BuildRoot'
Require-Directory $bin 'Build bin'
Require-Directory $sdk 'Installed SDK'
Require-Directory $demo 'Demo project'
Require-File (Join-Path $demo 'project.proto.json') 'Demo project manifest'
Require-Directory (Join-Path $demo 'Assets') 'Demo Assets'
Require-Directory (Join-Path $demo 'Scenes') 'Demo Scenes'
Require-Directory (Join-Path $demo 'Code') 'Demo Code'
Require-Directory (Join-Path $demo 'Config') 'Demo Config'
Require-Directory (Join-Path $demo 'Notices') 'Demo Notices'
Require-File (Join-Path $demo 'README.md') 'Demo README'
Assert-NoReparse $demo 'Demo project'
Assert-NoReparse $sdk 'Installed SDK'
$sdkInfo = Get-Content -LiteralPath (Join-Path $sdk 'sdk.json') -Raw | ConvertFrom-Json
if ([string]$sdkInfo.configuration -notin @('Debug', 'Release')) { throw "Unsupported SDK configuration: $($sdkInfo.configuration)" }
$demoManifest = Get-Content -LiteralPath (Join-Path $demo 'project.proto.json') -Raw | ConvertFrom-Json
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite an existing distribution: $output" }
$outputParent = Split-Path -Parent $output

$requiredBinaries = @('ProtoEditor.exe', 'ProtoPackage.exe', 'ProtoProject.exe', 'ProtoPlayer.exe')
foreach ($name in $requiredBinaries) { Require-File (Join-Path $bin $name) "Distribution binary $name" }
Require-Directory (Join-Path $bin 'shaders') 'Build shaders'
Require-Directory (Join-Path $bin 'licenses') 'Build licenses'
$noticeFiles = @(Get-ChildItem -LiteralPath $bin -File -Force | Where-Object { $_.Name -match '^THIRD_PARTY_NOTICES\.(md|txt)$' })
if ($noticeFiles.Count -eq 0) { throw "Build third-party notice is missing beside the binaries: $bin" }
if ([string]$sdkInfo.configuration -eq 'Debug') { Require-Directory (Join-Path $bin 'validation') 'Debug validation files' }
$demoPackageRoot = Resolve-DemoPackage
Assert-NoReparse $demoPackageRoot 'Demo runtime package'
# Staging is a sibling of OutputRoot. It must never be inside an input tree,
# otherwise recursive copying would include the growing staging tree itself.
foreach ($copyRoot in @($sdk, $bin, $demo, $demoPackageRoot, (Join-Path $source 'docs/research/m8'))) {
    $copyRootFull = [IO.Path]::GetFullPath($copyRoot).TrimEnd('\', '/')
    $outputParentFull = [IO.Path]::GetFullPath($outputParent).TrimEnd('\', '/')
    if ($outputParentFull.Equals($copyRootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $outputParentFull.StartsWith($copyRootFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Distribution output overlaps a copied input tree: $copyRoot"
    }
}
# Reject junction ancestors before relying on lexical containment for writes.
$existingOutputAncestor = $outputParent
while (-not (Test-Path -LiteralPath $existingOutputAncestor)) {
    $existingOutputAncestor = Split-Path -Parent $existingOutputAncestor
    if (-not $existingOutputAncestor) { throw 'Distribution output has no existing parent.' }
}
$outputAncestor = Get-Item -LiteralPath $existingOutputAncestor -Force
while ($null -ne $outputAncestor) {
    if (($outputAncestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Distribution output has a reparse-point ancestor: $($outputAncestor.FullName)"
    }
    $outputAncestor = $outputAncestor.Parent
}
$demoExecutables = @(Get-ChildItem -LiteralPath $demoPackageRoot -Filter '*.exe' -File)
if ($demoExecutables.Count -ne 1) { throw "Demo runtime package must contain exactly one executable: $demoPackageRoot" }
$demoExecutable = $demoExecutables[0]
$runtimeManifestPath = Join-Path $demoPackageRoot 'Data/runtime.json'
Require-File $runtimeManifestPath 'Demo runtime manifest'
$runtimeManifest = Get-Content -LiteralPath $runtimeManifestPath -Raw | ConvertFrom-Json
if ([string]$runtimeManifest.projectId -ne [string]$demoManifest.projectId) {
    throw 'Accepted demo package projectId does not match the authored demo manifest.'
}
if ([string]$runtimeManifest.startupScene -ne [string]$demoManifest.startupScene) {
    throw 'Accepted demo package startupScene does not match the authored demo manifest.'
}
if ([string]$runtimeManifest.sdkBuildId -ne [string]$sdkInfo.sdkBuildId) {
    throw 'Accepted demo package sdkBuildId does not match the selected SDK.'
}

$stage = Join-Path $outputParent ((Split-Path -Leaf $output) + '.tmp-' + [Guid]::NewGuid().ToString('N'))
Assert-OwnedStagePath $stage $outputParent $output
if ($outputParent) { New-Item -ItemType Directory -Path $outputParent -Force | Out-Null }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$published = $false
try {
    $stageBin = Join-Path $stage 'bin'
    $stageSdk = Join-Path $stage 'sdk'
    $stageTemplate = Join-Path $stage 'Templates/ProtoCppEmpty'
    $stageDemo = Join-Path $stage 'Examples/ProtoM8Demo'
    $stagePlayer = Join-Path $stage 'Player'
    $stageDocs = Join-Path $stage 'Docs'
    New-Item -ItemType Directory -Path $stageBin, $stageSdk, $stageTemplate, $stageDemo, $stagePlayer, $stageDocs -Force | Out-Null

    foreach ($name in $requiredBinaries) { Copy-One (Join-Path $bin $name) (Join-Path $stageBin $name) "Build binary $name" }
    Copy-Tree (Join-Path $bin 'shaders') (Join-Path $stageBin 'shaders') 'Build shaders'
    Copy-Tree (Join-Path $bin 'licenses') (Join-Path $stageBin 'licenses') 'Build licenses'
    foreach ($notice in $noticeFiles) { Copy-One $notice.FullName (Join-Path $stageBin $notice.Name) 'Build notice' }
    if ([string]$sdkInfo.configuration -eq 'Debug') {
        Copy-Tree (Join-Path $bin 'validation') (Join-Path $stageBin 'validation') 'Debug validation files'
    }
    Copy-Tree $sdk $stageSdk 'Installed SDK'

    Copy-One (Join-Path $demo 'project.proto.json') (Join-Path $stageDemo 'project.proto.json') 'Demo manifest'
    foreach ($folder in @('Assets', 'Scenes', 'Code', 'Config', 'Notices')) {
        Copy-Tree (Join-Path $demo $folder) (Join-Path $stageDemo $folder) "Demo $folder"
    }
    Copy-One (Join-Path $demo 'README.md') (Join-Path $stageDemo 'README.md') 'Demo README'
    Copy-Tree $demoPackageRoot $stagePlayer 'Accepted demo package'

    foreach ($name in @('Create-Project.ps1', 'README.md')) {
        Copy-One (Join-Path (Join-Path $source 'templates/ProtoCppEmpty') $name) (Join-Path $stageTemplate $name) "Empty template $name"
    }

    $requiredDocs = @('FOUNDATION.md', 'ARCHITECTURE.md', 'DATA_FORMATS.md', 'DEPENDENCIES.md',
        'M5_SDK_GUIDE.md', 'M6_CONTRACT.md', 'M6_GUIDE.md', 'M6_REPORT.md', 'M7_CONTRACT.md',
        'M7_GUIDE.md', 'M7_REPORT.md', 'M8_CONTRACT.md', 'M8_GUIDE.md', 'M8_REPORT.md')
    foreach ($name in $requiredDocs) {
        Copy-One (Join-Path (Join-Path $source 'docs') $name) (Join-Path $stageDocs $name) "Documentation $name"
    }
    $optionalEvidence = @(
        @{ source = 'docs/research/m7/final/summary-comparison.csv'; destination = 'Docs/research/m7/final/summary-comparison.csv' },
        @{ source = 'docs/research/m7/final/summary-groups.csv'; destination = 'Docs/research/m7/final/summary-groups.csv' },
        @{ source = 'docs/research/m7/final/summary-runs.csv'; destination = 'Docs/research/m7/final/summary-runs.csv' },
        @{ source = 'docs/research/m8-environment.json'; destination = 'Docs/research/m8-environment.json' },
        @{ source = 'docs/research/m8-summary.json'; destination = 'Docs/research/m8-summary.json' },
        @{ source = 'docs/research/m8-review.md'; destination = 'Docs/research/m8-review.md' },
        @{ source = 'docs/research/m8-preservation.json'; destination = 'Docs/research/m8-preservation.json' },
        @{ source = 'docs/research/m8-template-ps51.json'; destination = 'Docs/research/m8-template-ps51.json' },
        @{ source = 'docs/research/m8-package-guards.json'; destination = 'Docs/research/m8-package-guards.json' },
        @{ source = 'docs/research/m7/final-tables.md'; destination = 'Docs/research/m7/final-tables.md' },
        @{ source = 'docs/research/m7/final/summary.json'; destination = 'Docs/research/m7/final/summary.json' },
        @{ source = 'docs/research/m7/vsync-final/summary.json'; destination = 'Docs/research/m7/vsync-final/summary.json' },
        @{ source = 'docs/research/m7/supplement-summary-final.json'; destination = 'Docs/research/m7/supplement-summary-final.json' },
        @{ source = 'build/m7-release/test-results/m7-gpu-asset-cycle.json'; destination = 'Docs/research/m7/m7-gpu-asset-cycle.json' },
        @{ source = 'docs/research/m7-summary.json'; destination = 'Docs/research/m7-summary.json' },
        @{ source = 'docs/research/m7-summary-groups.csv'; destination = 'Docs/research/m7-summary-groups.csv' },
        @{ source = 'docs/research/m7-summary-comparison.csv'; destination = 'Docs/research/m7-summary-comparison.csv' },
        @{ source = 'docs/research/m7-summary-runs.csv'; destination = 'Docs/research/m7-summary-runs.csv' },
        @{ source = 'docs/research/m7/summary-groups.csv'; destination = 'Docs/research/m7/summary-groups.csv' },
        @{ source = 'docs/research/m7/summary-comparison.csv'; destination = 'Docs/research/m7/summary-comparison.csv' },
        @{ source = 'docs/research/m7/summary-runs.csv'; destination = 'Docs/research/m7/summary-runs.csv' }
    )
    foreach ($entry in $optionalEvidence) {
        $sourcePath = Join-Path $source $entry.source
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            Copy-One $sourcePath (Join-Path $stage $entry.destination) "M7 evidence $($entry.source)"
        }
    }

    if (Test-Path -LiteralPath (Join-Path $source 'docs/research/m8') -PathType Container) {
        Copy-Tree (Join-Path $source 'docs/research/m8') (Join-Path $stageDocs 'research/m8') 'M8 acceptance evidence'
    }
    $stagedDemoExecutable = @(Get-ChildItem -LiteralPath $stagePlayer -Filter '*.exe' -File)
    if ($stagedDemoExecutable.Count -ne 1) { throw "Staged Player package has no unambiguous executable: $stagePlayer" }
    $playerRelative = Relative-Path $stage $stagedDemoExecutable.FullName
    $playerCommandPath = $playerRelative.Replace('/', '\')
    Write-Utf8 (Join-Path $stage 'Launch-Editor.cmd') @"
@echo off
setlocal
start "" "%~dp0bin\ProtoEditor.exe" %*
"@
    Write-Utf8 (Join-Path $stage 'Launch-Demo.cmd') @"
@echo off
setlocal
start "" "%~dp0bin\ProtoEditor.exe" --project "%~dp0Examples\ProtoM8Demo\project.proto.json" --scene-camera %*
"@
    Write-Utf8 (Join-Path $stage 'Launch-Player.cmd') @"
@echo off
setlocal
start "" "%~dp0$playerCommandPath" %*
"@
    Write-Utf8 (Join-Path $stage 'README.md') @'
# Proto Engine v0.1

- `Launch-Demo.cmd` — демонстраційний проєкт у редакторі.
- `Launch-Editor.cmd` — редактор для своїх проєктів.
- `Launch-Player.cmd` — готова демонстрація; WASD/Q/E рухають камеру,
  стрілки змінюють огляд, Space підіймає куб, F1 відкриває графіку.

Для передавання готової програми достатньо всієї папки `Player`.
Потрібні Windows x64 і драйвер Vulkan 1.3. Компілятор, SDK та Python
для запуску Player не потрібні. Для роботи редактора виберіть папку з правом запису.

[Початок роботи](Docs/M8_GUIDE.md) · [Приймання й обмеження](Docs/M8_REPORT.md)
· [Продуктивність](Docs/M7_REPORT.md) · [C++-шаблон](Templates/ProtoCppEmpty/README.md).

Компіляція C++-проєктів використовує наявні GCC/CMake/Ninja, зазначені
у `sdk/sdk.json`. Інструменти та Python до комплекту не включені.
`distribution-manifest.json` містить склад файлів, SHA-256 і версію SDK.
Скрипт складання комплекту потребує PowerShell 7; стартовий шаблон працює
також із Windows PowerShell 5.1.
'@

    $fileEntries = [Collections.Generic.List[object]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $stage -File -Recurse -Force |
            Where-Object { $_.FullName -ne (Join-Path $stage 'distribution-manifest.json') } |
            Sort-Object FullName)) {
        $relative = Relative-Path $stage $file.FullName
        $hash = Get-Sha256 $file.FullName
        $fileEntries.Add([ordered]@{ path = $relative; bytes = [uint64]$file.Length; sha256 = $hash })
    }
    $material = [Text.StringBuilder]::new()
    foreach ($entry in $fileEntries) { [void]$material.Append($entry.path).Append('|').Append($entry.bytes).Append('|').Append($entry.sha256).Append("`n") }
    $contentBytes = [Text.UTF8Encoding]::new($false).GetBytes($material.ToString())
    $contentHash = [Convert]::ToHexString(([Security.Cryptography.SHA256]::Create().ComputeHash($contentBytes))).ToLowerInvariant()
    $manifest = [ordered]@{
        format = 'proto.distribution'; version = 1; generated = (Get-Date -Format o)
        contentSha256 = $contentHash; configuration = [string]$sdkInfo.configuration
        sdkBuildId = [string]$sdkInfo.sdkBuildId; sdkJsonSha256 = Get-Sha256 (Join-Path $stageSdk 'sdk.json')
        toolchain = [ordered]@{ compiler = [string]$sdkInfo.compiler; cmake = [string]$sdkInfo.cmake; ninja = [string]$sdkInfo.ninja; compilerVersion = [string]$sdkInfo.compilerVersion; compilerTarget = [string]$sdkInfo.compilerTarget }
        binaries = [ordered]@{ editor = Get-Sha256 (Join-Path $stageBin 'ProtoEditor.exe'); package = Get-Sha256 (Join-Path $stageBin 'ProtoPackage.exe'); project = Get-Sha256 (Join-Path $stageBin 'ProtoProject.exe'); player = Get-Sha256 (Join-Path $stageBin 'ProtoPlayer.exe'); demo = Get-Sha256 $stagedDemoExecutable.FullName }
        demo = [ordered]@{ name = [string]$demoManifest.name; projectId = [string]$demoManifest.projectId; startupScene = [string]$demoManifest.startupScene; player = $playerRelative }
        files = @($fileEntries)
    }
    Write-Json $manifest (Join-Path $stage 'distribution-manifest.json')

    if (Test-Path -LiteralPath $output) { throw "Distribution destination appeared during staging: $output" }
    [System.IO.Directory]::Move($stage, $output)
    $published = $true
    Write-Output "M8 distribution published: $output"
    Write-Output "Content SHA-256: $contentHash"
} catch {
    throw
} finally {
    if (-not $published -and (Test-Path -LiteralPath $stage)) {
        Assert-OwnedStagePath $stage $outputParent $output
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
