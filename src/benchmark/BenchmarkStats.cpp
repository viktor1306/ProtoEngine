#include "benchmark/BenchmarkStats.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace proto::benchmark {
namespace {
using Values = std::vector<double>;

BenchmarkMetricSummary metric(Values values) {
    BenchmarkMetricSummary result;
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !std::isfinite(value); }),
                 values.end());
    if (values.empty())
        return result;
    std::sort(values.begin(), values.end());
    result.count = values.size();
    const auto percentile = [&](double p) {
        const double position = p * static_cast<double>(values.size() - 1);
        const auto lower = static_cast<size_t>(position);
        const auto upper = std::min(lower + 1, values.size() - 1);
        return values[lower] + (values[upper] - values[lower]) * (position - static_cast<double>(lower));
    };
    result.median = percentile(.50);
    result.p95 = percentile(.95);
    result.p99 = percentile(.99);
    return result;
}

std::string number(double value) {
    if (!std::isfinite(value))
        return "null";
    std::ostringstream stream;
    stream << std::setprecision(12) << value;
    return stream.str();
}

std::string metricJson(const BenchmarkMetricSummary& value) {
    return "{\"count\":" + std::to_string(value.count) + ",\"median\":" +
           (value.count ? number(value.median) : "null") + ",\"p95\":" +
           (value.count ? number(value.p95) : "null") + ",\"p99\":" +
           (value.count ? number(value.p99) : "null") + "}";
}

void writeSampleJson(std::ostream& out, const BenchmarkSample& sample) {
    const auto& r = sample.render;
    out << "{\"frame\":" << sample.frame << ",\"wall_ms\":" << number(sample.wallMilliseconds)
        << ",\"frame_wait_ms\":" << number(sample.frameWaitMilliseconds)
        << ",\"scene_advance_ms\":" << number(sample.sceneAdvanceMilliseconds)
        << ",\"view_build_ms\":" << number(sample.viewBuildMilliseconds)
        << ",\"render_submit_ms\":" << number(sample.renderSubmitMilliseconds)
        << ",\"gpu_valid\":" << (sample.gpuValid ? "true" : "false")
        << ",\"gpu_serial\":" << sample.gpuSerial << ",\"gpu_ms\":"
        << (sample.gpuValid ? number(sample.gpuMilliseconds) : "null")
        << ",\"process_valid\":" << (sample.process.valid ? "true" : "false")
        << ",\"process_cpu_ms\":" << (sample.process.valid ? number(sample.process.cpuMilliseconds) : "null")
        << ",\"working_set_bytes\":"
        << (sample.process.valid ? std::to_string(sample.process.workingSetBytes) : "null")
        << ",\"private_bytes\":"
        << (sample.process.valid ? std::to_string(sample.process.privateBytes) : "null")
        << ",\"vma_valid\":" << (sample.vma.valid ? "true" : "false")
        << ",\"vma_budget_available\":" << (sample.vma.budgetAvailable ? "true" : "false")
        << ",\"vma_allocation_bytes\":"
        << (sample.vma.valid ? std::to_string(sample.vma.allocationBytes) : "null")
        << ",\"vma_peak_allocation_bytes\":"
        << (sample.vma.valid ? std::to_string(sample.vma.peakAllocationBytes) : "null")
        << ",\"vma_heap_usage_bytes\":"
        << (sample.vma.valid ? std::to_string(sample.vma.heapUsageBytes) : "null")
        << ",\"vma_heap_budget_bytes\":"
        << (sample.vma.valid ? std::to_string(sample.vma.heapBudgetBytes) : "null")
        << ",\"raw_instances\":" << r.rawInstances << ",\"visible_instances\":" << r.visibleInstances
        << ",\"culled_instances\":" << r.culledInstances << ",\"lights\":" << r.lights
        << ",\"depth_draw_calls\":" << r.depthDrawCalls << ",\"shadow_draw_calls\":" << r.shadowDrawCalls
        << ",\"base_draw_calls\":" << r.baseDrawCalls
        << ",\"lighting_draw_calls\":" << r.lightingDrawCalls
        << ",\"tone_map_draw_calls\":" << r.toneMapDrawCalls
        << ",\"legacy_draw_calls\":" << r.legacyDrawCalls
        << ",\"presenter_draw_calls\":" << r.presenterDrawCalls << ",\"draw_calls\":" << r.drawCalls
        << ",\"total_draw_calls\":" << r.totalDrawCalls << ",\"all_draw_calls\":" << r.allDrawCalls
        << ",\"dispatch_count\":" << r.dispatchCount
        << ",\"shadow_faces\":" << r.shadowFaces << ",\"shadow_cache_hits\":" << r.shadowCacheHits
        << ",\"shadow_misses\":" << r.shadowMisses << ",\"shadow_batches\":" << r.shadowBatches
        << ",\"asset_upload_bytes\":" << r.assetUploadBytes
        << ",\"dynamic_lighting_upload_bytes\":" << r.dynamicLightingUploadBytes
        << ",\"gather_ms\":" << number(r.gatherMilliseconds)
        << ",\"plan_ms\":" << number(r.planMilliseconds)
        << ",\"upload_frame_ms\":" << number(r.uploadFrameMilliseconds) << "}";
}

void csvSample(std::ostream& out, const BenchmarkSample& sample) {
    const auto& r = sample.render;
    out << sample.frame << ',' << number(sample.wallMilliseconds) << ',' << number(sample.frameWaitMilliseconds) << ','
        << number(sample.sceneAdvanceMilliseconds) << ',' << number(sample.viewBuildMilliseconds) << ','
        << number(sample.renderSubmitMilliseconds) << ',' << (sample.gpuValid ? 1 : 0) << ',' << sample.gpuSerial
        << ',' << (sample.gpuValid ? number(sample.gpuMilliseconds) : "") << ',' << (sample.process.valid ? 1 : 0)
        << ',' << (sample.process.valid ? number(sample.process.cpuMilliseconds) : "") << ','
        << (sample.process.valid ? std::to_string(sample.process.workingSetBytes) : "") << ','
        << (sample.process.valid ? std::to_string(sample.process.privateBytes) : "") << ','
        << (sample.vma.valid ? 1 : 0) << ',' << (sample.vma.valid ? std::to_string(sample.vma.allocationBytes) : "")
        << ',' << (sample.vma.valid ? std::to_string(sample.vma.peakAllocationBytes) : "") << ','
        << (sample.vma.valid ? std::to_string(sample.vma.heapUsageBytes) : "") << ','
        << (sample.vma.valid ? std::to_string(sample.vma.heapBudgetBytes) : "") << ',' << r.rawInstances << ','
        << r.visibleInstances << ',' << r.culledInstances << ',' << r.lights << ',' << r.depthDrawCalls << ','
        << r.shadowDrawCalls << ',' << r.baseDrawCalls << ',' << r.lightingDrawCalls << ',' << r.toneMapDrawCalls
        << ',' << r.legacyDrawCalls << ',' << r.presenterDrawCalls << ',' << r.drawCalls << ',' << r.totalDrawCalls
        << ',' << r.allDrawCalls << ',' << r.dispatchCount << ','
        << r.shadowFaces << ',' << r.shadowCacheHits << ',' << r.shadowMisses << ',' << r.shadowBatches << ','
        << r.assetUploadBytes << ',' << r.dynamicLightingUploadBytes << ',' << number(r.gatherMilliseconds) << ','
        << number(r.planMilliseconds) << ',' << number(r.uploadFrameMilliseconds) << '\n';
}

} // namespace

BenchmarkSampleReservoir::BenchmarkSampleReservoir(size_t capacity, uint64_t seed)
    : capacity_(capacity), state_(seed ? seed : 1) {
    samples_.reserve(capacity);
}

uint64_t BenchmarkSampleReservoir::next() {
    // xorshift64* is sufficient here: this is a deterministic bounded sample
    // selector, not a security or simulation random source.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 2685821657736338717ull;
}

uint64_t BenchmarkSampleReservoir::bounded(uint64_t limit) {
    if (!limit)
        return 0;
    const auto threshold = uint64_t(-limit) % limit;
    for (;;) {
        const auto value = next();
        if (value >= threshold)
            return value % limit;
    }
}

void BenchmarkSampleReservoir::add(BenchmarkSample sample) {
    if (seen_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Benchmark sample count overflow");
    const auto index = seen_++;
    if (!capacity_)
        return;
    if (samples_.size() < capacity_) {
        samples_.push_back(std::move(sample));
        return;
    }
    const auto replacement = bounded(index + 1);
    if (replacement < capacity_)
        samples_[static_cast<size_t>(replacement)] = std::move(sample);
}

std::vector<BenchmarkSample> BenchmarkSampleReservoir::take() {
    return std::move(samples_);
}

BenchmarkSummary summarize(const BenchmarkReport& report) {
    BenchmarkSummary result;
    result.rawFrameCount = report.rawFrameCount;
    result.sampleCount = report.samples.size();
    result.actualDurationMilliseconds = report.actualDurationMilliseconds;
    Values wall, frameWait, sceneAdvance, viewBuild, renderSubmit;
    wall.reserve(report.samples.size());
    frameWait.reserve(report.samples.size());
    sceneAdvance.reserve(report.samples.size());
    viewBuild.reserve(report.samples.size());
    renderSubmit.reserve(report.samples.size());
    for (const auto& sample : report.samples) {
        wall.push_back(sample.wallMilliseconds);
        frameWait.push_back(sample.frameWaitMilliseconds);
        sceneAdvance.push_back(sample.sceneAdvanceMilliseconds);
        viewBuild.push_back(sample.viewBuildMilliseconds);
        renderSubmit.push_back(sample.renderSubmitMilliseconds);
    }
    result.wall = metric(std::move(wall));
    result.frameWait = metric(std::move(frameWait));
    result.sceneAdvance = metric(std::move(sceneAdvance));
    result.viewBuild = metric(std::move(viewBuild));
    result.renderSubmit = metric(std::move(renderSubmit));

    // A renderer may finish a query one or two frames after submission. Keep
    // the serial in the summary and guard against accidental duplicate drains.
    Values gpu;
    std::unordered_set<uint64_t> serials;
    for (const auto& sample : report.gpuSamples)
        if (sample.serial && serials.insert(sample.serial).second)
            gpu.push_back(sample.milliseconds);
    result.gpuSampleCount = gpu.size();
    result.gpu = metric(std::move(gpu));
    return result;
}

BenchmarkOutputPaths benchmarkOutputPaths(const std::filesystem::path& output) {
    if (output.empty())
        throw std::invalid_argument("Benchmark output path is empty");
    auto base = output;
    if (base.extension() == L".json" || base.extension() == L".csv")
        base.replace_extension();
    return {base.wstring() + L".manifest.json", base.wstring() + L".json", base.wstring() + L".csv"};
}

void writeBenchmarkReports(const std::filesystem::path& output, const BenchmarkReport& report) {
    const auto paths = benchmarkOutputPaths(output);
    ensureParent(paths.manifest);
    ensureParent(paths.json);
    ensureParent(paths.csv);
    const auto summary = summarize(report);

    {
        std::ofstream file(paths.manifest, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot open benchmark manifest output");
        file << (report.manifestJson.empty() ? "{}" : report.manifestJson) << '\n';
    }
    {
        std::ofstream file(paths.json, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot open benchmark JSON output");
        file << "{\"format\":\"proto.benchmark\",\"version\":1,\"scenario\":"
             << jsonString(report.scenario) << ",\"mode\":" << jsonString(report.mode)
             << ",\"commandLine\":" << jsonString(report.commandLine) << ",\"device\":"
             << jsonString(report.device) << ",\"buildId\":" << jsonString(report.buildId)
             << ",\"sourceHash\":" << jsonString(report.sourceHash)
             << ",\"shaderHash\":" << jsonString(report.shaderHash)
             << ",\"contentHash\":" << jsonString(report.contentHash)
             << ",\"target\":{\"width\":" << report.targetWidth << ",\"height\":" << report.targetHeight
             << "},\"vSync\":" << (report.vSync ? "true" : "false") << ",\"validation\":"
             << (report.validation ? "true" : "false") << ",\"warmupSeconds\":" << number(report.warmupSeconds)
             << ",\"requestedSeconds\":" << number(report.requestedSeconds)
             << ",\"actualDurationMs\":" << number(report.actualDurationMilliseconds)
             << ",\"rawFrameCount\":" << report.rawFrameCount << ",\"timedFrameCount\":"
             << report.timedFrameCount << ",\"warmupFrameCount\":" << report.warmupFrameCount
             << ",\"gpuWarmupSerial\":" << report.gpuWarmupSerial << ",\"gpuEndSerial\":"
             << report.gpuEndSerial << ",\"sampleCap\":" << report.sampleCap << ",\"sampleStride\":"
             << report.sampleStride << ",\"gpuSamplesDropped\":" << report.gpuSamplesDropped
             << ",\"gpuTimingStatus\":" << jsonString(report.gpuTimingStatus)
             << ",\"sampleSelection\":" << jsonString(report.sampleSelection)
             << ",\"summary\":{\"raw_frame_count\":" << summary.rawFrameCount
             << ",\"sample_count\":" << summary.sampleCount << ",\"gpu_sample_count\":"
             << summary.gpuSampleCount << ",\"actual_duration_ms\":" << number(summary.actualDurationMilliseconds)
             << ",\"wall_ms\":" << metricJson(summary.wall) << ",\"frame_wait_ms\":"
             << metricJson(summary.frameWait) << ",\"scene_advance_ms\":" << metricJson(summary.sceneAdvance)
             << ",\"view_build_ms\":" << metricJson(summary.viewBuild) << ",\"render_submit_ms\":"
             << metricJson(summary.renderSubmit) << ",\"gpu_ms\":" << metricJson(summary.gpu) << "},\"gpuSamples\":[";
        for (size_t i = 0; i < report.gpuSamples.size(); ++i) {
            if (i)
                file << ',';
            file << "{\"serial\":" << report.gpuSamples[i].serial << ",\"milliseconds\":"
                 << number(report.gpuSamples[i].milliseconds) << '}';
        }
        file << "],\"samples\":[";
        for (size_t i = 0; i < report.samples.size(); ++i) {
            if (i)
                file << ',';
            writeSampleJson(file, report.samples[i]);
        }
        file << "]}\n";
    }
    {
        std::ofstream file(paths.csv, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot open benchmark CSV output");
        file << "frame,wall_ms,frame_wait_ms,scene_advance_ms,view_build_ms,render_submit_ms,gpu_valid,gpu_serial,gpu_ms,"
                "process_valid,process_cpu_ms,working_set_bytes,private_bytes,vma_valid,vma_allocation_bytes,"
                "vma_peak_allocation_bytes,vma_heap_usage_bytes,vma_heap_budget_bytes,raw_instances,visible_instances,"
                "culled_instances,lights,depth_draw_calls,shadow_draw_calls,base_draw_calls,lighting_draw_calls,"
                "tone_map_draw_calls,legacy_draw_calls,presenter_draw_calls,draw_calls,total_draw_calls,all_draw_calls,"
                "dispatch_count,shadow_faces,"
                "shadow_cache_hits,shadow_misses,shadow_batches,asset_upload_bytes,dynamic_lighting_upload_bytes,"
                "gather_ms,plan_ms,upload_frame_ms\n";
        for (const auto& sample : report.samples)
            csvSample(file, sample);
        file << "\nsummary_metric,count,median,p95,p99\n";
        const auto summaryRow = [&](const char* name, const BenchmarkMetricSummary& value) {
            file << name << ',' << value.count << ','
                 << (value.count ? number(value.median) : "") << ',' << (value.count ? number(value.p95) : "") << ','
                 << (value.count ? number(value.p99) : "") << '\n';
        };
        summaryRow("wall_ms", summary.wall);
        summaryRow("frame_wait_ms", summary.frameWait);
        summaryRow("scene_advance_ms", summary.sceneAdvance);
        summaryRow("view_build_ms", summary.viewBuild);
        summaryRow("render_submit_ms", summary.renderSubmit);
        summaryRow("gpu_ms", summary.gpu);
        file << "\ngpu_sample_serial,gpu_ms\n";
        for (const auto& gpu : report.gpuSamples)
            file << gpu.serial << ',' << number(gpu.milliseconds) << '\n';
    }
}

} // namespace proto::benchmark
