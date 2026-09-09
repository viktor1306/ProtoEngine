[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BaselineExecutable,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$ValidationDirectory,
    [string[]]$Scenarios=@('small','instances-10000-visible','instances-10000-culled','instances-10000-moving',
        'moving-lights-32','moving-casters','pool-overflow','pool-resident','lights-128'),
    [int]$CaptureFrame=60
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $root) { throw 'Use a new capture output directory' }
New-Item -ItemType Directory -Path $root | Out-Null
function Quote-Argument([string]$Value) {
    # Paths/arguments here never end in a backslash; reject embedded quotes.
    if ($Value.Contains('"') -or $Value.EndsWith('\')) { throw 'Unsupported capture argument' }
    return '"'+$Value+'"'
}
$results=[Collections.Generic.List[object]]::new()
foreach ($scenario in $Scenarios) {
    if ($scenario -notmatch '^[a-z0-9-]+$') { throw 'Invalid scenario' }
    $reports=@{}
    foreach ($phase in @('baseline','candidate')) {
        $binary=[IO.Path]::GetFullPath($(if ($phase -eq 'baseline') { $BaselineExecutable } else { $Executable }))
        $folder=Join-Path $root $phase
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $stem=Join-Path $folder $scenario
        $arguments=@('--scenario',$scenario,'--warmup','0','--seconds','60','--frames','4','--output',($stem+'.json'),
            '--capture',($stem+'.png'),'--capture-frame',([string]$CaptureFrame))
        if ($phase -eq 'candidate') { $arguments+=@('--validation-dir',[IO.Path]::GetFullPath($ValidationDirectory)) }
        $start=[Diagnostics.ProcessStartInfo]::new()
        $start.FileName=$binary
        $start.Arguments=(($arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
        $start.WorkingDirectory=Split-Path -Parent $binary
        $start.UseShellExecute=$false; $start.CreateNoWindow=$true
        $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
        $start.EnvironmentVariables['PATH']=Join-Path $env:SystemRoot 'System32'
        $process=[Diagnostics.Process]::new(); $process.StartInfo=$start
        try {
            if (-not $process.Start()) { throw 'Capture process could not start' }
            $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(90000)) {
                try { $process.Kill($true) } catch { $process.Kill() }
                throw "Capture timed out: $phase/$scenario"
            }
            $process.WaitForExit()
            [IO.File]::WriteAllText(($stem+'.stdout.log'),$stdout.GetAwaiter().GetResult())
            [IO.File]::WriteAllText(($stem+'.stderr.log'),$stderr.GetAwaiter().GetResult())
            if ($process.ExitCode) { throw "Capture failed: $phase/$scenario" }
        } finally { $process.Dispose() }
        $report=Get-Content -LiteralPath ($stem+'.json') -Raw | ConvertFrom-Json
        $reports[$phase]=[pscustomobject]@{report=$report;executable_sha256=(Get-FileHash -LiteralPath $binary).Hash;
            png_sha256=(Get-FileHash -LiteralPath ($stem+'-viewport.png')).Hash;arguments=$arguments}
    }
    $before=$reports.baseline; $after=$reports.candidate
    if ($before.report.contentHash -ne $after.report.contentHash -or $before.report.shaderHash -ne $after.report.shaderHash) {
        throw "Capture content/shader mismatch: $scenario"
    }
    $same=$before.png_sha256 -eq $after.png_sha256
    $results.Add([pscustomobject]@{scenario=$scenario;capture_frame=$CaptureFrame;
        encoded_png_identical=$same;baseline=$before;candidate=$after})
    [IO.File]::WriteAllText((Join-Path $root 'comparison.json'),(@($results) | ConvertTo-Json -Depth 10))
    Write-Output ("M7 capture $scenario exact PNG match: $same")
    if (-not $same) { throw "Capture differs: $scenario; inspect images before accepting optimization" }
}
