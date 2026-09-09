[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string] $EditorPath,

    [Parameter(Mandatory = $true, Position = 1)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$editor = [System.IO.Path]::GetFullPath($EditorPath)
$output = [System.IO.Path]::GetFullPath($OutputPath)
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$starterPath = Join-Path $repo 'examples\Starter.scene.json'

if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "Editor executable was not found: $editor"
}
if (-not (Test-Path -LiteralPath $starterPath -PathType Leaf)) {
    throw "Starter scene was not found: $starterPath"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
$runDirectory = Join-Path $output ('m3-dim-batch-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
$summaryPath = Join-Path $output 'm3-dim-batch-report.json'

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)] $Value,
        [Parameter(Mandatory = $true)] [string] $Path
    )
    $json = $Value | ConvertTo-Json -Depth 100
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Quote-ProcessArgument {
    param([Parameter(Mandatory = $true)] [string] $Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}

function New-DirectionalLightEntity {
    param([Parameter(Mandatory = $true)] [int] $Index)
    $id = ('10000000-0000-4000-8000-{0:x12}' -f $Index)
    return [ordered]@{
        id = $id
        name = ('DimSun-{0:d3}' -f $Index)
        parent = $null
        enabled = $true
        transform = [ordered]@{
            position = @(0.0, 0.0, 0.0)
            scale = @(1.0, 1.0, 1.0)
            # Scene JSON uses x,y,z,w. Scene validation normalizes this fixed
            # quaternion, which rotates local -Z to the inclined sun ray.
            rotation = @(-0.38, 0.16, 0.07, 0.91)
        }
        components = [ordered]@{
            directionalLight = [ordered]@{
                color = @(1.0, 1.0, 1.0)
                intensity = 0.0013
                shadows = $true
            }
        }
        behaviors = @()
    }
}

function New-DimBatchScene {
    param(
        [Parameter(Mandatory = $true)] [uint32] $ShadowPoolMiB,
        [Parameter(Mandatory = $true)] [bool] $WithLights
    )
    $scene = Get-Content -LiteralPath $starterPath -Raw | ConvertFrom-Json
    $planeId = '00000000-0000-4000-8000-000000000002'
    $cubeId = '00000000-0000-4000-8000-000000000001'
    foreach ($entity in @($scene.entities)) {
        $meshProperty = $entity.components.PSObject.Properties['meshRenderer']
        if ($null -ne $meshProperty -and $meshProperty.Value.mesh -eq $planeId) {
            # This is the native primitive material path: AssetRenderer binds
            # its native roughness (.65) and this debugColor is its albedo.
            $entity.components.meshRenderer.debugColor = @(0.5, 0.5, 0.5, 1.0)
            $entity.components.meshRenderer.castShadows = $true
            $entity.components.meshRenderer.receiveShadows = $true
        } elseif ($null -ne $meshProperty -and $meshProperty.Value.mesh -eq $cubeId) {
            $entity.components.meshRenderer.debugColor = @(0.7, 0.7, 0.7, 1.0)
            $entity.components.meshRenderer.castShadows = $true
            $entity.components.meshRenderer.receiveShadows = $true
        }
    }
    $lighting = [pscustomobject][ordered]@{
        shadowResolution = 256
        shadowCascades = 1
        shadowPoolMiB = $ShadowPoolMiB
        shadowDistance = 40.0
        depthBias = 0.001
        normalBias = 0.015
        ambient = 1.0
        shadows = $true
        filteredShadows = $false
        referenceLighting = $false
        forceShadowRefresh = $false
    }
    $scene | Add-Member -MemberType NoteProperty -Name lighting -Value $lighting -Force
    if ($WithLights) {
        $entities = @($scene.entities)
        for ($index = 1; $index -le 128; ++$index) {
            $entities += New-DirectionalLightEntity -Index $index
        }
        $scene.entities = $entities
    }
    return $scene
}

function Invoke-EditorScene {
    param(
        [Parameter(Mandatory = $true)] [string] $Label,
        [Parameter(Mandatory = $true)] [string] $ScenePath
    )
    $capturePath = Join-Path $runDirectory ($Label + '.png')
    $reportPath = Join-Path $runDirectory ($Label + '.report.json')
    $arguments = @(
        '--scene', (Quote-ProcessArgument $ScenePath),
        '--scene-camera',
        '--frames', '12',
        '--capture', (Quote-ProcessArgument $capturePath),
        '--report', (Quote-ProcessArgument $reportPath)
    )
    $commandLine = $arguments -join ' '
    $process = Start-Process -FilePath $editor -ArgumentList $commandLine -WorkingDirectory (Split-Path -Parent $editor) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(240000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "$Label editor run exceeded the 240 second bound"
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) {
        throw "$Label editor run failed with exit code $($process.ExitCode)"
    }
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "$Label editor run did not write a report"
    }
    $viewportPath = Join-Path ([System.IO.Path]::GetDirectoryName($capturePath)) (([System.IO.Path]::GetFileNameWithoutExtension($capturePath)) + '-viewport.png')
    if (-not (Test-Path -LiteralPath $viewportPath -PathType Leaf)) {
        throw "$Label editor run did not write the viewport capture"
    }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    return [pscustomobject]@{
        Label = $Label
        ScenePath = $ScenePath
        CapturePath = $capturePath
        ViewportPath = $viewportPath
        ReportPath = $reportPath
        Report = $report
    }
}

function Assert-Report {
    param(
        [Parameter(Mandatory = $true)] $Run,
        [Parameter(Mandatory = $true)] [int] $ExpectedLights,
        [Parameter(Mandatory = $true)] [int] $ExpectedBatches,
        [Parameter(Mandatory = $true)] [uint64] $ShadowBudgetBytes
    )
    $report = $Run.Report
    if (-not $report.passed) { throw "$($Run.Label) report.passed was false" }
    if ([int]$report.validation_errors -ne 0) { throw "$($Run.Label) reported validation errors: $($report.validation_errors)" }
    if (-not $report.capture_saved) { throw "$($Run.Label) did not report capture_saved" }
    if ([int]$report.rendered_frames -lt 12) { throw "$($Run.Label) rendered fewer than 12 frames" }
    if ([int]$report.scene_gpu.geometry_pixels -le 100) { throw "$($Run.Label) reported no native geometry" }
    if ([int]$report.lighting.lights -ne $ExpectedLights) {
        throw "$($Run.Label) reported $($report.lighting.lights) lights, expected $ExpectedLights"
    }
    if ([int]$report.lighting.batches -ne $ExpectedBatches) {
        throw "$($Run.Label) reported $($report.lighting.batches) batches, expected $ExpectedBatches"
    }
    if ([uint64]$report.lighting.shadow_bytes -gt $ShadowBudgetBytes) {
        throw "$($Run.Label) shadow allocation $($report.lighting.shadow_bytes) exceeds budget $ShadowBudgetBytes"
    }
}

function Add-Type-PngReader {
    if ('ProtoNativePng' -as [type]) { return }
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public sealed class ProtoNativeImage {
    public int Width { get; set; }
    public int Height { get; set; }
    public byte[] Rgba { get; set; }
}

public static class ProtoNativePng {
    public static ProtoNativeImage Read(string path) {
        using (var source = new Bitmap(path))
        using (var bitmap = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb)) {
            using (var graphics = Graphics.FromImage(bitmap))
                graphics.DrawImageUnscaled(source, 0, 0);
            var rectangle = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
            var data = bitmap.LockBits(rectangle, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                var stride = Math.Abs(data.Stride);
                var bytes = new byte[stride * bitmap.Height];
                Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
                var rgba = new byte[bitmap.Width * bitmap.Height * 4];
                for (var y = 0; y < bitmap.Height; ++y) {
                    var sourceRow = data.Stride >= 0 ? y * stride : (bitmap.Height - 1 - y) * stride;
                    for (var x = 0; x < bitmap.Width; ++x) {
                        var sourceAt = sourceRow + x * 4;
                        var targetAt = (y * bitmap.Width + x) * 4;
                        rgba[targetAt + 0] = bytes[sourceAt + 2];
                        rgba[targetAt + 1] = bytes[sourceAt + 1];
                        rgba[targetAt + 2] = bytes[sourceAt + 0];
                        rgba[targetAt + 3] = bytes[sourceAt + 3];
                    }
                }
                return new ProtoNativeImage { Width = bitmap.Width, Height = bitmap.Height, Rgba = rgba };
            } finally {
                bitmap.UnlockBits(data);
            }
        }
    }
}
'@
}

function Compare-RgbImages {
    param(
        [Parameter(Mandatory = $true)] [string] $ExpectedPath,
        [Parameter(Mandatory = $true)] [string] $ActualPath
    )
    $expected = [ProtoNativePng]::Read($ExpectedPath)
    $actual = [ProtoNativePng]::Read($ActualPath)
    if ($expected.Width -ne $actual.Width -or $expected.Height -ne $actual.Height) {
        throw "Viewport dimensions differ: $ExpectedPath is $($expected.Width)x$($expected.Height), $ActualPath is $($actual.Width)x$($actual.Height)"
    }
    $maxDifference = 0
    [uint64]$differenceSum = 0
    [uint64]$differingChannels = 0
    [uint64]$differingPixels = 0
    $pixelCount = $expected.Width * $expected.Height
    for ($pixel = 0; $pixel -lt $pixelCount; ++$pixel) {
        $base = $pixel * 4
        $pixelDiffered = $false
        for ($channel = 0; $channel -lt 3; ++$channel) {
            $difference = [Math]::Abs([int]$expected.Rgba[$base + $channel] - [int]$actual.Rgba[$base + $channel])
            if ($difference -gt 0) { ++$differingChannels; $pixelDiffered = $true }
            if ($difference -gt $maxDifference) { $maxDifference = $difference }
            $differenceSum += [uint64]$difference
        }
        if ($pixelDiffered) { ++$differingPixels }
    }
    return [pscustomobject]@{
        Width = $expected.Width
        Height = $expected.Height
        MaxDiff = $maxDifference
        MeanAbsDiff = [double]$differenceSum / [double]($pixelCount * 3)
        DifferingChannels = $differingChannels
        DifferingPixels = $differingPixels
    }
}

function Find-LightingContribution {
    param(
        [Parameter(Mandatory = $true)] [string] $BaselinePath,
        [Parameter(Mandatory = $true)] [string] $LitPath
    )
    $baseline = [ProtoNativePng]::Read($BaselinePath)
    $lit = [ProtoNativePng]::Read($LitPath)
    if ($baseline.Width -ne $lit.Width -or $baseline.Height -ne $lit.Height) {
        throw 'Baseline and lit viewport dimensions differ'
    }
    $maxBrightening = 0
    $brightenedChannels = 0
    $startRow = [int][Math]::Floor($lit.Height * 0.5)
    for ($y = $startRow; $y -lt $lit.Height; ++$y) {
        for ($x = 0; $x -lt $lit.Width; ++$x) {
            $base = ($y * $lit.Width + $x) * 4
            for ($channel = 0; $channel -lt 3; ++$channel) {
                $difference = [int]$lit.Rgba[$base + $channel] - [int]$baseline.Rgba[$base + $channel]
                if ($difference -gt $maxBrightening) { $maxBrightening = $difference }
                if ($difference -ge 2) { ++$brightenedChannels }
            }
        }
    }
    if ($maxBrightening -lt 2) {
        throw "Dim directional lights did not brighten any lower-half viewport channel by at least 2 (max=$maxBrightening)"
    }
    return [pscustomobject]@{
        MaxBrightening = $maxBrightening
        BrightenedChannels = $brightenedChannels
    }
}

$summary = [ordered]@{
    status = 'running'
    editor = $editor
    output = $output
    run_directory = $runDirectory
    scenes = @{}
}

try {
    $baselineScenePath = Join-Path $runDirectory 'baseline.scene.json'
    $largeScenePath = Join-Path $runDirectory 'large-pool.scene.json'
    $smallScenePath = Join-Path $runDirectory 'small-pool.scene.json'
    Write-Utf8Json -Value (New-DimBatchScene -ShadowPoolMiB 256 -WithLights $false) -Path $baselineScenePath
    Write-Utf8Json -Value (New-DimBatchScene -ShadowPoolMiB 256 -WithLights $true) -Path $largeScenePath
    Write-Utf8Json -Value (New-DimBatchScene -ShadowPoolMiB 2 -WithLights $true) -Path $smallScenePath

    $baselineRun = Invoke-EditorScene -Label 'baseline' -ScenePath $baselineScenePath
    $largeRun = Invoke-EditorScene -Label 'large-pool' -ScenePath $largeScenePath
    $smallRun = Invoke-EditorScene -Label 'small-pool' -ScenePath $smallScenePath
    Assert-Report -Run $baselineRun -ExpectedLights 0 -ExpectedBatches 0 -ShadowBudgetBytes ([uint64]256 * 1024 * 1024)
    Assert-Report -Run $largeRun -ExpectedLights 128 -ExpectedBatches 1 -ShadowBudgetBytes ([uint64]256 * 1024 * 1024)
    Assert-Report -Run $smallRun -ExpectedLights 128 -ExpectedBatches 128 -ShadowBudgetBytes ([uint64]2 * 1024 * 1024)

    Add-Type-PngReader
    $imageComparison = Compare-RgbImages -ExpectedPath $largeRun.ViewportPath -ActualPath $smallRun.ViewportPath
    if ($imageComparison.MaxDiff -gt 1) {
        throw "Large-pool and small-pool viewport RGB differ by $($imageComparison.MaxDiff), expected maxdiff <= 1"
    }
    $lightingContribution = Find-LightingContribution -BaselinePath $baselineRun.ViewportPath -LitPath $largeRun.ViewportPath
    $summary.status = 'passed'
    $summary.image_comparison = $imageComparison
    $summary.lighting_contribution = $lightingContribution
    $summary.runs = @(
        [ordered]@{ label = $baselineRun.Label; scene = $baselineRun.ScenePath; report = $baselineRun.ReportPath; viewport = $baselineRun.ViewportPath },
        [ordered]@{ label = $largeRun.Label; scene = $largeRun.ScenePath; report = $largeRun.ReportPath; viewport = $largeRun.ViewportPath },
        [ordered]@{ label = $smallRun.Label; scene = $smallRun.ScenePath; report = $smallRun.ReportPath; viewport = $smallRun.ViewportPath }
    )
    Write-Utf8Json -Value $summary -Path $summaryPath
    Write-Output ("M3 dim batch passed: maxdiff={0}, mean={1}, differingChannels={2}, maxBrightening={3}" -f
        $imageComparison.MaxDiff, $imageComparison.MeanAbsDiff, $imageComparison.DifferingChannels, $lightingContribution.MaxBrightening)
    exit 0
} catch {
    $summary.status = 'failed'
    $summary.error = $_.Exception.Message
    try { Write-Utf8Json -Value $summary -Path $summaryPath } catch { }
    Write-Error $_
    exit 1
}
