[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Destination,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Name
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$templateRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$tool = [System.IO.Path]::GetFullPath((Join-Path $templateRoot '..\..\bin\ProtoProject.exe'))
$destinationPath = [System.IO.Path]::GetFullPath($Destination)

if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    throw "ProtoProject.exe was not found beside this distribution: $tool"
}
if (Test-Path -LiteralPath $destinationPath) {
    throw "Refusing to overwrite an existing project: $destinationPath"
}
if ([string]::IsNullOrWhiteSpace($Name)) {
    throw 'Project name must not be empty.'
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

$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $tool
$start.Arguments = (@('--create', $destinationPath, '--name', $Name) |
    ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' '
$start.WorkingDirectory = Split-Path -Parent $tool
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
$start.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
$start.EnvironmentVariables['PATH'] = Join-Path $env:SystemRoot 'System32'

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw "Could not start ProtoProject.exe: $tool" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) {
        try { $process.Kill() } catch {}
        [void]$process.WaitForExit(10000)
        throw 'ProtoProject.exe exceeded the 120 second project creation bound.'
    }
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($stdout) { Write-Output $stdout.TrimEnd() }
    if ($process.ExitCode -ne 0) {
        if ($stderr) { Write-Error $stderr.TrimEnd() }
        throw "ProtoProject.exe failed with exit code $($process.ExitCode)."
    }
} finally {
    $process.Dispose()
}

if (-not (Test-Path -LiteralPath (Join-Path $destinationPath 'project.proto.json') -PathType Leaf)) {
    throw "ProtoProject.exe reported success but did not create project.proto.json: $destinationPath"
}
Write-Output "Created Proto project: $destinationPath"
