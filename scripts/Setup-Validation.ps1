# Local files only. No elevation, registry edits, PATH edits, or Python install.
[CmdletBinding()]
param([string]$SdkArchive)
$ErrorActionPreference = 'Stop'
$m0Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$m0Lock = Get-Content -LiteralPath (Join-Path $m0Root 'dependencies.lock.json') -Raw | ConvertFrom-Json
$m0Tool = $m0Lock.tools.validation
$m0Destination = Join-Path $m0Root ('.cache/tools/validation-' + $m0Tool.version)

function Test-M0Hash([string]$File, [string]$Hash) {
    return (Test-Path -LiteralPath $File -PathType Leaf) -and ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -eq $Hash)
}
function Test-M0Layer {
    foreach ($m0Entry in $m0Tool.files.PSObject.Properties) {
        if (!(Test-M0Hash (Join-Path $m0Destination $m0Entry.Name) $m0Entry.Value)) { return $false }
    }
    return $true
}

if (Test-M0Layer) {
    Write-Host 'Pinned local Vulkan validation layer verified (1.4.357.0).'
    exit 0
}

# Qt's copy_only mode rejects 8.3 paths containing ~. The project root is a
# normal absolute path; all temporary extraction stays under its local cache.
$m0Stage = Join-Path $m0Root ('.cache/validation-extract-' + [guid]::NewGuid().ToString('N'))
if ($m0Stage.Contains('~')) { throw 'Use the full Windows project path, not an 8.3 short path.' }
New-Item -ItemType Directory -Force -Path $m0Stage | Out-Null
try {
    if ($SdkArchive) { $m0Archive = (Resolve-Path -LiteralPath $SdkArchive).Path }
    else {
        $m0Archive = Join-Path $m0Stage 'lunarg-package.exe'
        Write-Host 'Downloading the signed LunarG package (288 MB) to extract the 22 MB validation layer. The temporary package is removed afterwards.'
        Invoke-WebRequest -Uri $m0Tool.url -OutFile $m0Archive
    }
    if (!(Test-M0Hash $m0Archive $m0Tool.sha256)) { throw 'LunarG package SHA-256 mismatch.' }
    $m0Signature = Get-AuthenticodeSignature -LiteralPath $m0Archive
    if ($m0Signature.Status -ne 'Valid' -or $m0Signature.SignerCertificate.Subject -notmatch 'LunarG') {
        throw 'LunarG package signature could not be verified.'
    }
    $m0Extract = Join-Path $m0Stage 'files'
    $m0Arguments = @('--root', ('"' + $m0Extract + '"'), '--accept-licenses', '--default-answer', '--confirm-command', 'install', 'copy_only=1')
    $m0Process = Start-Process -FilePath $m0Archive -ArgumentList $m0Arguments -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $m0Stage 'extract.log') -RedirectStandardError (Join-Path $m0Stage 'extract.err')
    if ($m0Process.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $m0Stage 'extract.log') -Tail 20 | Write-Host
        throw ('LunarG copy-only extraction failed with code ' + $m0Process.ExitCode)
    }
    # Validate every retained binary before placing it in the project's cache.
    foreach ($m0Entry in $m0Tool.files.PSObject.Properties) {
        $m0File = Join-Path $m0Extract ('Bin/' + $m0Entry.Name)
        if (!(Test-M0Hash $m0File $m0Entry.Value)) { throw ('Extracted file hash mismatch: ' + $m0Entry.Name) }
    }
    New-Item -ItemType Directory -Force -Path $m0Destination | Out-Null
    foreach ($m0Entry in $m0Tool.files.PSObject.Properties) {
        Copy-Item -LiteralPath (Join-Path $m0Extract ('Bin/' + $m0Entry.Name)) -Destination (Join-Path $m0Destination $m0Entry.Name)
    }
    Copy-Item -LiteralPath (Join-Path $m0Extract 'Licenses/LICENSE.txt') -Destination (Join-Path $m0Destination 'LunarG-NOTICE.txt')
    if (!(Test-M0Layer)) { throw 'Final validation-layer verification failed.' }
    Write-Host ('Ready: ' + $m0Destination)
} finally {
    # Delete only this invocation's checked, task-owned temporary directory.
    $m0ResolvedStage = [IO.Path]::GetFullPath($m0Stage)
    $m0ExpectedParent = [IO.Path]::GetFullPath((Join-Path $m0Root '.cache')) + [IO.Path]::DirectorySeparatorChar
    if (!$m0ResolvedStage.StartsWith($m0ExpectedParent, [StringComparison]::OrdinalIgnoreCase) -or
        !(Split-Path $m0ResolvedStage -Leaf).StartsWith('validation-extract-')) { throw 'Refusing cleanup outside the task staging directory.' }
    if (Test-Path -LiteralPath $m0ResolvedStage) { Remove-Item -LiteralPath $m0ResolvedStage -Recurse -Force }
}
