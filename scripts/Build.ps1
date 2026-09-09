[CmdletBinding()]
param(
    [ValidateSet('debug','release','m2-debug','m2-release','m3-debug','m3-release','m4-debug','m4-release','m5-debug','m5-release','m6-debug','m6-release','m7-debug','m7-release','m8-debug','m8-release')][string]$Configuration = 'm8-debug',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$m0Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
# Fallback is process-local for an older terminal that has not seen user PATH.
if (!(Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    $m0Toolchain = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/w64devkit/bin'
    if (!(Test-Path -LiteralPath (Join-Path $m0Toolchain 'cmake.exe'))) { throw 'CMake, Ninja and a C++20 compiler must be on PATH.' }
    $env:PATH = $m0Toolchain + ';' + $env:PATH
}
Push-Location -LiteralPath $m0Root
try {
    if ($Configuration.EndsWith('debug')) { & (Join-Path $PSScriptRoot 'Setup-Validation.ps1'); if ($LASTEXITCODE) { throw 'Validation setup failed.' } }
    & cmake --preset $Configuration
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    & cmake --build --preset $Configuration
    if ($LASTEXITCODE) { throw 'Build failed.' }
    if ($Test) {
        & ctest --preset $Configuration
        if ($LASTEXITCODE) { throw 'Acceptance tests failed. See build/<configuration>/test-results.' }
    }
    Write-Host ('Editor: ' + (Join-Path $m0Root ('build/' + $Configuration + '/bin/ProtoEditor.exe')))
} finally { Pop-Location }
