# Delivery smoke uses PowerShell 7/.NET 6 for hashing and relative-path checks.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $DistributionRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputRoot,

    [Parameter(Mandatory = $false)]
    [ValidateRange(30, 900)]
    [int] $TimeoutSeconds = 240
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$distribution = [System.IO.Path]::GetFullPath($DistributionRoot)
$output = [System.IO.Path]::GetFullPath($OutputRoot)

function Require-Directory {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "$Label directory is missing: $Path" }
}
function Require-File {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label file is missing: $Path" }
}
function Assert-UnderRoot {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Root,
          [Parameter(Mandatory = $true)][string] $Label)
    $full = [System.IO.Path]::GetFullPath($Path)
    $base = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label escapes its owned root: $full"
    }
    return $full
}
function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-Utf8 {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][AllowEmptyString()][string] $Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}
function Save-Json {
    param([Parameter(Mandatory = $true)] $Value, [Parameter(Mandatory = $true)][string] $Path)
    Write-Utf8 $Path ($Value | ConvertTo-Json -Depth 32)
}
function Quote-WindowsArgument {
    param([Parameter(Mandatory = $true)][string] $Value)
    if ($Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { ++$slashes; continue }
        if ($character -eq '"') {
            for ($index = 0; $index -lt (2 * $slashes + 1); ++$index) { [void]$builder.Append('\') }
            [void]$builder.Append('"')
        } else {
            for ($index = 0; $index -lt $slashes; ++$index) { [void]$builder.Append('\') }
            [void]$builder.Append($character)
        }
        $slashes = 0
    }
    for ($index = 0; $index -lt (2 * $slashes); ++$index) { [void]$builder.Append('\') }
    [void]$builder.Append('"')
    return $builder.ToString()
}
function Copy-DirectoryContents {
    param([Parameter(Mandatory = $true)][string] $Source, [Parameter(Mandatory = $true)][string] $Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath $Source -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
    }
}
function Relative-Path {
    param([Parameter(Mandatory = $true)][string] $Root, [Parameter(Mandatory = $true)][string] $Path)
    return ([System.IO.Path]::GetRelativePath($Root, $Path)).Replace('\', '/')
}
function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string] $FilePath,
        [Parameter(Mandatory = $true)][string[]] $Arguments,
        [Parameter(Mandatory = $true)][string] $WorkingDirectory,
        [Parameter(Mandatory = $true)][string] $Label,
        [Parameter(Mandatory = $true)][string] $ArtifactRoot
    )
    Assert-UnderRoot $WorkingDirectory $ArtifactRoot "$Label working directory" | Out-Null
    $stdoutPath = Join-Path $ArtifactRoot ($Label + '.stdout.log')
    $stderrPath = Join-Path $ArtifactRoot ($Label + '.stderr.log')
    $metadataPath = Join-Path $ArtifactRoot ($Label + '.process.json')
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.Arguments = (@($Arguments) | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' '
    $start.WorkingDirectory = $WorkingDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PATH'] = Join-Path $env:SystemRoot 'System32'
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $stdout = ''
    $stderr = ''
    $exitCode = -1
    $timedOut = $false
    $errorText = $null
    $started = $false
    $stdoutTask = $null
    $stderrTask = $null
    try {
        if (-not $process.Start()) { throw "Could not start ${Label}: $FilePath" }
        $started = $true
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
            [void]$process.WaitForExit(10000)
            throw "$Label exceeded the $TimeoutSeconds second bound."
        }
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
    } catch {
        $errorText = $_.Exception.Message
        if ($started -and -not $process.HasExited) {
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
            try { [void]$process.WaitForExit(10000) } catch {}
        }
        try { if ($stdoutTask) { $stdout = $stdoutTask.GetAwaiter().GetResult() } } catch {}
        try { if ($stderrTask) { $stderr = $stderrTask.GetAwaiter().GetResult() } } catch {}
        try { if ($process.HasExited) { $exitCode = $process.ExitCode } } catch {}
    } finally {
        $timer.Stop()
        Write-Utf8 $stdoutPath $stdout
        Write-Utf8 $stderrPath $stderr
        $executableHash = if (Test-Path -LiteralPath $FilePath -PathType Leaf) { Get-Sha256 $FilePath } else { $null }
        Save-Json ([ordered]@{
                format = 'proto.m8.process'; version = 1; label = $Label; executable = $FilePath
                executableSha256 = $executableHash
                arguments = @($Arguments); commandLine = ((@($FilePath) + @($Arguments)) |
                    ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' '
                workingDirectory = $WorkingDirectory; path = $start.EnvironmentVariables['PATH']
                durationMs = $timer.Elapsed.TotalMilliseconds; exitCode = $exitCode; timedOut = $timedOut
                error = $errorText; stdout = $stdoutPath; stderr = $stderrPath
            }) $metadataPath
        $process.Dispose()
    }
    return [pscustomobject]@{
        label = $Label; executable = $FilePath; arguments = @($Arguments); exitCode = $exitCode
        timedOut = $timedOut; error = $errorText; stdout = $stdoutPath; stderr = $stderrPath; metadata = $metadataPath
    }
}
function Assert-Success($Result, [string] $Label) {
    if ($Result.exitCode -ne 0 -or $Result.timedOut -or $Result.error) {
        throw "$Label failed with exit code $($Result.exitCode): $($Result.error)"
    }
}
function Get-Json($Path) {
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}
function Assert-ProjectCreated {
    param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $Label)
    Require-Directory $Path $Label
    foreach ($relative in @('project.proto.json', 'Assets', 'Scenes', 'Code', 'Code/Behaviors.cpp', 'Config', 'Config/graphics.json')) {
        $candidate = Join-Path $Path $relative
        if ($relative -match '\.') { Require-File $candidate "$Label $relative" } else { Require-Directory $candidate "$Label $relative" }
    }
    $manifest = Get-Json (Join-Path $Path 'project.proto.json')
    if ($manifest.format -ne 'proto.project' -or $manifest.engine.sdkVersion -ne '0.1' -or
        $manifest.code.sourceRoot -ne 'Code' -or $manifest.code.registrationFile -ne 'Code/Behaviors.cpp' -or
        $manifest.graphicsSettings -ne 'Config/graphics.json' -or $manifest.build.target -ne 'windows-x64') {
        throw "$Label has an invalid v0.1 project contract."
    }
    $sceneFiles = @(Get-ChildItem -LiteralPath (Join-Path $Path 'Scenes') -Filter '*.scene.json' -File -Recurse)
    $matches = @($sceneFiles | ForEach-Object { Get-Json $_.FullName } |
        Where-Object { $_.sceneId -eq $manifest.startupScene })
    if ($matches.Count -ne 1) { throw "$Label startupScene does not resolve to exactly one scene." }
    return [pscustomobject]@{ projectId = [string]$manifest.projectId; startupScene = [string]$manifest.startupScene }
}
function Verify-DistributionManifest {
    param([Parameter(Mandatory = $true)][string] $Root)
    foreach ($item in @((Get-Item -LiteralPath $Root -Force)) +
            @(Get-ChildItem -LiteralPath $Root -Recurse -Force)) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Distribution contains a reparse point: $($item.FullName)"
        }
    }
    $manifestPath = Join-Path $Root 'distribution-manifest.json'
    Require-File $manifestPath 'Distribution manifest'
    $manifest = Get-Json $manifestPath
    if ($manifest.format -ne 'proto.distribution' -or $manifest.version -ne 1) {
        throw 'Distribution manifest format/version is invalid.'
    }
    $material = [Text.StringBuilder]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($manifest.files)) {
        if (-not $seen.Add([string]$entry.path)) { throw "Duplicate distribution manifest entry: $($entry.path)" }
        $relative = [string]$entry.path
        $path = [System.IO.Path]::GetFullPath((Join-Path $Root ($relative.Replace('/', '\'))))
        Assert-UnderRoot $path $Root 'Distribution manifest entry' | Out-Null
        Require-File $path "Manifest file $relative"
        $hash = Get-Sha256 $path
        if ($hash -ne [string]$entry.sha256 -or [uint64](Get-Item -LiteralPath $path).Length -ne [uint64]$entry.bytes) {
            throw "Distribution manifest hash or size mismatch: $relative"
        }
        [void]$material.Append($relative).Append('|').Append([uint64]$entry.bytes).Append('|').Append($hash).Append("`n")
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
            Where-Object { $_.FullName -ne $manifestPath })) {
        $actualRelative = Relative-Path $Root $file.FullName
        if (-not $seen.Contains($actualRelative)) {
            throw "Unlisted file found in distribution: $actualRelative"
        }
    }
    $expectedBinaries = [ordered]@{
        'bin/ProtoEditor.exe' = [string]$manifest.binaries.editor
        'bin/ProtoPackage.exe' = [string]$manifest.binaries.package
        'bin/ProtoProject.exe' = [string]$manifest.binaries.project
        'bin/ProtoPlayer.exe' = [string]$manifest.binaries.player
        ([string]$manifest.demo.player) = [string]$manifest.binaries.demo
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -Filter '*.exe' -File -Recurse -Force)) {
        $relative = Relative-Path $Root $file.FullName
        if (-not $expectedBinaries.Contains($relative)) {
            throw "Unexpected executable in distribution (compiler/toolchain is forbidden): $relative"
        }
    }
    foreach ($entry in $expectedBinaries.GetEnumerator()) {
        $path = Join-Path $Root ($entry.Key.Replace('/', '\'))
        Require-File $path "Manifest binary $($entry.Key)"
        if ((Get-Sha256 $path) -ne $entry.Value) { throw "Manifest binary hash mismatch: $($entry.Key)" }
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($material.ToString())
    $computed = [Convert]::ToHexString(([Security.Cryptography.SHA256]::Create().ComputeHash($bytes))).ToLowerInvariant()
    if ($computed -ne [string]$manifest.contentSha256) { throw 'Distribution content SHA-256 does not match its file list.' }
    return $manifest
}

Require-Directory $distribution 'Distribution root'
Require-Directory (Join-Path $distribution 'bin') 'Distribution bin'
Require-Directory (Join-Path $distribution 'Templates/ProtoCppEmpty') 'Distribution template'
Require-Directory (Join-Path $distribution 'Examples/ProtoM8Demo') 'Distribution demo'
Require-Directory (Join-Path $distribution 'Player') 'Distribution Player'
Require-File (Join-Path $distribution 'bin/ProtoProject.exe') 'ProtoProject.exe'
Require-File (Join-Path $distribution 'bin/ProtoEditor.exe') 'ProtoEditor.exe'
Require-File (Join-Path $distribution 'bin/ProtoPlayer.exe') 'ProtoPlayer.exe'
Require-File (Join-Path $distribution 'Templates/ProtoCppEmpty/Create-Project.ps1') 'Template creator'
$manifest = Verify-DistributionManifest $distribution

if (Test-Path -LiteralPath $output) {
    Require-Directory $output 'Smoke output root'
} else {
    New-Item -ItemType Directory -Path $output -Force | Out-Null
}
$run = Join-Path $output ('r-' + [Guid]::NewGuid().ToString('N').Substring(0, 12) + '-Україна')
 $distributionWithSeparator = $distribution.TrimEnd('\') + '\'
 $outputWithSeparator = $output.TrimEnd('\') + '\'
 if ($output.StartsWith($distributionWithSeparator, [System.StringComparison]::OrdinalIgnoreCase) -or
     $distribution.StartsWith($outputWithSeparator, [System.StringComparison]::OrdinalIgnoreCase) -or
     $output -eq $distribution) {
     throw 'Smoke OutputRoot must not overlap DistributionRoot.'
 }
New-Item -ItemType Directory -Path $run | Out-Null
$unrelatedCwd = Join-Path $run 'Unrelated CWD spaces'
New-Item -ItemType Directory -Path $unrelatedCwd -Force | Out-Null
$processes = [Collections.Generic.List[object]]::new()
$errorText = $null
$passed = $false
try {
    $templateScript = Join-Path $distribution 'Templates/ProtoCppEmpty/Create-Project.ps1'
    $powerShell = Join-Path $PSHOME 'pwsh.exe'
    if (-not (Test-Path -LiteralPath $powerShell -PathType Leaf)) { $powerShell = Join-Path $PSHOME 'powershell.exe' }
    Require-File $powerShell 'PowerShell host'
    $templateOne = Join-Path $run 'Template One Україна spaces'
    $templateTwo = Join-Path $run 'Template Two Україна spaces'
    foreach ($item in @([pscustomobject]@{ path = $templateOne; name = 'Template One' },
                         [pscustomobject]@{ path = $templateTwo; name = 'Template Two' })) {
        $result = Invoke-BoundedProcess -FilePath $powerShell -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $templateScript,
                '-Destination', $item.path, '-Name', $item.name) -WorkingDirectory $unrelatedCwd -Label ('template-' + $item.name.Replace(' ', '-')) -ArtifactRoot $run
        $processes.Add($result)
        Assert-Success $result "Template creation $($item.name)"
    }
    $oneIdentity = Assert-ProjectCreated $templateOne 'Template One'
    $twoIdentity = Assert-ProjectCreated $templateTwo 'Template Two'
    if ($oneIdentity.projectId -eq $twoIdentity.projectId -or $oneIdentity.startupScene -eq $twoIdentity.startupScene) {
        throw 'Two fresh template projects reused project or startup-scene identity.'
    }
    $collision = Invoke-BoundedProcess -FilePath $powerShell -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $templateScript,
            '-Destination', $templateOne, '-Name', 'Template One Again') -WorkingDirectory $unrelatedCwd -Label 'template-no-overwrite' -ArtifactRoot $run
    $processes.Add($collision)
    if ($collision.timedOut -or $collision.error -or $collision.exitCode -le 0) {
        throw 'Template collision must be a normal, nonzero rejection without a timeout or helper failure.'
    }
    Assert-ProjectCreated $templateOne 'Template One after collision check' | Out-Null

    $relocated = Join-Path $run 'Relocated ProtoEngine Україна spaces'
    Copy-DirectoryContents $distribution $relocated
    Verify-DistributionManifest $relocated | Out-Null

    $starterPackage = Join-Path $run 'Starter Package Україна spaces'
    $package = Invoke-BoundedProcess -FilePath (Join-Path $relocated 'bin/ProtoPackage.exe') -Arguments @(
        '--project', $templateOne, '--sdk', (Join-Path $relocated 'sdk'), '--output', $starterPackage
    ) -WorkingDirectory $unrelatedCwd -Label 'starter-package-build' -ArtifactRoot $run
    $processes.Add($package)
    Assert-Success $package 'Starter project ProtoPackage export'
    Require-File (Join-Path $starterPackage 'Data/runtime.json') 'Starter package runtime manifest'
    $starterPlayer = Join-Path $run 'Starter Player Only Україна spaces'
    Copy-DirectoryContents $starterPackage $starterPlayer
    $starterExecutables = @(Get-ChildItem -LiteralPath $starterPlayer -Filter '*.exe' -File)
    if ($starterExecutables.Count -ne 1) { throw 'Starter package must contain exactly one Player executable.' }
    $starterExecutable = $starterExecutables[0]
    $starterValidate = Invoke-BoundedProcess -FilePath $starterExecutable.FullName -Arguments @('--validate-package') -WorkingDirectory $unrelatedCwd -Label 'starter-player-validate' -ArtifactRoot $run
    $processes.Add($starterValidate)
    Assert-Success $starterValidate 'Starter Player package validation'
    $starterReport = Join-Path $run 'starter-player.json'
    $starterCapture = Join-Path $run 'starter-player.png'
    $starterRun = Invoke-BoundedProcess -FilePath $starterExecutable.FullName -Arguments @(
        '--frames', '12', '--capture', $starterCapture, '--report', $starterReport
    ) -WorkingDirectory $unrelatedCwd -Label 'starter-player-run' -ArtifactRoot $run
    $processes.Add($starterRun)
    Assert-Success $starterRun 'Starter Player frame run'
    Require-File $starterReport 'Starter Player report'
    Require-File $starterCapture 'Starter Player capture'
    if (-not [bool](Get-Json $starterReport).passed) { throw 'Starter Player report did not pass.' }

    $demoManifestPath = Join-Path $relocated 'Examples/ProtoM8Demo/project.proto.json'
    Require-File $demoManifestPath 'Relocated demo manifest'
    $editorReport = Join-Path $run 'editor-relocated.json'
    $editorCapture = Join-Path $run 'editor-relocated.png'
    $editor = Invoke-BoundedProcess -FilePath (Join-Path $relocated 'bin/ProtoEditor.exe') -Arguments @(
        '--project', $demoManifestPath, '--scene-camera', '--frames', '12', '--capture', $editorCapture, '--report', $editorReport
    ) -WorkingDirectory $unrelatedCwd -Label 'editor-relocated' -ArtifactRoot $run
    $processes.Add($editor)
    Assert-Success $editor 'Relocated Editor'
    Require-File $editorReport 'Relocated Editor report'
    Require-File $editorCapture 'Relocated Editor capture'
    $editorJson = Get-Json $editorReport
    if (-not [bool]$editorJson.passed -or [int]$editorJson.rendered_frames -lt 12) {
        throw 'Relocated Editor did not complete its bounded frame run.'
    }

    $playerSource = Join-Path $relocated 'Player'
    $playerOnly = Join-Path $run 'Player Only Україна spaces'
    Copy-DirectoryContents $playerSource $playerOnly
    if ((Test-Path -LiteralPath (Join-Path $playerOnly 'project.proto.json') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $playerOnly 'Assets') -PathType Container) -or
        (Test-Path -LiteralPath (Join-Path $playerOnly 'Code') -PathType Container)) {
        throw 'Standalone Player copy unexpectedly contains authored project files.'
    }
    $playerExecutables = @(Get-ChildItem -LiteralPath $playerOnly -Filter '*.exe' -File)
    if ($playerExecutables.Count -ne 1) { throw 'Standalone Player copy must contain exactly one executable.' }
    $playerExe = $playerExecutables[0]
    $validate = Invoke-BoundedProcess -FilePath $playerExe.FullName -Arguments @('--validate-package') -WorkingDirectory $unrelatedCwd -Label 'player-validate-package' -ArtifactRoot $run
    $processes.Add($validate)
    Assert-Success $validate 'Player package validation'
    if ((Get-Content -LiteralPath $validate.stdout -Raw) -notmatch '"passed"\s*:\s*true') {
        throw 'Player --validate-package did not report passed=true.'
    }
    $playerReport = Join-Path $run 'player-relocated.json'
    $playerCapture = Join-Path $run 'player-relocated.png'
    $playerArguments = @('--frames', '12', '--capture', $playerCapture, '--report', $playerReport)
    if ($manifest.configuration -eq 'Debug') {
        $playerArguments += @('--validation-dir', (Join-Path $relocated 'bin/validation'))
    }
    $frameRun = Invoke-BoundedProcess -FilePath $playerExe.FullName -Arguments $playerArguments `
        -WorkingDirectory $unrelatedCwd -Label 'player-relocated' -ArtifactRoot $run
    $processes.Add($frameRun)
    Assert-Success $frameRun 'Relocated Player frame run'
    Require-File $playerReport 'Relocated Player report'
    Require-File $playerCapture 'Relocated Player capture'
    $playerJson = Get-Json $playerReport
    if (-not [bool]$playerJson.passed -or [int]$playerJson.frames -lt 12) {
        throw 'Relocated Player did not complete its bounded frame run.'
    }
    if ($manifest.configuration -eq 'Debug' -and
        (-not $editorJson.validation_enabled -or -not $playerJson.validation_enabled -or
         $editorJson.validation_errors -ne 0 -or $playerJson.validation_errors -ne 0)) {
        throw 'Relocated Debug Editor/Player must run with validation enabled and zero errors.'
    }
    $passed = $true
} catch {
    $errorText = $_.Exception.Message
} finally {
    Save-Json ([ordered]@{
            format = 'proto.m8.distribution.smoke'; version = 1; passed = $passed; error = $errorText
            distribution = $distribution; distributionContentSha256 = [string]$manifest.contentSha256
            runRoot = $run; processes = @($processes)
        }) (Join-Path $run 'm8-distribution-smoke.json')
}
if (-not $passed) { throw $errorText }
Write-Output "M8 distribution smoke PASS: $(Join-Path $run 'm8-distribution-smoke.json')"
