[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string]$BaselineExecutable,
    [string[]]$Scenarios=@('empty','small','instances-1000','instances-10000-visible','instances-10000-culled',
        'instances-10000-moving','lights-1','lights-8','lights-32','lights-128','moving-lights-32',
        'moving-casters','pool-overflow','pool-resident'),
    [ValidateRange(1,10)][int]$Repeats=3,
    [ValidateRange(0,30)][double]$Warmup=2,
    [ValidateRange(0.1,60)][double]$Seconds=5,
    [switch]$VSync,
    [switch]$Resume
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputRoot)
$candidate=[IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "Missing benchmark $candidate" }
$baseline=$null
if ($BaselineExecutable) {
    $baseline=[IO.Path]::GetFullPath($BaselineExecutable)
    if (-not (Test-Path -LiteralPath $baseline -PathType Leaf)) { throw "Missing baseline $baseline" }
}
$savedIndex=$null
if (Test-Path -LiteralPath (Join-Path $output 'index.json')) {
    if (-not $Resume) { throw 'Benchmark output already has an index; use -Resume or a new output folder' }
    $savedIndex=Get-Content -LiteralPath (Join-Path $output 'index.json') -Raw | ConvertFrom-Json
    if ($savedIndex.repeats -ne $Repeats -or $savedIndex.requested_warmup_seconds -ne $Warmup -or
        $savedIndex.requested_sample_seconds -ne $Seconds -or [bool]$savedIndex.vsync -ne [bool]$VSync -or
        (@($savedIndex.scenarios) -join ',') -ne ($Scenarios -join ',')) { throw 'Resume parameters do not match the saved run' }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

function Quote-Argument([string]$Value) {
    if ($Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $result=[Text.StringBuilder]::new()
    [void]$result.Append('"')
    $slashes=0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            for ($i=0;$i -lt 2*$slashes+1;$i++) { [void]$result.Append('\') }
            [void]$result.Append('"')
        } else {
            for ($i=0;$i -lt $slashes;$i++) { [void]$result.Append('\') }
            [void]$result.Append($character)
        }
        $slashes=0
    }
    for ($i=0;$i -lt 2*$slashes;$i++) { [void]$result.Append('\') }
    [void]$result.Append('"')
    return $result.ToString()
}
function Save-Json($Value,[string]$Path) {
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 18),[Text.UTF8Encoding]::new($false))
}
function Host-Snapshot {
    $processes=@(Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
        try { [pscustomobject]@{pid=$_.Id; name=$_.ProcessName; cpu_seconds=$_.TotalProcessorTime.TotalSeconds} } catch {}
    })
    [ordered]@{at=(Get-Date -Format o); logical_processors=[Environment]::ProcessorCount;
        power_scheme=(powercfg /getactivescheme | Out-String).Trim(); processes=$processes}
}
function Run-Benchmark([string]$Path,[string[]]$Arguments,[string]$Stem) {
    $other=@(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^Proto(Editor|Player|Benchmark|AssetBenchmark)$' })
    if ($other.Count) { throw ('Another Proto process is active: '+(($other | Select-Object -ExpandProperty ProcessName) -join ', ')) }
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Path
    $start.Arguments=(($Arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $start.WorkingDirectory=Split-Path -Parent $Path
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    $start.EnvironmentVariables['PATH']=(Join-Path $env:SystemRoot 'System32')
    $before=Host-Snapshot
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=$start
    $timer=[Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $process.Start()) { throw 'Could not start benchmark' }
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        $timeout=[int][Math]::Max(120000,($Warmup+$Seconds)*1000+90000)
        if (-not $process.WaitForExit($timeout)) {
            try { $process.Kill($true) } catch { $process.Kill() }
            throw ('Benchmark timed out: '+$Path)
        }
        $process.WaitForExit()
        [IO.File]::WriteAllText($Stem+'.stdout.log',$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($Stem+'.stderr.log',$stderr.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
        $after=Host-Snapshot
        $metadata=[ordered]@{executable=$Path; executable_sha256=(Get-FileHash -LiteralPath $Path).Hash;
            arguments=$Arguments; duration_ms=$timer.Elapsed.TotalMilliseconds; exit_code=$process.ExitCode;
            process_cpu_seconds=$process.TotalProcessorTime.TotalSeconds; before=$before; after=$after}
        Save-Json $metadata ($Stem+'.host.json')
        if ($process.ExitCode -ne 0) { throw ('Benchmark failed: '+$Stem+'; see stderr.log') }
        if (-not (Test-Path -LiteralPath ($Stem+'.json') -PathType Leaf)) { throw ('Benchmark report missing: '+$Stem) }
    } finally { $process.Dispose() }
}

$runs=[Collections.Generic.List[object]]::new()
if ($savedIndex) {
    foreach ($run in @($savedIndex.runs)) {
        if (-not (Test-Path -LiteralPath $run.report) -or -not (Test-Path -LiteralPath $run.host)) { throw 'Committed run artifacts missing' }
        $hostData=Get-Content -LiteralPath $run.host -Raw | ConvertFrom-Json
        $expectedBinary=if ($run.phase -eq 'baseline') { $baseline } else { $candidate }
        if (-not $expectedBinary -or $hostData.exit_code -ne 0 -or
            $hostData.executable_sha256 -ne (Get-FileHash -LiteralPath $expectedBinary).Hash) { throw 'Resume binary differs from committed measurements' }
        $runs.Add($run)
    }
}
$index=[ordered]@{format='proto.benchmark.runs'; version=1; started=(Get-Date -Format o); repeats=$Repeats;
    requested_warmup_seconds=$Warmup; requested_sample_seconds=$Seconds; vsync=[bool]$VSync;
    scenarios=$Scenarios; order='paired alternating order by repetition when baseline supplied'; runs=$runs; complete=$false}
$environmentPath=Join-Path $PSScriptRoot '../.cache/m7-environment-start.json'
if (Test-Path -LiteralPath $environmentPath) { Copy-Item -LiteralPath $environmentPath -Destination (Join-Path $output 'environment.json') -Force }
$culture=[Globalization.CultureInfo]::InvariantCulture
foreach ($scenario in $Scenarios) {
    if ($scenario -notmatch '^[a-z0-9-]+$') { throw 'Invalid scenario name' }
    for ($repeat=1;$repeat -le $Repeats;$repeat++) {
        $phases=if ($baseline) { if ($repeat%2) { @('baseline','candidate') } else { @('candidate','baseline') } } else { @('candidate') }
        foreach ($phase in $phases) {
            if (@($runs | Where-Object { $_.scenario -eq $scenario -and $_.phase -eq $phase -and $_.repeat -eq $repeat }).Count) { continue }
            $directory=Join-Path $output $phase
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
            $stem=Join-Path $directory ($scenario+'-r'+$repeat)
            $arguments=@('--scenario',$scenario,'--warmup',$Warmup.ToString($culture),'--seconds',$Seconds.ToString($culture),'--output',($stem+'.json'))
            if ($VSync) { $arguments+='--vsync' }
            $binary=if ($phase -eq 'baseline') { $baseline } else { $candidate }
            Write-Output ("M7 $phase $scenario repeat $repeat/$Repeats")
            Run-Benchmark $binary $arguments $stem
            $runs.Add([pscustomobject]@{scenario=$scenario; phase=$phase; repeat=$repeat; report=($stem+'.json'); host=($stem+'.host.json')})
            Save-Json $index (Join-Path $output 'index.json')
        }
    }
}
$index.complete=$true
$index.finished=(Get-Date -Format o)
Save-Json $index (Join-Path $output 'index.json')
Write-Output ('M7 matrix complete: '+(Join-Path $output 'index.json'))
