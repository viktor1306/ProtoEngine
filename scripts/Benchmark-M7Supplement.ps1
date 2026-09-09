[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][Alias('BinRoot','BinaryDirectory')][string]$BinDirectory,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [ValidateRange(1,10)][int]$Repeats=3,
    [ValidateRange(0,30)][double]$Warmup=2,
    [ValidateRange(0.1,60)][double]$Seconds=5,
    [ValidateRange(1,100000)][int]$AssetCycles=12,
    [Alias('SkipEditorBenchmarks')][switch]$SkipEditor,
    [Alias('SkipAssets','SkipAsset')][switch]$SkipAssetBenchmark,
    [switch]$Resume
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

$bin=[IO.Path]::GetFullPath($BinDirectory)
$output=[IO.Path]::GetFullPath($OutputRoot)
$editor=Join-Path $bin 'ProtoEditor.exe'
$assetBenchmark=Join-Path $bin 'ProtoAssetBenchmark.exe'
$indexPath=Join-Path $output 'index.json'
$culture=[Globalization.CultureInfo]::InvariantCulture
$warmupText=$Warmup.ToString($culture)
$secondsText=$Seconds.ToString($culture)

if (-not (Test-Path -LiteralPath $bin -PathType Container)) { throw "Missing bin directory: $bin" }
if (-not $SkipEditor -and -not (Test-Path -LiteralPath $editor -PathType Leaf)) { throw "Missing ProtoEditor.exe: $editor" }
if (-not $SkipAssetBenchmark -and -not (Test-Path -LiteralPath $assetBenchmark -PathType Leaf)) {
    throw "Missing ProtoAssetBenchmark.exe: $assetBenchmark"
}
if ($SkipEditor -and $SkipAssetBenchmark) { throw 'Both benchmark groups are disabled' }

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Save-Json($Value,[string]$Path) {
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 24),[Text.UTF8Encoding]::new($false))
}
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
function Format-Command([string]$Path,[string[]]$Arguments) {
    return ((@($Path)+$Arguments) | ForEach-Object { Quote-Argument ([string]$_) }) -join ' '
}
function Host-Snapshot {
    $processes=@(Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            [pscustomobject]@{pid=$_.Id; name=$_.ProcessName; cpu_seconds=$_.TotalProcessorTime.TotalSeconds}
        } catch {}
    })
    [ordered]@{
        at=(Get-Date -Format o)
        logical_processors=[Environment]::ProcessorCount
        power_scheme=(powercfg /getactivescheme | Out-String).Trim()
        processes=$processes
    }
}
function Assert-NoProtoBenchmarkProcess {
    $other=@(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match '^Proto(Editor|Benchmark|AssetBenchmark)$' })
    if ($other.Count) {
        throw ('Another Proto benchmark process is active: ' +
            (($other | Select-Object -ExpandProperty ProcessName) -join ', '))
    }
}
function Test-ArtifactExists([string]$Path) {
    return Test-Path -LiteralPath $Path -PathType Leaf
}

$editorHash=$null
$assetHash=$null
if (-not $SkipEditor) { $editorHash=Get-Hash $editor }
if (-not $SkipAssetBenchmark) { $assetHash=Get-Hash $assetBenchmark }

$savedIndex=$null
if (Test-Path -LiteralPath $output) {
    if (-not (Test-Path -LiteralPath $output -PathType Container)) { throw "Output is not a directory: $output" }
    if (Test-Path -LiteralPath $indexPath -PathType Leaf) {
        if (-not $Resume) { throw 'Benchmark output already has an index; use -Resume or a new output folder' }
        $savedIndex=Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    } elseif (@(Get-ChildItem -LiteralPath $output -Force).Count -gt 0) {
        throw 'Output directory is non-empty without an index; refusing to overwrite it'
    }
} else {
    New-Item -ItemType Directory -Path $output -Force | Out-Null
}

if ($savedIndex) {
    if ($savedIndex.format -ne 'proto.benchmark.supplement' -or $savedIndex.version -ne 1) {
        throw 'Resume index has an incompatible format'
    }
    if ([IO.Path]::GetFullPath([string]$savedIndex.bin_directory) -ne $bin -or
        $savedIndex.repeats -ne $Repeats -or $savedIndex.warmup_seconds -ne $Warmup -or
        $savedIndex.sample_seconds -ne $Seconds -or $savedIndex.asset_cycles -ne $AssetCycles -or
        [bool]$savedIndex.skip_editor -ne [bool]$SkipEditor -or
        [bool]$savedIndex.skip_asset_benchmark -ne [bool]$SkipAssetBenchmark) {
        throw 'Resume parameters do not match the saved supplementary run'
    }
    if (-not $SkipEditor -and $savedIndex.binaries.editor.sha256 -ne $editorHash) {
        throw 'Resume editor binary hash differs from the saved run'
    }
    if (-not $SkipAssetBenchmark -and $savedIndex.binaries.asset_benchmark.sha256 -ne $assetHash) {
        throw 'Resume asset benchmark binary hash differs from the saved run'
    }
}

function New-RunPlan([string]$Group,[int]$Repeat) {
    if ($Group -eq 'editor-idle' -or $Group -eq 'editor-interactive') {
        $interactive=$Group -eq 'editor-interactive'
        $directory=Join-Path $output ($Group)
        $stem=Join-Path $directory ('r'+$Repeat)
        $arguments=[Collections.Generic.List[string]]::new()
        $arguments.Add('--benchmark'); $arguments.Add($stem)
        $arguments.Add('--report'); $arguments.Add($stem+'.main.json')
        $arguments.Add('--benchmark-warmup'); $arguments.Add($warmupText)
        $arguments.Add('--benchmark-seconds'); $arguments.Add($secondsText)
        if ($interactive) { $arguments.Add('--benchmark-interactive') }
        $reports=@(
            ($stem + '.json')
            ($stem + '.csv')
            ($stem + '.manifest.json')
            ($stem + '.observations.json')
            ($stem + '.main.json')
        )
        return [pscustomobject]@{
            id=($Group+'-r'+$Repeat); group=$Group; repeat=$Repeat; executable=$editor;
            executable_sha256=$editorHash; arguments=@($arguments); stem=$stem; reports=$reports;
            timeout_ms=[int][Math]::Max(120000,($Warmup+$Seconds)*1000+90000)
        }
    }
    if ($Group -eq 'asset') {
        $directory=Join-Path $output 'asset'
        $stem=Join-Path $directory ('r'+$Repeat)
        $arguments=@('--output',$stem,'--cycles',([string]$AssetCycles))
        $reports=@(
            ($stem + '.json')
            ($stem + '.csv')
        )
        return [pscustomobject]@{
            id=('asset-r'+$Repeat); group='asset'; repeat=$Repeat; executable=$assetBenchmark;
            executable_sha256=$assetHash; arguments=$arguments; stem=$stem; reports=$reports;
            timeout_ms=[int][Math]::Max(120000,$AssetCycles*15000+60000)
        }
    }
    throw "Unknown supplementary benchmark group: $Group"
}

function Assert-CommittedRun($Run) {
    if (-not (Test-Path -LiteralPath $Run.host -PathType Leaf)) { throw "Committed host metadata missing: $($Run.host)" }
    $hostData=Get-Content -LiteralPath $Run.host -Raw | ConvertFrom-Json
    if ($hostData.exit_code -ne 0 -or $hostData.executable_sha256 -ne $Run.executable_sha256) {
        throw "Committed run failed or binary hash changed: $($Run.id)"
    }
    foreach ($report in @($Run.reports)) {
        if (-not (Test-ArtifactExists ([string]$report))) { throw "Committed report missing: $report" }
    }
}

function Run-BoundedProcess($Plan) {
    foreach ($report in @($Plan.reports)) {
        if (Test-ArtifactExists ([string]$report)) { throw "Refusing to overwrite existing report: $report" }
    }
    $hostPath=$Plan.stem+'.host.json'
    $stdoutPath=$Plan.stem+'.stdout.log'
    $stderrPath=$Plan.stem+'.stderr.log'
    foreach ($path in @($hostPath,$stdoutPath,$stderrPath)) {
        if (Test-ArtifactExists $path) { throw "Refusing to overwrite existing run artifact: $path" }
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Plan.stem) -Force | Out-Null
    if ((Get-Hash $Plan.executable) -ne $Plan.executable_sha256) {
        throw "Benchmark binary changed before launch: $($Plan.executable)"
    }
    Assert-NoProtoBenchmarkProcess
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Plan.executable
    $start.Arguments=(($Plan.arguments | ForEach-Object { Quote-Argument ([string]$_) }) -join ' ')
    $start.WorkingDirectory=Split-Path -Parent $Plan.executable
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    $start.EnvironmentVariables['PATH']=(Join-Path $env:SystemRoot 'System32')
    $before=Host-Snapshot
    $commandLine=Format-Command $Plan.executable $Plan.arguments
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=$start
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $stdoutText=''; $stderrText=''; $stdoutTask=$null; $stderrTask=$null
    $exitCode=-1; $timedOut=$false; $errorText=$null
    $cpuBefore=0.0; $cpuAfter=0.0; $started=$false
    try {
        if (-not $process.Start()) { throw 'Could not start supplementary benchmark' }
        $started=$true
        try { $cpuBefore=$process.TotalProcessorTime.TotalSeconds } catch {}
        $stdoutTask=$process.StandardOutput.ReadToEndAsync()
        $stderrTask=$process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($Plan.timeout_ms)) {
            $timedOut=$true
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
            [void]$process.WaitForExit(10000)
            throw "Supplementary benchmark timed out: $($Plan.id)"
        }
        $process.WaitForExit()
        try { $exitCode=$process.ExitCode } catch { $exitCode=-1 }
        try { $cpuAfter=$process.TotalProcessorTime.TotalSeconds } catch {}
        $stdoutText=$stdoutTask.GetAwaiter().GetResult()
        $stderrText=$stderrTask.GetAwaiter().GetResult()
    } catch {
        $errorText=$_.Exception.Message
        if ($started -and -not $process.HasExited) {
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
            try { [void]$process.WaitForExit(10000) } catch {}
        }
        try { if ($stdoutTask) { $stdoutText=$stdoutTask.GetAwaiter().GetResult() } } catch {}
        try { if ($stderrTask) { $stderrText=$stderrTask.GetAwaiter().GetResult() } } catch {}
        try { if ($process.HasExited) { $exitCode=$process.ExitCode } } catch {}
    } finally {
        try { [IO.File]::WriteAllText($stdoutPath,$stdoutText,[Text.UTF8Encoding]::new($false)) } catch {}
        try { [IO.File]::WriteAllText($stderrPath,$stderrText,[Text.UTF8Encoding]::new($false)) } catch {}
        $after=Host-Snapshot
        $metadata=[ordered]@{
            format='proto.benchmark.process'; version=1; id=$Plan.id; group=$Plan.group; repeat=$Plan.repeat
            executable=$Plan.executable; executable_sha256=$Plan.executable_sha256; arguments=@($Plan.arguments)
            command_line=$commandLine; duration_ms=$timer.Elapsed.TotalMilliseconds; exit_code=$exitCode
            timed_out=$timedOut; error=$errorText; process_cpu_seconds_before=$cpuBefore
            process_cpu_seconds_after=$cpuAfter
            process_cpu_seconds_delta=[Math]::Max([double]0.0,[double]($cpuAfter-$cpuBefore))
            before=$before; after=$after; stdout=$stdoutPath; stderr=$stderrPath; reports=@($Plan.reports)
        }
        Save-Json $metadata $hostPath
        $process.Dispose()
    }
    if ($errorText) { throw $errorText }
    if ($exitCode -ne 0) { throw "Supplementary benchmark failed: $($Plan.id) (exit $exitCode)" }
    foreach ($report in @($Plan.reports)) {
        if (-not (Test-ArtifactExists ([string]$report))) { throw "Benchmark report missing: $report" }
    }
    return [pscustomobject]@{id=$Plan.id; group=$Plan.group; repeat=$Plan.repeat; executable=$Plan.executable;
        executable_sha256=$Plan.executable_sha256; arguments=@($Plan.arguments); reports=@($Plan.reports);
        host=$hostPath; stdout=$stdoutPath; stderr=$stderrPath}
}

$runs=[Collections.Generic.List[object]]::new()
if ($savedIndex) {
    foreach ($savedRun in @($savedIndex.runs)) {
        Assert-CommittedRun $savedRun
        $runs.Add($savedRun)
    }
}
$binaries=[ordered]@{
    editor=[ordered]@{path=$editor; sha256=$editorHash}
    asset_benchmark=[ordered]@{path=$assetBenchmark; sha256=$assetHash}
}
$index=[ordered]@{
    format='proto.benchmark.supplement'; version=1; started=if($savedIndex){$savedIndex.started}else{Get-Date -Format o}
    bin_directory=$bin; repeats=$Repeats; warmup_seconds=$Warmup; sample_seconds=$Seconds; asset_cycles=$AssetCycles
    skip_editor=[bool]$SkipEditor; skip_asset_benchmark=[bool]$SkipAssetBenchmark; binaries=$binaries
    order='editor idle repeats, editor interactive repeats, asset repeats; all serial'; runs=$runs; complete=$false
}
Save-Json $index $indexPath
$environmentPath=Join-Path $PSScriptRoot '../.cache/m7-environment-start.json'
if ((-not (Test-Path -LiteralPath (Join-Path $output 'environment.json'))) -and
    (Test-Path -LiteralPath $environmentPath -PathType Leaf)) {
    Copy-Item -LiteralPath $environmentPath -Destination (Join-Path $output 'environment.json')
}

$plans=[Collections.Generic.List[object]]::new()
if (-not $SkipEditor) {
    for ($repeat=1;$repeat -le $Repeats;$repeat++) { $plans.Add((New-RunPlan 'editor-idle' $repeat)) }
    for ($repeat=1;$repeat -le $Repeats;$repeat++) { $plans.Add((New-RunPlan 'editor-interactive' $repeat)) }
}
if (-not $SkipAssetBenchmark) {
    for ($repeat=1;$repeat -le $Repeats;$repeat++) { $plans.Add((New-RunPlan 'asset' $repeat)) }
}

foreach ($plan in $plans) {
    $existing=@($runs | Where-Object { $_.id -eq $plan.id })
    if ($existing.Count) {
        if ($existing.Count -ne 1) { throw "Duplicate committed run: $($plan.id)" }
        if ($existing[0].executable_sha256 -ne $plan.executable_sha256 -or
            (@($existing[0].arguments) -join "`n") -ne (@($plan.arguments) -join "`n")) {
            throw "Committed command or binary differs: $($plan.id)"
        }
        continue
    }
    Write-Output ("M7 supplement $($plan.group) repeat $($plan.repeat)/$Repeats")
    $run=Run-BoundedProcess $plan
    $runs.Add($run)
    Save-Json $index $indexPath
}

$index.complete=$true
$index.finished=Get-Date -Format o
Save-Json $index $indexPath
Write-Output ('M7 supplementary benchmark complete: '+$indexPath)
