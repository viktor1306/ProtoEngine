[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$EditorBaselineRoot,
    [string]$EditorCandidateRoot,
    [Parameter(Mandatory=$true)][string]$AssetBaselineRoot,
    [string]$AssetCandidateRoot,
    [Parameter(Mandatory=$true)][string]$OutputStem
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

function Read-Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing artifact: $Path" }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}
function Get-Prop($Object,[string]$Name,$Default=$null) {
    if ($null -eq $Object) { return $Default }
    $property=$Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}
function To-Double($Value) {
    if ($null -eq $Value) { return $null }
    $number=0.0
    if (-not [double]::TryParse(([string]$Value),[Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,[ref]$number)) { return $null }
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $null }
    return [double]$number
}
function Values([object[]]$Items,[scriptblock]$Selector) {
    $result=[Collections.Generic.List[double]]::new()
    foreach ($item in @($Items)) {
        $value=& $Selector $item
        $number=To-Double $value
        if ($null -ne $number) { $result.Add($number) }
    }
    return @($result)
}
function Percentile([double[]]$InputValues,[double]$P) {
    $values=@($InputValues | Sort-Object)
    if ($values.Count -eq 0) { return $null }
    $position=$P * ($values.Count - 1)
    $lower=[math]::Floor($position)
    $upper=[math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$values[$lower] }
    $fraction=$position-$lower
    return [double]$values[$lower] + ([double]$values[$upper]-[double]$values[$lower])*$fraction
}
function Metric-Stats([object[]]$Items,[scriptblock]$Selector) {
    $values=@(Values $Items $Selector)
    return [ordered]@{
        count=$values.Count
        median=Percentile $values .50
        p95=Percentile $values .95
        p99=Percentile $values .99
    }
}
function Max-Nullable([object[]]$Items,[scriptblock]$Selector) {
    $values=@(Values $Items $Selector)
    if ($values.Count -eq 0) { return $null }
    return ($values | Measure-Object -Maximum).Maximum
}
function Hash-File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Save-Json($Value,[string]$Path) {
    $parent=Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 24),[Text.UTF8Encoding]::new($false))
}
function Csv-Cell($Value) {
    if ($null -eq $Value) { return '' }
    if ($Value -is [double] -or $Value -is [single] -or $Value -is [decimal]) {
        $text=([IFormattable]$Value).ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    } elseif ($Value -is [bool]) {
        $text=if($Value){'true'}else{'false'}
    } else { $text=[string]$Value }
    if ($text.Contains('"')) { $text=$text.Replace('"','""') }
    if ($text.Contains(',') -or $text.Contains('"') -or $text.Contains("`r") -or $text.Contains("`n")) {
        return '"'+$text+'"'
    }
    return $text
}
function Save-Csv([object[]]$Rows,[string]$Path) {
    $items=@($Rows)
    $lines=[Collections.Generic.List[string]]::new()
    if ($items.Count -eq 0) { [IO.File]::WriteAllText($Path,'',[Text.UTF8Encoding]::new($false)); return }
    $columns=@($items[0].PSObject.Properties.Name)
    $lines.Add((($columns | ForEach-Object { Csv-Cell $_ }) -join ','))
    foreach ($item in $items) {
        $lines.Add((($columns | ForEach-Object { Csv-Cell (Get-Prop $item $_) }) -join ','))
    }
    [IO.File]::WriteAllLines($Path,$lines,[Text.UTF8Encoding]::new($false))
}
function Test-True($Value) { return $null -ne $Value -and [bool]$Value }
function Resolve-Root([string]$Root) { return [IO.Path]::GetFullPath($Root) }

function Validate-InputRoot([string]$Root,[string]$Kind) {
    $resolved=Resolve-Root $Root
    $index=Read-Json (Join-Path $resolved 'index.json')
    if ($index.format -ne 'proto.benchmark.supplement' -or $index.version -ne 1 -or -not (Test-True $index.complete)) {
        throw "Incomplete or incompatible $Kind supplement index: $resolved"
    }
    $requiredGroups=if ($Kind -eq 'editor') { @('editor-idle','editor-interactive') } else { @('asset') }
    foreach ($group in $requiredGroups) {
        $groupRuns=@($index.runs | Where-Object { $_.group -eq $group })
        if ($groupRuns.Count -lt 3) { throw "$Kind index has fewer than three $group runs: $resolved" }
        foreach ($repeat in 1..3) {
            if (@($groupRuns | Where-Object { $_.repeat -eq $repeat }).Count -ne 1) {
                throw "$Kind index does not have exactly one $group repeat ${repeat}: ${resolved}"
            }
        }
    }
    # A mixed supplement can retain older Editor results while its unchanged
    # asset executable is still current. Validate exactly the requested group.
    foreach ($run in @($index.runs | Where-Object { $_.group -in $requiredGroups })) {
        $hostData=Read-Json $run.host
        if ($hostData.exit_code -ne 0 -or (Test-True $hostData.timed_out)) { throw "Failed run in ${resolved}: $($run.id)" }
        $binary=Get-Prop $run 'executable'
        $binaryHash=Get-Prop $run 'executable_sha256'
        if (-not $binary -or -not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing run binary: $binary" }
        if ($binaryHash -ne (Hash-File $binary) -or $hostData.executable_sha256 -ne $binaryHash) {
            throw "Binary hash mismatch for $($run.id)"
        }
        $indexHash=$null
        if ($run.group -like 'editor-*') { $indexHash=Get-Prop (Get-Prop $index 'binaries') 'editor' | Select-Object -ExpandProperty sha256 }
        if ($run.group -eq 'asset') { $indexHash=Get-Prop (Get-Prop $index 'binaries') 'asset_benchmark' | Select-Object -ExpandProperty sha256 }
        if ($indexHash -and $indexHash -ne $binaryHash) { throw "Index binary hash mismatch for $($run.id)" }
        foreach ($reportPath in @($run.reports)) {
            if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "Missing report for $($run.id): $reportPath" }
        }
    }
    return [pscustomobject]@{label=$Kind; root=$resolved; index=$index}
}

function Find-ReportPath($Run,[string]$Kind) {
    foreach ($path in @($Run.reports)) {
        if ([IO.Path]::GetExtension([string]$path) -ne '.json') { continue }
        $candidate=Read-Json $path
        if ($Kind -eq 'editor' -and $candidate.format -eq 'proto.benchmark') { return [string]$path }
        if ($Kind -eq 'asset' -and $candidate.format -eq 'proto.asset-benchmark') { return [string]$path }
    }
    throw "Primary $Kind report not found for $($Run.id)"
}

function Get-EditorRun($Run) {
    $report=Read-Json (Find-ReportPath $Run 'editor')
    $observations=Read-Json (($Run.reports | Where-Object { ([string]$_).EndsWith('.observations.json') } | Select-Object -First 1))
    $samples=@($report.samples)
    $summary=Get-Prop $report 'summary'
    $wall=[ordered]@{median=Get-Prop (Get-Prop $summary 'wall_ms') 'median'; p95=Get-Prop (Get-Prop $summary 'wall_ms') 'p95'; p99=Get-Prop (Get-Prop $summary 'wall_ms') 'p99'}
    $gpu=[ordered]@{median=Get-Prop (Get-Prop $summary 'gpu_ms') 'median'; p95=Get-Prop (Get-Prop $summary 'gpu_ms') 'p95'; p99=Get-Prop (Get-Prop $summary 'gpu_ms') 'p99'}
    if ($null -eq $wall.median) { $wall=Metric-Stats $samples { param($s) Get-Prop $s 'wall_ms' } }
    if ($null -eq $gpu.median) { $gpu=Metric-Stats (Get-Prop $report 'gpuSamples') { param($s) Get-Prop $s 'milliseconds' } }
    $process=@($samples | Where-Object { Test-True (Get-Prop $_ 'process_valid') })
    $lastProcess=$process | Select-Object -Last 1
    $duration=To-Double (Get-Prop $report 'actualDurationMs')
    $cpuLast=To-Double (Get-Prop $lastProcess 'process_cpu_ms')
    $cpuRatio=if ($null -ne $cpuLast -and $duration -and $duration -gt 0) { $cpuLast/$duration } else { $null }
    $vma=[ordered]@{
        allocation_peak=Max-Nullable $samples { param($s) Get-Prop $s 'vma_peak_allocation_bytes' }
        allocation_observed_peak=Max-Nullable $samples { param($s) Get-Prop $s 'vma_allocation_bytes' }
    }
    $memory=[ordered]@{
        working_set_peak=Max-Nullable $process { param($s) Get-Prop $s 'working_set_bytes' }
        private_bytes_peak=Max-Nullable $process { param($s) Get-Prop $s 'private_bytes' }
    }
    $gpuStart=To-Double (Get-Prop $report 'gpuWarmupSerial'); $gpuEnd=To-Double (Get-Prop $report 'gpuEndSerial')
    $timedFrames=To-Double (Get-Prop $observations 'timedRenderedFrames')
    $gpuRows=@(Get-Prop $report 'gpuSamples')
    $gpuRange=Test-True (Get-Prop $observations 'gpuCoverageValid') -and
        $gpuStart -ne $null -and $gpuEnd -ne $null -and $gpuEnd -ge $gpuStart -and
        ($gpuEnd-$gpuStart) -eq $timedFrames -and @($gpuRows).Count -eq $timedFrames
    $dimensionsStable=Test-True (Get-Prop $observations 'dimensionsStable')
    if (-not $gpuRange -or -not $dimensionsStable) { throw "Invalid Editor measurement coverage or dimensions: $($Run.id)" }
    [pscustomobject]@{
        id=$Run.id; group=$Run.group; repeat=$Run.repeat; report=(Find-ReportPath $Run 'editor')
        executable_sha256=$Run.executable_sha256; source_hash=Get-Prop $report 'sourceHash'; build_id=Get-Prop $report 'buildId'; shader_hash=Get-Prop $report 'shaderHash'
        target_width=Get-Prop (Get-Prop $report 'target') 'width'; target_height=Get-Prop (Get-Prop $report 'target') 'height'
        wall=$wall; gpu=$gpu; actual_duration_ms=$duration; timed_frames=$timedFrames
        session_rendered_frames=Get-Prop $observations 'sessionRenderedFrames'; session_scene_frames=Get-Prop $observations 'sessionSceneFrames'
        warmup_frames=Get-Prop $observations 'warmupFrameCount'; timed_scene_frames=Get-Prop $observations 'timedSceneFrames'
        process_cpu_ms_last=$cpuLast; process_cpu_ratio_1core=$cpuRatio; process_cpu_ratio_16core=if($null -ne $cpuRatio){$cpuRatio/16}else{$null}
        process_cpu_percent_1core=if($null -ne $cpuRatio){$cpuRatio*100}else{$null}; process_cpu_percent_16core=if($null -ne $cpuRatio){$cpuRatio*100/16}else{$null}
        memory=$memory; vma=$vma; dimensions_stable=$dimensionsStable; gpu_complete=$gpuRange
        gpu_warmup_serial=$gpuStart; gpu_end_serial=$gpuEnd; gpu_sample_count=@($gpuRows).Count
    }
}

function Get-AssetRun($Run) {
    $report=Read-Json (Find-ReportPath $Run 'asset')
    $stageMap=@{}
    foreach ($stage in @($report.stages)) { $stageMap[[string]$stage.name]=$stage }
    $cycles=@($report.cycles_detail)
    $weak=Get-Prop $report 'weak_owner_checks'; $ops=Get-Prop $report 'file_operations'; $cleanup=Get-Prop $report 'cleanup'
    $metricNames=@('move_redo_ms','move_undo_ms','move_redo_again_ms','move_final_undo_ms','copy_redo_ms','copy_undo_ms','copy_redo_again_ms','copy_final_undo_ms')
    $opsSummary=[ordered]@{}
    foreach ($name in $metricNames) { $opsSummary[$name]=Metric-Stats $cycles { param($c) Get-Prop $c $name } }
    $cyclePrivate=Max-Nullable $cycles { param($c) Get-Prop (Get-Prop $c 'process') 'peak_private_bytes' }
    $cycleWorking=Max-Nullable $cycles { param($c) Get-Prop (Get-Prop $c 'process') 'peak_working_set_bytes' }
    $loadStage=$stageMap['material_document_asset_load_unload']
    $allRestored=(@($cycles | Where-Object { -not (Test-True (Get-Prop $_ 'restored')) }).Count -eq 0)
    $allWeak=Test-True (Get-Prop $weak 'model_expired') -and Test-True (Get-Prop $weak 'mesh_expired') -and Test-True (Get-Prop $weak 'texture_pixels_expired')
    $allHashes=Test-True (Get-Prop $ops 'hashes_restored'); $allIds=Test-True (Get-Prop $ops 'ids_restored')
    $allCleanup=Test-True (Get-Prop $cleanup 'attempted') -and Test-True (Get-Prop $cleanup 'succeeded')
    if ((Get-Prop $report 'cycles') -ne 12 -or $cycles.Count -ne 12 -or -not $allRestored -or -not $allWeak -or
        -not $allHashes -or -not $allIds -or -not $allCleanup) {
        throw "Invalid AssetBenchmark cycle/ownership/cleanup validation: $($Run.id)"
    }
    [pscustomobject]@{
        id=$Run.id; group=$Run.group; repeat=$Run.repeat; report=(Find-ReportPath $Run 'asset'); executable_sha256=$Run.executable_sha256
        source_hash=Get-Prop $report 'source_hash'; imported_id=Get-Prop $report 'imported_id'; cycles=Get-Prop $report 'cycles'
        initial_import_ms=Get-Prop $stageMap['initial_import'] 'duration_ms'
        cooked_cold_load_ms=Get-Prop $stageMap['cooked_cold_load'] 'duration_ms'
        cooked_warm_load_ms=Get-Prop $stageMap['cooked_warm_load'] 'duration_ms'
        cold_from_cache=Get-Prop $report 'cold_from_cache'; warm_from_cache=Get-Prop $report 'warm_from_cache'
        material_document_load_unload_ms=Get-Prop $loadStage 'duration_ms'
        material_private_peak=Get-Prop (Get-Prop $loadStage 'process') 'peak_private_bytes'; material_working_peak=Get-Prop (Get-Prop $loadStage 'process') 'peak_working_set_bytes'
        cycle_count=$cycles.Count; cycles_restored=$allRestored; weak_owners_expired=$allWeak
        hashes_restored=$allHashes; ids_restored=$allIds
        cleanup_all=$allCleanup
        cycle_private_peak=$cyclePrivate; cycle_working_peak=$cycleWorking; cycle_vma_peak=$null; operation_metrics=$opsSummary
    }
}

function Get-EditorGroup($RootData,[string]$Label) {
    $runs=[Collections.Generic.List[object]]::new()
    foreach ($group in @('editor-idle','editor-interactive')) {
        foreach ($run in @($RootData.index.runs | Where-Object { $_.group -eq $group -and $_.repeat -le 3 } | Sort-Object repeat)) {
            $value=Get-EditorRun $run
            [void]($value | Add-Member -NotePropertyName label -NotePropertyValue $Label -PassThru)
            [void]$runs.Add($value)
        }
    }
    return $runs.ToArray()
}
function Get-AssetGroup($RootData,[string]$Label) {
    $runs=[Collections.Generic.List[object]]::new()
    foreach ($run in @($RootData.index.runs | Where-Object { $_.group -eq 'asset' -and $_.repeat -le 3 } | Sort-Object repeat)) {
        $value=Get-AssetRun $run
        [void]($value | Add-Member -NotePropertyName label -NotePropertyValue $Label -PassThru)
        [void]$runs.Add($value)
    }
    return $runs.ToArray()
}
function Aggregate-Editor([object[]]$Runs,[string]$Label,[string]$Variant) {
    return [ordered]@{
        label=$Label; variant=$Variant; run_count=$Runs.Count
        wall=[ordered]@{median=Percentile (Values $Runs {param($r) $r.wall.median}) .5; p95=Percentile (Values $Runs {param($r) $r.wall.p95}) .5; p99=Percentile (Values $Runs {param($r) $r.wall.p99}) .5}
        gpu=[ordered]@{median=Percentile (Values $Runs {param($r) $r.gpu.median}) .5; p95=Percentile (Values $Runs {param($r) $r.gpu.p95}) .5; p99=Percentile (Values $Runs {param($r) $r.gpu.p99}) .5}
        actual_duration_ms=Percentile (Values $Runs {param($r) $r.actual_duration_ms}) .5
        process_cpu_ms_last=Percentile (Values $Runs {param($r) $r.process_cpu_ms_last}) .5
        process_cpu_ratio_1core=Percentile (Values $Runs {param($r) $r.process_cpu_ratio_1core}) .5
        process_cpu_ratio_16core=Percentile (Values $Runs {param($r) $r.process_cpu_ratio_16core}) .5
        working_set_peak=Percentile (Values $Runs {param($r) $r.memory.working_set_peak}) .5
        private_bytes_peak=Percentile (Values $Runs {param($r) $r.memory.private_bytes_peak}) .5
        vma_allocation_peak=Percentile (Values $Runs {param($r) $r.vma.allocation_peak}) .5
        target_width=Percentile (Values $Runs {param($r) $r.target_width}) .5; target_height=Percentile (Values $Runs {param($r) $r.target_height}) .5
        session_scene_frames=Percentile (Values $Runs {param($r) $r.session_scene_frames}) .5; timed_scene_frames=Percentile (Values $Runs {param($r) $r.timed_scene_frames}) .5
        all_dimensions_stable=(@($Runs | Where-Object { -not $_.dimensions_stable }).Count -eq 0)
        all_gpu_complete=(@($Runs | Where-Object { -not $_.gpu_complete }).Count -eq 0)
    }
}
function Aggregate-Asset([object[]]$Runs,[string]$Label) {
    $operationNames=@('move_redo_ms','move_undo_ms','move_redo_again_ms','move_final_undo_ms','copy_redo_ms','copy_undo_ms','copy_redo_again_ms','copy_final_undo_ms')
    $ops=[ordered]@{}
    foreach ($name in $operationNames) {
        $stats=@($Runs | ForEach-Object { $_.operation_metrics[$name] })
        $ops[$name]=[ordered]@{median=Percentile (Values $stats {param($s) $s.median}) .5; p95=Percentile (Values $stats {param($s) $s.p95}) .5; p99=Percentile (Values $stats {param($s) $s.p99}) .5}
    }
    return [ordered]@{
        label=$Label; variant='asset'; run_count=$Runs.Count
        initial_import_ms=Percentile (Values $Runs {param($r) $r.initial_import_ms}) .5
        cooked_cold_load_ms=Percentile (Values $Runs {param($r) $r.cooked_cold_load_ms}) .5
        cooked_warm_load_ms=Percentile (Values $Runs {param($r) $r.cooked_warm_load_ms}) .5
        material_document_load_unload_ms=Percentile (Values $Runs {param($r) $r.material_document_load_unload_ms}) .5
        material_private_peak=Percentile (Values $Runs {param($r) $r.material_private_peak}) .5
        material_working_peak=Percentile (Values $Runs {param($r) $r.material_working_peak}) .5
        cycle_private_peak=Percentile (Values $Runs {param($r) $r.cycle_private_peak}) .5
        cycle_working_peak=Percentile (Values $Runs {param($r) $r.cycle_working_peak}) .5
        all_cycles_restored=(@($Runs | Where-Object { -not $_.cycles_restored }).Count -eq 0)
        all_weak_owners_expired=(@($Runs | Where-Object { -not $_.weak_owners_expired }).Count -eq 0)
        all_hashes_restored=(@($Runs | Where-Object { -not $_.hashes_restored }).Count -eq 0)
        all_ids_restored=(@($Runs | Where-Object { -not $_.ids_restored }).Count -eq 0)
        all_cleanup_succeeded=(@($Runs | Where-Object { -not $_.cleanup_all }).Count -eq 0)
        operations=$ops
    }
}

$editorInputs=@([pscustomobject]@{label='baseline'; root=(Validate-InputRoot $EditorBaselineRoot 'editor')})
if ($EditorCandidateRoot) { $editorInputs+=([pscustomobject]@{label='candidate'; root=(Validate-InputRoot $EditorCandidateRoot 'editor')}) }
$assetInputs=@([pscustomobject]@{label='baseline'; root=(Validate-InputRoot $AssetBaselineRoot 'asset')})
if ($AssetCandidateRoot) { $assetInputs+=([pscustomobject]@{label='candidate'; root=(Validate-InputRoot $AssetCandidateRoot 'asset')}) }

$editorRuns=[Collections.Generic.List[object]]::new(); $editorAggregates=[Collections.Generic.List[object]]::new()
foreach ($entry in $editorInputs) {
    $groupRuns=@(Get-EditorGroup $entry.root $entry.label)
    $runs=[Collections.Generic.List[object]]::new()
    foreach ($item in $groupRuns) {
        if ($item -is [array]) {
            foreach ($nested in $item) {
                if ($null -ne (Get-Prop $nested 'id')) { [void]$runs.Add($nested) }
            }
        } elseif ($null -ne (Get-Prop $item 'id')) { [void]$runs.Add($item) }
    }
    foreach ($run in $runs) { [void]$editorRuns.Add($run) }
    $idleRuns=@($runs | Where-Object {(Get-Prop $_ 'group') -eq 'editor-idle'})
    $interactiveRuns=@($runs | Where-Object {(Get-Prop $_ 'group') -eq 'editor-interactive'})
    [void]$editorAggregates.Add((Aggregate-Editor $idleRuns $entry.label 'idle'))
    [void]$editorAggregates.Add((Aggregate-Editor $interactiveRuns $entry.label 'interactive'))
}
$assetRuns=[Collections.Generic.List[object]]::new(); $assetAggregates=[Collections.Generic.List[object]]::new()
foreach ($entry in $assetInputs) {
    $groupRuns=@(Get-AssetGroup $entry.root $entry.label)
    $runs=[Collections.Generic.List[object]]::new()
    foreach ($item in $groupRuns) {
        if ($item -is [array]) {
            foreach ($nested in $item) {
                if ($null -ne (Get-Prop $nested 'id')) { [void]$runs.Add($nested) }
            }
        } elseif ($null -ne (Get-Prop $item 'id')) { [void]$runs.Add($item) }
    }
    foreach ($run in $runs) { [void]$assetRuns.Add($run) }
    [void]$assetAggregates.Add((Aggregate-Asset $runs $entry.label))
}

$out=[IO.Path]::GetFullPath($OutputStem)
$jsonPath=$out; if ([IO.Path]::GetExtension($jsonPath) -ne '.json') { $jsonPath+='.json' }
$editorCsv=$out+'.editor.csv'; $assetCsv=$out+'.asset.csv'
$summary=[ordered]@{
    format='proto.benchmark.supplement.summary'; version=1; generated=(Get-Date -Format o)
    inputs=[ordered]@{editor=$editorInputs|ForEach-Object {[ordered]@{label=$_.label;root=$_.root.root;index=(Join-Path $_.root.root 'index.json')}}; asset=$assetInputs|ForEach-Object {[ordered]@{label=$_.label;root=$_.root.root;index=(Join-Path $_.root.root 'index.json')}}}
    editor=[ordered]@{runs=$editorRuns; aggregates=$editorAggregates}
    asset=[ordered]@{runs=$assetRuns; aggregates=$assetAggregates}
}
Save-Json $summary $jsonPath
$editorRows=@($editorRuns | ForEach-Object {
    [pscustomobject]@{label=$_.label; variant=$_.group; repeat=$_.repeat; wall_median_ms=$_.wall.median; wall_p95_ms=$_.wall.p95; wall_p99_ms=$_.wall.p99; gpu_median_ms=$_.gpu.median; gpu_p95_ms=$_.gpu.p95; gpu_p99_ms=$_.gpu.p99; process_cpu_ms_last=$_.process_cpu_ms_last; process_cpu_ratio_1core=$_.process_cpu_ratio_1core; process_cpu_ratio_16core=$_.process_cpu_ratio_16core; target_width=$_.target_width; target_height=$_.target_height; session_scene_frames=$_.session_scene_frames; timed_scene_frames=$_.timed_scene_frames; working_set_peak=$_.memory.working_set_peak; private_bytes_peak=$_.memory.private_bytes_peak; vma_allocation_peak=$_.vma.allocation_peak; dimensions_stable=$_.dimensions_stable; gpu_complete=$_.gpu_complete}
})
foreach ($aggregate in @($editorAggregates)) {
    $editorRows += [pscustomobject]@{
        label=$aggregate.label; variant=$aggregate.variant; repeat='aggregate'
        wall_median_ms=$aggregate.wall.median; wall_p95_ms=$aggregate.wall.p95; wall_p99_ms=$aggregate.wall.p99
        gpu_median_ms=$aggregate.gpu.median; gpu_p95_ms=$aggregate.gpu.p95; gpu_p99_ms=$aggregate.gpu.p99
        process_cpu_ms_last=$aggregate.process_cpu_ms_last; process_cpu_ratio_1core=$aggregate.process_cpu_ratio_1core; process_cpu_ratio_16core=$aggregate.process_cpu_ratio_16core
        target_width=$aggregate.target_width; target_height=$aggregate.target_height; session_scene_frames=$aggregate.session_scene_frames; timed_scene_frames=$aggregate.timed_scene_frames
        working_set_peak=$aggregate.working_set_peak; private_bytes_peak=$aggregate.private_bytes_peak; vma_allocation_peak=$aggregate.vma_allocation_peak
        dimensions_stable=$aggregate.all_dimensions_stable; gpu_complete=$aggregate.all_gpu_complete
    }
}
Save-Csv $editorRows $editorCsv
$assetRows=@($assetRuns | ForEach-Object {
    [pscustomobject]@{label=$_.label; repeat=$_.repeat; initial_import_ms=$_.initial_import_ms; cooked_cold_load_ms=$_.cooked_cold_load_ms; cooked_warm_load_ms=$_.cooked_warm_load_ms; material_document_load_unload_ms=$_.material_document_load_unload_ms; material_private_peak=$_.material_private_peak; cycle_private_peak=$_.cycle_private_peak; cycles_restored=$_.cycles_restored; weak_owners_expired=$_.weak_owners_expired; hashes_restored=$_.hashes_restored; ids_restored=$_.ids_restored; cleanup_all=$_.cleanup_all; move_redo_first_ms=$_.operation_metrics.move_redo_ms.median; move_undo_first_ms=$_.operation_metrics.move_undo_ms.median; move_redo_again_ms=$_.operation_metrics.move_redo_again_ms.median; move_undo_final_ms=$_.operation_metrics.move_final_undo_ms.median; copy_redo_first_ms=$_.operation_metrics.copy_redo_ms.median; copy_undo_first_ms=$_.operation_metrics.copy_undo_ms.median; copy_redo_again_ms=$_.operation_metrics.copy_redo_again_ms.median; copy_undo_final_ms=$_.operation_metrics.copy_final_undo_ms.median}
})
foreach ($aggregate in @($assetAggregates)) {
    $assetRows += [pscustomobject]@{
        label=$aggregate.label; repeat='aggregate'; initial_import_ms=$aggregate.initial_import_ms; cooked_cold_load_ms=$aggregate.cooked_cold_load_ms; cooked_warm_load_ms=$aggregate.cooked_warm_load_ms
        material_document_load_unload_ms=$aggregate.material_document_load_unload_ms; material_private_peak=$aggregate.material_private_peak; cycle_private_peak=$aggregate.cycle_private_peak
        cycles_restored=$aggregate.all_cycles_restored; weak_owners_expired=$aggregate.all_weak_owners_expired; hashes_restored=$aggregate.all_hashes_restored; ids_restored=$aggregate.all_ids_restored; cleanup_all=$aggregate.all_cleanup_succeeded
        move_redo_first_ms=$aggregate.operations.move_redo_ms.median; move_undo_first_ms=$aggregate.operations.move_undo_ms.median; move_redo_again_ms=$aggregate.operations.move_redo_again_ms.median; move_undo_final_ms=$aggregate.operations.move_final_undo_ms.median
        copy_redo_first_ms=$aggregate.operations.copy_redo_ms.median; copy_undo_first_ms=$aggregate.operations.copy_undo_ms.median; copy_redo_again_ms=$aggregate.operations.copy_redo_again_ms.median; copy_undo_final_ms=$aggregate.operations.copy_final_undo_ms.median
    }
}
Save-Csv $assetRows $assetCsv
Write-Output ('M7 supplement summary written: '+$jsonPath)
