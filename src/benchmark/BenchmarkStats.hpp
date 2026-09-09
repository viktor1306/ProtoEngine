#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace proto::benchmark {

struct BenchmarkGpuSample {
    uint64_t serial{};
    double milliseconds{};
};

struct BenchmarkProcessSample {
    bool valid{};
    double cpuMilliseconds{};
    uint64_t workingSetBytes{};
    uint64_t privateBytes{};
};

// VMA allocationBytes is the allocator's suballocation total. heapUsageBytes
// and heapBudgetBytes come from VK_EXT_memory_budget and describe the adapter
// heap budget, so they must not be presented as physical VRAM usage.
struct BenchmarkVmaSample {
    bool valid{};
    bool budgetAvailable{};
    uint64_t allocationBytes{};
    uint64_t peakAllocationBytes{};
    uint64_t heapUsageBytes{};
    uint64_t heapBudgetBytes{};
};

struct BenchmarkRenderSample {
    uint64_t rawInstances{};
    uint64_t visibleInstances{};
    uint64_t culledInstances{};
    uint32_t lights{};
    uint32_t depthDrawCalls{};
    uint32_t shadowDrawCalls{};
    uint32_t baseDrawCalls{};
    uint32_t lightingDrawCalls{};
    uint32_t toneMapDrawCalls{};
    uint32_t legacyDrawCalls{};
    uint32_t presenterDrawCalls{};
    uint32_t drawCalls{};
    uint32_t totalDrawCalls{};
    uint32_t allDrawCalls{};
    uint32_t dispatchCount{};
    uint32_t shadowFaces{};
    uint32_t shadowCacheHits{};
    uint32_t shadowMisses{};
    uint32_t shadowBatches{};
    uint64_t assetUploadBytes{};
    uint64_t dynamicLightingUploadBytes{};
    double gatherMilliseconds{};
    double planMilliseconds{};
    double uploadFrameMilliseconds{};
};

struct BenchmarkSample {
    uint64_t frame{};
    double wallMilliseconds{};
    double frameWaitMilliseconds{};
    double sceneAdvanceMilliseconds{};
    double viewBuildMilliseconds{};
    double renderSubmitMilliseconds{};
    bool gpuValid{};
    uint64_t gpuSerial{};
    double gpuMilliseconds{};
    BenchmarkProcessSample process;
    BenchmarkVmaSample vma;
    BenchmarkRenderSample render;
};

struct BenchmarkMetricSummary {
    uint64_t count{};
    double median{};
    double p95{};
    double p99{};
};

struct BenchmarkSummary {
    uint64_t rawFrameCount{};
    uint64_t sampleCount{};
    uint64_t gpuSampleCount{};
    double actualDurationMilliseconds{};
    BenchmarkMetricSummary wall;
    BenchmarkMetricSummary frameWait;
    BenchmarkMetricSummary sceneAdvance;
    BenchmarkMetricSummary viewBuild;
    BenchmarkMetricSummary renderSubmit;
    BenchmarkMetricSummary gpu;
};

struct BenchmarkReport {
    std::string scenario;
    std::string mode{"renderer"};
    std::string commandLine;
    std::string device;
    std::string buildId;
    std::string sourceHash;
    std::string shaderHash;
    std::string contentHash;
    std::string manifestJson;
    std::string gpuTimingStatus{"omitted"};
    std::string sampleSelection{"all"};
    uint32_t targetWidth{1280};
    uint32_t targetHeight{720};
    bool vSync{};
    bool validation{};
    double warmupSeconds{};
    double requestedSeconds{};
    double actualDurationMilliseconds{};
    uint64_t rawFrameCount{};
    uint64_t timedFrameCount{};
    uint64_t warmupFrameCount{};
    uint64_t gpuWarmupSerial{};
    uint64_t gpuEndSerial{};
    uint64_t gpuSamplesDropped{};
    uint64_t sampleCap{};
    uint64_t sampleStride{1};
    std::vector<BenchmarkSample> samples;
    std::vector<BenchmarkGpuSample> gpuSamples;
};

// Deterministic uniform reservoir for bounded raw-frame storage. `seen()` is
// the complete raw-frame count; `samples()` contains at most `capacity()`
// uniformly selected samples. This keeps fast CPU-only scenes from retaining
// only the beginning of a run while preserving a bounded report size.
class BenchmarkSampleReservoir {
  public:
    explicit BenchmarkSampleReservoir(size_t capacity, uint64_t seed = 0x9e3779b97f4a7c15ull);
    void add(BenchmarkSample sample);
    size_t capacity() const { return capacity_; }
    uint64_t seen() const { return seen_; }
    const std::vector<BenchmarkSample>& samples() const { return samples_; }
    std::vector<BenchmarkSample> take();

  private:
    uint64_t next();
    uint64_t bounded(uint64_t limit);
    std::vector<BenchmarkSample> samples_;
    size_t capacity_{};
    uint64_t seen_{};
    uint64_t state_{};
};

BenchmarkSummary summarize(const BenchmarkReport& report);

// --output accepts either a complete .json path or a path prefix. The
// resulting artifacts are <base>.manifest.json, <base>.json and <base>.csv.
struct BenchmarkOutputPaths {
    std::filesystem::path manifest;
    std::filesystem::path json;
    std::filesystem::path csv;
};
BenchmarkOutputPaths benchmarkOutputPaths(const std::filesystem::path& output);
void writeBenchmarkReports(const std::filesystem::path& output, const BenchmarkReport& report);

} // namespace proto::benchmark
