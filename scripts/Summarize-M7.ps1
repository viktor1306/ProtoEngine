[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputRoot,
    [string]$OutputStem
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($InputRoot)
if (-not $OutputStem) { $OutputStem=Join-Path $root 'summary' }
$index=Get-Content -LiteralPath (Join-Path $root 'index.json') -Raw | ConvertFrom-Json
if (-not $index.complete) { throw 'Only a completed matrix can be summarized' }

function Quantile($Values,[double]$P=.5) {
    $sorted=@($Values | Where-Object { $null -ne $_ -and [double]::IsFinite([double]$_) } | Sort-Object)
    if (-not $sorted.Count) { return $null }
    $position=$P*($sorted.Count-1)
    $lower=[int][Math]::Floor($position)
    $upper=[int][Math]::Ceiling($position)
    return [double]$sorted[$lower]+([double]$sorted[$upper]-[double]$sorted[$lower])*($position-$lower)
}
function Maximum($Values) { return ($Values | Measure-Object -Maximum).Maximum }
$runs=foreach ($entry in $index.runs) {
    $path=Join-Path $root ($entry.phase+'/'+$entry.scenario+'-r'+$entry.repeat+'.json')
    $r=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($r.gpuTimingStatus -ne 'complete' -or $r.rawFrameCount -ne $r.timedFrameCount -or
        $r.gpuSamples.Count -ne $r.timedFrameCount -or $r.validation) {
        throw "Invalid performance run: $path"
    }
    $sample=$r.samples
    [pscustomobject][ordered]@{
        scenario=$entry.scenario; phase=$entry.phase; repeat=$entry.repeat;
        source_hash=$r.sourceHash; build_id=$r.buildId; content_hash=$r.contentHash; shader_hash=$r.shaderHash;
        timed_frames=$r.timedFrameCount; actual_duration_ms=$r.actualDurationMs;
        measured_frames_per_second=$r.timedFrameCount*1000.0/$r.actualDurationMs;
        wall_median_ms=$r.summary.wall_ms.median; wall_p95_ms=$r.summary.wall_ms.p95; wall_p99_ms=$r.summary.wall_ms.p99;
        gpu_median_ms=$r.summary.gpu_ms.median; gpu_p95_ms=$r.summary.gpu_ms.p95; gpu_p99_ms=$r.summary.gpu_ms.p99;
        wait_median_ms=$r.summary.frame_wait_ms.median; view_median_ms=$r.summary.view_build_ms.median;
        gather_median_ms=(Quantile $sample.gather_ms); plan_median_ms=(Quantile $sample.plan_ms);
        upload_median_ms=(Quantile $sample.upload_frame_ms);
        raw_instances=(Maximum $sample.raw_instances); visible_min=(($sample.visible_instances | Measure-Object -Minimum).Minimum);
        visible_max=(Maximum $sample.visible_instances); lights_min=(($sample.lights | Measure-Object -Minimum).Minimum);
        lights_max=(Maximum $sample.lights); draws_median=(Quantile $sample.all_draw_calls);
        dispatches_median=(Quantile $sample.dispatch_count); shadow_faces_median=(Quantile $sample.shadow_faces);
        shadow_cache_hits_median=(Quantile $sample.shadow_cache_hits); batches_median=(Quantile $sample.shadow_batches);
        asset_upload_max_bytes=(Maximum $sample.asset_upload_bytes);
        dynamic_upload_median_bytes=(Quantile $sample.dynamic_lighting_upload_bytes);
        private_peak_bytes=(Maximum $sample.private_bytes); working_set_peak_bytes=(Maximum $sample.working_set_bytes);
        vma_peak_bytes=(Maximum $sample.vma_allocation_bytes); vma_budget_peak_bytes=(Maximum $sample.vma_heap_budget_bytes)
    }
}
$phases=@($runs.phase | Sort-Object -Unique)
$metrics=@('wall_median_ms','wall_p95_ms','wall_p99_ms','gpu_median_ms','gpu_p95_ms','gpu_p99_ms',
    'wait_median_ms','view_median_ms','gather_median_ms','plan_median_ms','upload_median_ms','measured_frames_per_second',
    'private_peak_bytes','working_set_peak_bytes','vma_peak_bytes','draws_median','dispatches_median','shadow_faces_median',
    'shadow_cache_hits_median','batches_median','asset_upload_max_bytes','dynamic_upload_median_bytes')
$groups=foreach ($scenario in $index.scenarios) {
    $sceneRuns=@($runs | Where-Object scenario -eq $scenario)
    if (@($sceneRuns.content_hash | Sort-Object -Unique).Count -ne 1 -or
        @($sceneRuns.shader_hash | Sort-Object -Unique).Count -ne 1) { throw "Content/shaders changed in $scenario" }
    foreach ($phase in $phases) {
        $values=@($sceneRuns | Where-Object phase -eq $phase)
        if ($values.Count -ne $index.repeats) { throw "Incomplete repetitions: $scenario/$phase" }
        $row=[ordered]@{scenario=$scenario; phase=$phase; repeats=$values.Count;
            timed_frames_min=(($values.timed_frames | Measure-Object -Minimum).Minimum);
            timed_frames_max=(Maximum $values.timed_frames); raw_instances=(Maximum $values.raw_instances);
            visible_min=(($values.visible_min | Measure-Object -Minimum).Minimum); visible_max=(Maximum $values.visible_max);
            lights_min=(($values.lights_min | Measure-Object -Minimum).Minimum); lights_max=(Maximum $values.lights_max)}
        foreach ($metric in $metrics) { $row[$metric]=Quantile $values.$metric }
        $row['wall_run_median_min_ms']=($values.wall_median_ms | Measure-Object -Minimum).Minimum
        $row['wall_run_median_max_ms']=Maximum $values.wall_median_ms
        [pscustomobject]$row
    }
}
$comparison=foreach ($scenario in $index.scenarios) {
    $before=@($groups | Where-Object { $_.scenario -eq $scenario -and $_.phase -eq 'baseline' })
    $after=@($groups | Where-Object { $_.scenario -eq $scenario -and $_.phase -eq 'candidate' })
    if ($before.Count -and $after.Count) {
        $row=[ordered]@{scenario=$scenario}
        foreach ($metric in $metrics) {
            $a=$before[0].$metric; $b=$after[0].$metric
            $row['before_'+$metric]=$a; $row['after_'+$metric]=$b
            $row['change_percent_'+$metric]=if ($a) { 100.0*($b/$a-1) } else { $null }
        }
        [pscustomobject]$row
    }
}
$document=[ordered]@{format='proto.benchmark.summary'; version=1;
    aggregation='Median of per-run medians/p95/p99, not pooled percentiles; run ranges retained. Timed throughput includes benchmark overhead, active frame wall excludes event/metric work. GPU and CPU rows have independent serials.';
    caveat='Runs below 1000 frames provide sparse p99 evidence. Integrated-GPU heap budget is not physical VRAM.';
    runs=@($runs); groups=@($groups); comparison=@($comparison)}
$parent=Split-Path -Parent ([IO.Path]::GetFullPath($OutputStem))
New-Item -ItemType Directory -Force -Path $parent | Out-Null
[IO.File]::WriteAllText($OutputStem+'.json',($document | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
$runs | Export-Csv -LiteralPath ($OutputStem+'-runs.csv') -NoTypeInformation -Encoding utf8
$groups | Export-Csv -LiteralPath ($OutputStem+'-groups.csv') -NoTypeInformation -Encoding utf8
if (@($comparison).Count) { $comparison | Export-Csv -LiteralPath ($OutputStem+'-comparison.csv') -NoTypeInformation -Encoding utf8 }
Write-Output ('M7 summary: '+$OutputStem+'.json')
