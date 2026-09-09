[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $BuildRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $SourceRoot,

    [Parameter(Mandatory = $false)]
    [string] $ValidationDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$build = [System.IO.Path]::GetFullPath($BuildRoot)
$sourceArgument = [System.IO.Path]::GetFullPath($SourceRoot)
$packageTool = Join-Path $build 'bin\ProtoPackage.exe'
$sdk = Join-Path $build 'sdk'

if (-not (Test-Path -LiteralPath $packageTool -PathType Leaf)) {
    throw "ProtoPackage.exe was not found: $packageTool"
}
if (-not (Test-Path -LiteralPath $sdk -PathType Container)) {
    throw "Installed SDK was not found: $sdk"
}
$maskDescriptor = Join-Path $build 'test-results\m6-mask-package.json'
if (-not (Test-Path -LiteralPath $maskDescriptor -PathType Leaf)) {
    throw "Static MASK package descriptor was not found: $maskDescriptor"
}
$maskInfo = Get-Content -LiteralPath $maskDescriptor -Raw | ConvertFrom-Json
$graphicsPackageRoot = [System.IO.Path]::GetFullPath([string]$maskInfo.root)
if (-not (Test-Path -LiteralPath (Join-Path $graphicsPackageRoot 'Data\runtime.json') -PathType Leaf)) {
    throw "Static MASK package root is invalid: $graphicsPackageRoot"
}
$graphicsPlayerItem = Get-ChildItem -LiteralPath $graphicsPackageRoot -Filter '*.exe' -File | Select-Object -First 1
if ($null -eq $graphicsPlayerItem) { throw "Static MASK package contains no executable" }
$graphicsPlayer = $graphicsPlayerItem.FullName

$sample = $sourceArgument
if (-not (Test-Path -LiteralPath (Join-Path $sample 'project.proto.json') -PathType Leaf)) {
    $candidate = Join-Path $sample 'examples\ProtoStandalone'
    if (Test-Path -LiteralPath (Join-Path $candidate 'project.proto.json') -PathType Leaf) {
        $sample = $candidate
    } else {
        throw "SourceRoot is neither ProtoStandalone nor a repository containing examples\ProtoStandalone: $sourceArgument"
    }
}

$validation = $null
if (-not [string]::IsNullOrWhiteSpace($ValidationDir)) {
    $validation = [System.IO.Path]::GetFullPath($ValidationDir)
    if (-not (Test-Path -LiteralPath (Join-Path $validation 'VkLayer_khronos_validation.json') -PathType Leaf)) {
        throw "Vulkan validation directory is missing VkLayer_khronos_validation.json: $validation"
    }
}

$runRoot = Join-Path $build 'test-results\m6-player-smoke'
$runId = [Guid]::NewGuid().ToString('N')
$artifact = Join-Path $runRoot ("run-$runId-Україна spaces")
$authored = Join-Path $artifact 'Authored Clone Україна spaces'
$offlineAuthored = Join-Path $artifact 'Authored Offline Україна spaces'
$exported = Join-Path $artifact 'Exported Package Україна spaces'
$copiedPackage = Join-Path $artifact 'Copied Package Україна spaces'
$unrelatedCwd = Join-Path $artifact 'Unrelated CWD spaces'
New-Item -ItemType Directory -Force -Path $artifact, $unrelatedCwd | Out-Null

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)] $Value,
        [Parameter(Mandatory = $true)] [string] $Path
    )
    $text = $Value | ConvertTo-Json -Depth 100
    [System.IO.File]::WriteAllText($Path, $text, [System.Text.UTF8Encoding]::new($false))
}

function Assert-UnderRoot {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [string] $Root,
        [Parameter(Mandatory = $true)] [string] $Label
    )
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its owned root: $fullPath"
    }
    return $fullPath
}

function Quote-WindowsArgument {
    param([Parameter(Mandatory = $true)] [string] $Value)
    if ($Value.Length -eq 0) { return '""' }
    $needsQuotes = $Value.IndexOfAny([char[]]@(' ', "`t", '"')) -ge 0
    if (-not $needsQuotes) { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    $appendSlashes = {
        param([int] $Count)
        for ($index = 0; $index -lt $Count; ++$index) { [void]$builder.Append('\') }
    }
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            ++$slashes
            continue
        }
        if ($character -eq '"') {
            & $appendSlashes (2 * $slashes + 1)
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) { & $appendSlashes $slashes }
        [void]$builder.Append($character)
        $slashes = 0
    }
    if ($slashes -gt 0) { & $appendSlashes (2 * $slashes) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)] [string] $FilePath,
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [string] $WorkingDirectory,
        [Parameter(Mandatory = $true)] [int] $TimeoutMilliseconds,
        [Parameter(Mandatory = $false)] [string] $ProcessPath
    )
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.Arguments = (($Arguments | ForEach-Object { Quote-WindowsArgument $_ }) -join ' ')
    $start.WorkingDirectory = $WorkingDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    if ($PSBoundParameters.ContainsKey('ProcessPath') -and -not [string]::IsNullOrWhiteSpace($ProcessPath)) {
        $start.EnvironmentVariables['PATH'] = $ProcessPath
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Could not start process: $FilePath" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = $false
    try {
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {} }
            $process.WaitForExit(10000) | Out-Null
            throw "Process exceeded the $TimeoutMilliseconds ms bound: $FilePath (PID $($process.Id))"
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Stdout = $stdout; Stderr = $stderr; Pid = $process.Id }
    } finally {
        if ($timedOut -and -not $process.HasExited) {
            try { $process.Kill($true) } catch {}
        }
        $process.Dispose()
    }
}

function Copy-ProjectClone {
    param([Parameter(Mandatory = $true)] [string] $Source, [Parameter(Mandatory = $true)] [string] $Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($folder in @('Assets', 'Scenes', 'Code', 'Config')) {
        $sourceFolder = Join-Path $Source $folder
        if (-not (Test-Path -LiteralPath $sourceFolder -PathType Container)) { throw "Source folder missing: $sourceFolder" }
        $sourceItem = Get-Item -LiteralPath $sourceFolder
        if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Source folder is a reparse point: $sourceFolder"
        }
        Copy-Item -LiteralPath $sourceFolder -Destination (Join-Path $Destination $folder) -Recurse -Force
    }
    Copy-Item -LiteralPath (Join-Path $Source 'project.proto.json') -Destination (Join-Path $Destination 'project.proto.json') -Force
    New-Item -ItemType Directory -Force -Path (Join-Path $Destination '.proto') | Out-Null
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)] [string] $Path)
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-Entity {
    param([Parameter(Mandatory = $true)] $Scene, [Parameter(Mandatory = $true)] [string] $Id)
    $found = @($Scene.entities | Where-Object { $_.id -eq $Id })
    if ($found.Count -ne 1) { throw "Expected exactly one scene entity $Id, found $($found.Count)" }
    return $found[0]
}

function Assert-Changed {
    param([Parameter(Mandatory = $true)] $Before, [Parameter(Mandatory = $true)] $After, [Parameter(Mandatory = $true)] [string] $Label)
    $beforeText = ($Before | ConvertTo-Json -Depth 100 -Compress)
    $afterText = ($After | ConvertTo-Json -Depth 100 -Compress)
    if ($beforeText -eq $afterText) { throw "$Label did not change" }
}

function Invoke-Player {
    param(
        [Parameter(Mandatory = $true)] [string] $Label,
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [int] $TimeoutMilliseconds,
        [Parameter(Mandatory = $false)] [string] $PlayerPath = $player
    )
    $reportPath = Join-Path $artifact ($Label + '.report.json')
    $fullArguments = @($Arguments + @('--report', $reportPath))
    $systemPath = Join-Path $env:SystemRoot 'System32'
    $environmentPath = $systemPath
    $result = Invoke-BoundedProcess -FilePath $PlayerPath -Arguments $fullArguments -WorkingDirectory $unrelatedCwd `
        -TimeoutMilliseconds $TimeoutMilliseconds -ProcessPath $environmentPath
    if ($result.ExitCode -ne 0) {
        throw "$Label Player failed with exit code $($result.ExitCode): $($result.Stderr)"
    }
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "$Label did not write a report" }
    $report = Read-JsonFile $reportPath
    if (-not $report.passed) { throw "$Label report.passed was false: $($report.error)" }
    return [pscustomobject]@{ Label = $Label; ReportPath = $reportPath; Report = $report; Stdout = $result.Stdout; Stderr = $result.Stderr }
}

function Assert-GraphicsReport {
    param(
        [Parameter(Mandatory = $true)] $Run,
        [Parameter(Mandatory = $true)] [uint32] $SunResolution,
        [Parameter(Mandatory = $true)] [uint32] $SunCascades,
        [Parameter(Mandatory = $true)] [uint32] $PointResolution,
        [Parameter(Mandatory = $true)] [uint32] $PointPoolMiB,
        [Parameter(Mandatory = $true)] [double] $RenderScale,
        [Parameter(Mandatory = $false)] [int] $TextureTopMipDrop = -1
    )
    $g = $Run.Report.graphics
    if ($Run.Report.validation_errors -ne 0) { throw "$($Run.Label) reported validation errors" }
    if ([uint32]$g.sunResolution -ne $SunResolution -or [uint32]$g.sunCascades -ne $SunCascades) {
        throw "$($Run.Label) sun settings do not match the user configuration"
    }
    if ([uint32]$g.pointResolution -ne $PointResolution -or [uint32]$g.pointPoolMiB -ne $PointPoolMiB) {
        throw "$($Run.Label) point settings do not match the user configuration"
    }
    if ([Math]::Abs([double]$g.renderScale - $RenderScale) -gt 0.001) { throw "$($Run.Label) render scale mismatch" }
    if ($TextureTopMipDrop -ge 0 -and [uint32]$g.textureTopMipDrop -ne [uint32]$TextureTopMipDrop) {
        throw "$($Run.Label) texture mip policy mismatch"
    }
    if ([uint32]$g.swapWidth -le 0 -or [uint32]$g.swapHeight -le 0 -or [uint32]$g.viewportWidth -le 0 -or [uint32]$g.viewportHeight -le 0) {
        throw "$($Run.Label) reported invalid extents"
    }
    if ([uint64]$g.mipUploadBytes -eq 0) { throw "$($Run.Label) reported no texture upload" }
    if ([uint64]$Run.Report.shadow_faces_total -eq 0) { throw "$($Run.Label) reported no shadow faces" }
    if ([uint64]$g.sunShadowPoolBytes -eq 0 -or [uint64]$g.pointShadowPoolBytes -eq 0) {
        throw "$($Run.Label) reported an empty directional or point shadow pool"
    }
    $pointBudgetBytes = [uint64]$PointPoolMiB * 1024 * 1024
    if ([uint64]$g.pointShadowPoolBytes -gt $pointBudgetBytes) {
        throw "$($Run.Label) point shadow pool exceeded its configured budget"
    }
}

$null = Assert-UnderRoot -Path $authored -Root $artifact -Label 'Authored clone'
$null = Assert-UnderRoot -Path $exported -Root $artifact -Label 'Export destination'
$null = Assert-UnderRoot -Path $copiedPackage -Root $artifact -Label 'Copied package'
$null = Assert-UnderRoot -Path $offlineAuthored -Root $artifact -Label 'Offline authored clone'
Copy-ProjectClone -Source $sample -Destination $authored
$exportResult = Invoke-BoundedProcess -FilePath $packageTool -Arguments @('--project', $authored, '--sdk', $sdk, '--output', $exported) `
    -WorkingDirectory (Split-Path -Parent $packageTool) -TimeoutMilliseconds 900000
if ($exportResult.ExitCode -ne 0) { throw "ProtoPackage export failed: $($exportResult.Stderr)" }

$null = Assert-UnderRoot -Path $exported -Root $artifact -Label 'Exported package'
Copy-Item -LiteralPath $exported -Destination $copiedPackage -Recurse -Force
if (Test-Path -LiteralPath $offlineAuthored) { throw "Offline authored destination already exists: $offlineAuthored" }
Move-Item -LiteralPath $authored -Destination $offlineAuthored
$playerItem = Get-ChildItem -LiteralPath $copiedPackage -Filter '*.exe' -File | Select-Object -First 1
if ($null -eq $playerItem) { throw "Copied package contains no executable" }
$player = $playerItem.FullName

$headless = Invoke-Player -Label 'headless-portable' -Arguments @('--headless', '--frames', '90', '--fixed-dt', '0.0166667', '--test-input') -TimeoutMilliseconds 60000
if ([bool]$headless.Report.validation_enabled) { throw 'Headless portable run unexpectedly enabled validation' }
if ([int]$headless.Report.updates -lt 90) { throw 'Headless portable run did not reach its frame bound' }
$logPath = [System.IO.Path]::ChangeExtension($headless.ReportPath, '.log')
if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { throw 'Headless Player log was not written' }
if ((Get-Content -LiteralPath $logPath -Raw) -notmatch 'ReadData loaded 9f21b5d0-7f62-4e93-a8b3-68d2c4f11022: Proto Standalone raw payload v1') {
    throw 'ReadData did not observe the packaged raw payload'
}
$initial = $headless.Report.initialScene
$final = $headless.Report.finalScene
$spinId = '50000000-0000-4000-8000-000000000010'
$moveLightId = '004de170-579e-4f2c-8d1f-35903540a74a'
$keyboardId = '50000000-0000-4000-8000-000000000010'
$spinBefore = Get-Entity $initial $spinId
$spinAfter = Get-Entity $final $spinId
$moveBefore = Get-Entity $initial $moveLightId
$moveAfter = Get-Entity $final $moveLightId
$keyboardBefore = Get-Entity $initial $keyboardId
$keyboardAfter = Get-Entity $final $keyboardId
Assert-Changed $spinBefore.transform.rotation $spinAfter.transform.rotation 'Spin rotation'
Assert-Changed $moveBefore.transform.position $moveAfter.transform.position 'MoveLight position'
Assert-Changed $keyboardBefore.transform.position $keyboardAfter.transform.position 'Keyboard input position'

$defaultGraphics = Join-Path $graphicsPackageRoot 'Config\graphics.json'
$defaultHash = (Get-FileHash -LiteralPath $defaultGraphics -Algorithm SHA256).Hash
$normalGpu = Invoke-Player -Label 'gpu-portable' -Arguments @('--frames', '45', '--capture', (Join-Path $artifact 'gpu.png')) -TimeoutMilliseconds 240000
if ([bool]$normalGpu.Report.validation_enabled) { throw 'Normal packaged GPU run unexpectedly enabled validation' }
if (-not (Test-Path -LiteralPath (Join-Path $artifact 'gpu.png') -PathType Leaf)) { throw 'Normal GPU capture is missing' }

$smokeArguments = @('--graphics-smoke', '--frames', '180')
if ($null -ne $validation) { $smokeArguments += @('--validation-dir', $validation) }
$graphicsSmoke = Invoke-Player -Label 'graphics-smoke' -PlayerPath $graphicsPlayer -Arguments $smokeArguments -TimeoutMilliseconds 300000
$smokeReport = $graphicsSmoke.Report
if (-not [bool]$smokeReport.graphics.graphicsSmokeCancel -or -not [bool]$smokeReport.graphics.graphicsSmokeFailure -or -not [bool]$smokeReport.graphics.graphicsSmokeRecovery) {
    throw 'Graphics smoke did not complete cancel/failure/recovery coverage'
}
if ([uint32]$smokeReport.graphics.graphicsSmokeAttempts -lt 5 -or [uint32]$smokeReport.graphics.graphicsSmokeFailureCount -lt 4) {
    throw 'Graphics smoke did not exercise every bounded failure attempt'
}
if ($null -ne $validation -and -not [bool]$smokeReport.validation_enabled) { throw 'Graphics smoke did not enable supplied validation' }
if ($null -eq $validation -and [bool]$smokeReport.validation_enabled) { throw 'Graphics smoke unexpectedly enabled validation' }
Assert-GraphicsReport -Run $graphicsSmoke -SunResolution ([uint32]$smokeReport.graphics.sunResolution) `
    -SunCascades ([uint32]$smokeReport.graphics.sunCascades) -PointResolution ([uint32]$smokeReport.graphics.pointResolution) `
    -PointPoolMiB ([uint32]$smokeReport.graphics.pointPoolMiB) -RenderScale ([double]$smokeReport.graphics.renderScale)
$userGraphics = Join-Path $graphicsPackageRoot 'Config\graphics.user.json'
if (-not (Test-Path -LiteralPath $userGraphics -PathType Leaf)) { throw 'Graphics smoke did not persist user settings' }
$smokeSettings = Read-JsonFile $userGraphics
if ($smokeSettings.profile -ne 'Custom') { throw 'Graphics smoke did not persist Custom settings' }
$restart = Invoke-Player -Label 'graphics-restart' -PlayerPath $graphicsPlayer -Arguments @('--frames', '35') -TimeoutMilliseconds 240000
Assert-GraphicsReport -Run $restart -SunResolution ([uint32]$smokeSettings.sunShadows.resolution) `
    -SunCascades ([uint32]$smokeSettings.sunShadows.cascades) -PointResolution ([uint32]$smokeSettings.pointShadows.resolution) `
    -PointPoolMiB ([uint32]$smokeSettings.pointShadows.poolBudgetMiB) -RenderScale ([double]$smokeSettings.renderScale) `
    -TextureTopMipDrop ([int]$smokeSettings.textureTopMipDrop)

$low = Read-JsonFile $defaultGraphics
$low.profile = 'Low'; $low.renderScale = 0.75; $low.sunShadows.resolution = 1024; $low.sunShadows.cascades = 2
$low.pointShadows.resolution = 256; $low.pointShadows.poolBudgetMiB = 48; $low.textureTopMipDrop = 1
$low.viewDistance = 200; $low.sunShadows.distance = 60; $low.pointShadows.filterTaps = 1
Write-Utf8Json $low $userGraphics
$lowRun = Invoke-Player -Label 'graphics-low' -PlayerPath $graphicsPlayer -Arguments @('--frames', '35') -TimeoutMilliseconds 240000
$lowSettings = Read-JsonFile $userGraphics
if ($lowSettings.profile -ne 'Low') { throw 'Low graphics profile was not retained' }
Assert-GraphicsReport -Run $lowRun -SunResolution 1024 -SunCascades 2 -PointResolution 256 -PointPoolMiB 48 -RenderScale 0.75 -TextureTopMipDrop 1

$high = Read-JsonFile $defaultGraphics
$high.profile = 'High'; $high.renderScale = 1.0; $high.sunShadows.resolution = 2048; $high.sunShadows.cascades = 4
$high.pointShadows.resolution = 1024; $high.pointShadows.poolBudgetMiB = 192; $high.textureTopMipDrop = 0
$high.viewDistance = 500; $high.sunShadows.distance = 100
Write-Utf8Json $high $userGraphics
$highRun = Invoke-Player -Label 'graphics-high' -PlayerPath $graphicsPlayer -Arguments @('--frames', '35') -TimeoutMilliseconds 240000
$highSettings = Read-JsonFile $userGraphics
if ($highSettings.profile -ne 'High') { throw 'High graphics profile was not retained' }
Assert-GraphicsReport -Run $highRun -SunResolution 2048 -SunCascades 4 -PointResolution 1024 -PointPoolMiB 192 -RenderScale 1.0 -TextureTopMipDrop 0
if ([uint32]$lowRun.Report.graphics.viewportWidth -ge [uint32]$highRun.Report.graphics.viewportWidth -or
    [uint32]$lowRun.Report.graphics.viewportHeight -ge [uint32]$highRun.Report.graphics.viewportHeight) {
    throw 'Low graphics render scale did not reduce the reported viewport extent'
}

$custom = Read-JsonFile $userGraphics
$custom.profile = 'Custom'; $custom.vSync = $false; $custom.renderScale = 0.5
Write-Utf8Json $custom $userGraphics
$customRun = Invoke-Player -Label 'graphics-custom' -PlayerPath $graphicsPlayer -Arguments @('--frames', '35') -TimeoutMilliseconds 120000
Assert-GraphicsReport -Run $customRun -SunResolution 2048 -SunCascades 4 -PointResolution 1024 -PointPoolMiB 192 -RenderScale 0.5 -TextureTopMipDrop 0
if ([bool]$customRun.Report.graphics.vSync -or [int]$customRun.Report.graphics.presentMode -notin @(0, 1, 2)) {
    throw 'Custom VSync setting or actual presentation mode is invalid'
}
# FIFO fallback remains valid on drivers that do not expose a tearing/mailbox mode.
$finalDefaultHash = (Get-FileHash -LiteralPath $defaultGraphics -Algorithm SHA256).Hash
if ($finalDefaultHash -ne $defaultHash) { throw 'Immutable Config/graphics.json was modified by graphics QA' }

$summary = [ordered]@{
    passed = $true
    package = $copiedPackage
    graphicsPackage = $graphicsPackageRoot
    authoredOffline = $offlineAuthored
    unrelatedWorkingDirectory = $unrelatedCwd
    headless = $headless.ReportPath
    gpu = $normalGpu.ReportPath
    graphicsSmoke = $graphicsSmoke.ReportPath
    restart = $restart.ReportPath
    low = $lowRun.ReportPath
    high = $highRun.ReportPath
    custom = $customRun.ReportPath
    validationDirectory = $validation
}
$summaryPath = Join-Path $artifact 'm6-player-smoke-report.json'
Write-Utf8Json $summary $summaryPath
Write-Output ("M6PlayerSmoke PASS: " + $summaryPath)
