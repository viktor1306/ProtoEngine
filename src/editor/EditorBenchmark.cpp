#include "editor/Editor.hpp"

#include "assets/AssetIO.hpp"
#include "benchmark/BenchmarkStats.hpp"
#include "core/Diagnostics.hpp"

#include <Proto/Build.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace proto {
namespace {
struct ProcessMemoryCounters {
    DWORD cb{};
    DWORD pageFaultCount{};
    SIZE_T peakWorkingSetSize{};
    SIZE_T workingSetSize{};
    SIZE_T quotaPeakPagedPoolUsage{};
    SIZE_T quotaPagedPoolUsage{};
    SIZE_T quotaPeakNonPagedPoolUsage{};
    SIZE_T quotaNonPagedPoolUsage{};
    SIZE_T pagefileUsage{};
    SIZE_T peakPagefileUsage{};
    SIZE_T privateUsage{};
};
using QueryMemoryFn = BOOL(WINAPI*)(HANDLE, ProcessMemoryCounters*, DWORD);

template <class Function> Function functionAddress(FARPROC address) {
    static_assert(sizeof(Function) == sizeof(address));
    Function result{};
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

struct ProcessSample {
    bool valid{};
    double cpuMs{};
    uint64_t workingSetBytes{};
    uint64_t privateBytes{};
};

uint64_t fileTimeTicks(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

double processCpuMilliseconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return std::numeric_limits<double>::quiet_NaN();
    // Windows process times are 100 ns units.  Keep the conversion in double
    // only after subtracting the two small counters.
    return static_cast<double>(fileTimeTicks(kernel) + fileTimeTicks(user)) / 10000.0;
}

ProcessSample processSample(double cpuBaseline) {
    ProcessSample result;
    const auto cpu = processCpuMilliseconds();
    if (std::isfinite(cpu) && std::isfinite(cpuBaseline)) {
        result.valid = true;
        result.cpuMs = std::max(0.0, cpu - cpuBaseline);
    }

    // K32GetProcessMemoryInfo is resolved at runtime so the normal editor
    // target does not acquire a new import/library dependency for the
    // benchmark-only diagnostics.
    const auto module = GetModuleHandleW(L"kernel32.dll");
    const auto query = module ? functionAddress<QueryMemoryFn>(GetProcAddress(module, "K32GetProcessMemoryInfo"))
                              : nullptr;
    if (!query)
        return result;
    ProcessMemoryCounters counters{};
    counters.cb = sizeof(counters);
    if (!query(GetCurrentProcess(), &counters, sizeof(counters)))
        return result;
    result.valid = true;
    result.workingSetBytes = static_cast<uint64_t>(counters.workingSetSize);
    result.privateBytes = static_cast<uint64_t>(counters.privateUsage);
    return result;
}

} // namespace

struct EditorBenchmarkState {
    std::filesystem::path output;
    double requestedSeconds{};
    double warmupSeconds{};
    bool interactive{};
    bool started{};
    bool finished{};
    uint64_t observedFrames{};
    uint64_t warmupFrames{};
    Clock::time_point startedAt{};
    Clock::time_point timedAt{};
    Clock::time_point endedAt{};
    float initialYaw{.65f};
    float initialPitch{.45f};
    double processCpuBaseline{std::numeric_limits<double>::quiet_NaN()};
    benchmark::BenchmarkReport report;
    std::unordered_set<uint64_t> gpuSerials;
    uint64_t lastGpuSerial{};
    uint64_t peakAllocationBytes{};
    uint64_t peakWorkingSetBytes{};
    uint64_t peakPrivateBytes{};
    bool gpuWarmupBoundaryCaptured{};
    bool currentTimedFrame{};
    uint64_t currentFrame{};
    VkExtent2D timedViewport{};
    VkExtent2D timedSwap{};
    bool dimensionsCaptured{};
    bool dimensionsStable{true};
    uint64_t timedRenderedFrames{};
    uint64_t sceneFramesAtTimedBoundary{};
    uint64_t sceneFramesAtTimedEnd{};
    uint64_t gpuEndSerial{};
    bool gpuEndBoundaryCaptured{};
    struct DimensionSample {
        uint64_t frame{};
        VkExtent2D viewport{};
        VkExtent2D swap{};
    };
    std::vector<DimensionSample> dimensions;
};

void drainGpu(EditorBenchmarkState& state, VulkanRenderer& renderer) {
    for (const auto sample : renderer.consumeGpuTimestampSamples()) {
        if (!sample.serial || !state.gpuSerials.insert(sample.serial).second)
            continue;
        if (!state.gpuWarmupBoundaryCaptured || sample.serial <= state.report.gpuWarmupSerial)
            continue;
        state.report.gpuSamples.push_back({sample.serial, sample.milliseconds});
        state.lastGpuSerial = std::max(state.lastGpuSerial, sample.serial);
    }
}

std::string commandLine() {
    const auto* raw = GetCommandLineW();
    return raw ? utf8(raw) : std::string{};
}

std::string shaderHash(const std::filesystem::path& directory) {
    static constexpr std::string_view names[] = {
        "triangle.vert.spv",       "triangle.frag.spv",     "scene.vert.spv",       "scene.frag.spv",
        "pbr.vert.spv",            "pbr.frag.spv",          "lit.vert.spv",         "lit.frag.spv",
        "shadow.vert.spv",         "shadow.frag.spv",       "camera_depth.vert.spv", "camera_depth.frag.spv",
        "light_tiles.comp.spv",    "tone.vert.spv",         "tone.frag.spv",        "player_present.vert.spv",
        "player_present.frag.spv"};
    std::string input;
    for (const auto name : names) {
        const auto bytes = assetBytes(directory / std::filesystem::path(name), 64 * 1024 * 1024);
        input += std::string(name) + ':' + sha256(bytes) + ';';
    }
    return sha256(input);
}

void Editor::startBenchmark(const std::filesystem::path& output, double seconds, double warmupSeconds,
                            bool interactive) {
    if (output.empty())
        throw std::invalid_argument("Editor benchmark output path is empty");
    if (!(seconds > 0) || !std::isfinite(seconds) || seconds > 3600)
        throw std::invalid_argument("Editor benchmark seconds must be finite and in (0, 3600]");
    if (!(warmupSeconds >= 0) || !std::isfinite(warmupSeconds) || warmupSeconds > 3600)
        throw std::invalid_argument("Editor benchmark warmup must be finite and in [0, 3600]");

    benchmark_ = std::make_shared<EditorBenchmarkState>();
    benchmark_->output = output;
    benchmark_->requestedSeconds = seconds;
    benchmark_->warmupSeconds = warmupSeconds;
    benchmark_->interactive = interactive;
    benchmark_->report.scenario = "editor-empty";
    benchmark_->report.mode = interactive ? "editor_interactive" : "editor_idle";
    benchmark_->report.commandLine = commandLine();
    benchmark_->report.buildId = sdk::buildId;
#ifdef PROTO_BENCHMARK_SOURCE_HASH
    benchmark_->report.sourceHash = PROTO_BENCHMARK_SOURCE_HASH;
#else
    benchmark_->report.sourceHash = "unavailable";
#endif
    try {
        benchmark_->report.shaderHash = shaderHash(executableDirectory() / "shaders");
    } catch (...) {
        benchmark_->report.shaderHash = "unavailable";
    }
    benchmark_->report.warmupSeconds = warmupSeconds;
    benchmark_->report.requestedSeconds = seconds;
    benchmark_->report.vSync = renderer_.vSync();
    benchmark_->report.gpuTimingStatus = "unavailable";
    benchmark_->report.manifestJson =
        "{\"format\":\"proto.benchmark.scene\",\"version\":1,\"scenario\":\"editor-empty\","
        "\"authorObjects\":0,\"builtinGrid\":true,\"cameraPath\":" +
        jsonString(interactive ? "programmatic_yaw_pitch_orbit" : "static") +
        ",\"timers\":\"viewBuildMs and renderSubmitMs are wall-clock stage durations; frameWaitMs is reported separately.\","
        "\"notes\":\"The grid is editor chrome and is excluded from author object counts.\"}";
    benchmark_->report.contentHash = sha256(benchmark_->report.manifestJson);
    // The renderer keeps a bounded queue only for opted-in benchmark runs;
    // regular Editor/Player sessions continue to use their last GPU sample.
    renderer_.setGpuTimestampCollectionEnabled(true);

    // A benchmark always uses a transient, empty authored scene.  This avoids
    // loading a project and means the normal editor settings/layout file is
    // not touched.  Grid/docking still render through the real Editor path.
    document_.scene = Scene{};
    document_.scene.name = "M7 Empty Benchmark";
    document_.scene.update();
    document_.selection = {};
    camera_ = {};
    benchmark_->initialYaw = camera_.yaw;
    benchmark_->initialPitch = camera_.pitch;
    showGrid_ = true;
    useSceneCamera_ = false;
    useStudioLighting_ = false;
    resetLayout_ = true;
}

bool Editor::benchmarkActive() const { return benchmark_ && !benchmark_->finished; }

bool Editor::benchmarkFinished() const { return benchmark_ && benchmark_->finished; }

void Editor::benchmarkBeginFrame() {
    if (!benchmark_ || benchmark_->finished)
        return;
    auto& state = *benchmark_;
    const auto now = Clock::now();
    if (!state.started) {
        state.started = true;
        state.startedAt = now;
        state.processCpuBaseline = processCpuMilliseconds();
        state.report.gpuTimingStatus = renderer_.hasGpuTiming() ? "completed_serial_queue" : "unavailable";
    }
    ++state.observedFrames;
    state.currentFrame = state.observedFrames;
    drainGpu(state, renderer_);
    const double elapsed = milliseconds(state.startedAt);
    state.currentTimedFrame = elapsed >= state.warmupSeconds * 1000.0;
    if (!state.currentTimedFrame) {
        ++state.warmupFrames;
        return;
    }
    if (state.timedAt == Clock::time_point{}) {
        state.timedAt = now;
        state.report.warmupFrameCount = state.warmupFrames;
        // Capture the last submitted serial before this timed frame is
        // submitted. Completed samples after this boundary belong to the
        // timed interval; older in-flight warmup work does not.
        state.report.gpuWarmupSerial = renderer_.submittedGpuTimestampSerial();
        state.gpuWarmupBoundaryCaptured = true;
        state.sceneFramesAtTimedBoundary = renderer_.sceneRenderFrames();
        state.processCpuBaseline = processCpuMilliseconds();
    }
    if (state.interactive) {
        // This is an explicit deterministic programmatic path; it does not
        // claim human mouse/keyboard interaction. The phase starts at timed0.
        const double phaseSeconds = std::chrono::duration<double>(now - state.timedAt).count();
        camera_.yaw = state.initialYaw + static_cast<float>(phaseSeconds * .8);
        camera_.pitch = state.initialPitch + .07f * std::sin(static_cast<float>(phaseSeconds * 1.1));
    }
}

void Editor::benchmarkFrame(double wallMs, double uiMs, double submitMs, double frameWaitMs, VkExtent2D viewportExtent,
                            VkExtent2D swapExtent, bool rendered) {
    if (!benchmark_ || benchmark_->finished)
        return;
    auto& state = *benchmark_;
    if (!state.currentTimedFrame) {
        state.currentTimedFrame = false;
        return;
    }
    const auto now = Clock::now();
    drainGpu(state, renderer_);
    if (rendered) {
        const auto viewport = viewportExtent;
        const auto swap = swapExtent;
        if (!state.dimensionsCaptured) {
            state.timedViewport = viewport;
            state.timedSwap = swap;
            state.dimensionsCaptured = true;
        } else if (state.timedViewport.width != viewport.width || state.timedViewport.height != viewport.height ||
                   state.timedSwap.width != swap.width || state.timedSwap.height != swap.height) {
            state.dimensionsStable = false;
        }
        state.dimensions.push_back({state.currentFrame, viewport, swap});
        benchmark::BenchmarkSample sample;
        sample.frame = state.currentFrame;
        sample.wallMilliseconds = wallMs;
        sample.frameWaitMilliseconds = frameWaitMs;
        sample.viewBuildMilliseconds = uiMs;
        sample.renderSubmitMilliseconds = submitMs;
        sample.render.rawInstances = 0;
        sample.render.visibleInstances = 0;
        sample.render.culledInstances = 0;
        sample.vma.valid = true;
        sample.vma.allocationBytes = renderer_.allocatedBytes();
        state.peakAllocationBytes = std::max(state.peakAllocationBytes, sample.vma.allocationBytes);
        sample.vma.peakAllocationBytes = state.peakAllocationBytes;

        const auto process = processSample(state.processCpuBaseline);
        sample.process.valid = process.valid;
        sample.process.cpuMilliseconds = process.cpuMs;
        sample.process.workingSetBytes = process.workingSetBytes;
        sample.process.privateBytes = process.privateBytes;
        state.peakWorkingSetBytes = std::max(state.peakWorkingSetBytes, process.workingSetBytes);
        state.peakPrivateBytes = std::max(state.peakPrivateBytes, process.privateBytes);
        state.report.samples.push_back(std::move(sample));
        ++state.timedRenderedFrames;
    }
    state.currentTimedFrame = false;
    const double timedElapsed = state.timedAt == Clock::time_point{}
                                    ? 0
                                    : std::chrono::duration<double, std::milli>(now - state.timedAt).count();
    if (timedElapsed >= state.requestedSeconds * 1000.0) {
        state.endedAt = now;
        state.finished = true;
        state.sceneFramesAtTimedEnd = renderer_.sceneRenderFrames();
        state.gpuEndSerial = renderer_.submittedGpuTimestampSerial();
        state.gpuEndBoundaryCaptured = true;
        state.report.actualDurationMilliseconds =
            state.timedAt == Clock::time_point{}
                ? 0
                : std::chrono::duration<double, std::milli>(state.endedAt - state.timedAt).count();
    }
}

void Editor::finishBenchmark(uint64_t rawFrames, uint64_t sceneFrames) {
    if (!benchmark_)
        return;
    auto& state = *benchmark_;
    if (!state.started)
        throw std::runtime_error("Editor benchmark did not start a frame");
    const bool completed = state.finished && state.gpuEndBoundaryCaptured && state.timedRenderedFrames > 0 &&
                           state.dimensionsCaptured;
    if (state.endedAt == Clock::time_point{})
        state.endedAt = Clock::now();
    state.report.rawFrameCount = state.timedRenderedFrames;
    state.report.timedFrameCount = state.report.samples.size();
    state.report.actualDurationMilliseconds =
        state.timedAt == Clock::time_point{}
            ? 0
            : std::chrono::duration<double, std::milli>(state.endedAt - state.timedAt).count();
    for (const auto sample : renderer_.consumeGpuTimestampSamples()) {
        if (!sample.serial || !state.gpuSerials.insert(sample.serial).second ||
            sample.serial <= state.report.gpuWarmupSerial || !state.gpuEndBoundaryCaptured ||
            sample.serial > state.gpuEndSerial)
            continue;
        state.report.gpuSamples.push_back({sample.serial, sample.milliseconds});
        state.lastGpuSerial = std::max(state.lastGpuSerial, sample.serial);
    }
    std::sort(state.report.gpuSamples.begin(), state.report.gpuSamples.end(),
              [](const auto& left, const auto& right) { return left.serial < right.serial; });
    state.report.gpuEndSerial = state.gpuEndBoundaryCaptured ? state.gpuEndSerial : state.lastGpuSerial;
    state.report.gpuSamplesDropped = renderer_.droppedGpuTimestampSamples();
    bool gpuCoverageValid = true;
    if (renderer_.hasGpuTiming()) {
        gpuCoverageValid = completed && !renderer_.gpuTimestampDrainOmitted() && state.report.gpuSamplesDropped == 0;
        uint64_t expected = state.timedRenderedFrames;
        gpuCoverageValid &= state.report.gpuEndSerial >= state.report.gpuWarmupSerial &&
                            state.report.gpuEndSerial - state.report.gpuWarmupSerial == expected;
        gpuCoverageValid &= state.report.gpuSamples.size() == expected;
        for (size_t i = 0; gpuCoverageValid && i < state.report.gpuSamples.size(); ++i) {
            const auto expectedSerial = state.report.gpuWarmupSerial + 1 + static_cast<uint64_t>(i);
            gpuCoverageValid &= state.report.gpuSamples[i].serial == expectedSerial;
        }
        if (!gpuCoverageValid)
            state.report.gpuTimingStatus = "incomplete_serial_range";
    }
    state.report.device = renderer_.deviceName();
    state.report.validation = renderer_.validationEnabled();
    state.report.targetWidth = state.timedViewport.width;
    state.report.targetHeight = state.timedViewport.height;
    state.report.vSync = renderer_.vSync();
    if (state.report.gpuTimingStatus == "unavailable")
        state.report.gpuTimingStatus = "unavailable";
    state.report.manifestJson = state.report.manifestJson.substr(0, state.report.manifestJson.size() - 1) +
                                ",\"renderTarget\":{\"width\":" + std::to_string(state.timedViewport.width) +
                                ",\"height\":" + std::to_string(state.timedViewport.height) +
                                "},\"swapExtent\":{\"width\":" + std::to_string(state.timedSwap.width) +
                                ",\"height\":" + std::to_string(state.timedSwap.height) + "}}";
    state.report.contentHash = sha256(state.report.manifestJson);
    renderer_.setGpuTimestampCollectionEnabled(false);
    benchmark::writeBenchmarkReports(state.output, state.report);

    const auto outputPaths = benchmark::benchmarkOutputPaths(state.output);
    auto observationsPath = outputPaths.json;
    observationsPath.replace_filename(outputPaths.json.stem().wstring() + L".observations.json");
    ensureParent(observationsPath);
    const auto timedSceneFrames = state.sceneFramesAtTimedEnd >= state.sceneFramesAtTimedBoundary
                                      ? state.sceneFramesAtTimedEnd - state.sceneFramesAtTimedBoundary
                                      : 0;
    std::ofstream observations(observationsPath, std::ios::binary | std::ios::trunc);
    if (!observations)
        throw std::runtime_error("Cannot open editor benchmark observations output");
    observations << "{\"format\":\"proto.editor.benchmark.observations\",\"version\":1"
                 << ",\"sessionRenderedFrames\":" << rawFrames << ",\"sessionSceneFrames\":" << sceneFrames
                 << ",\"warmupFrameCount\":" << state.warmupFrames
                 << ",\"timedRenderedFrames\":" << state.timedRenderedFrames
                 << ",\"timedSceneFrames\":" << timedSceneFrames
                 << ",\"dimensionsStable\":" << (state.dimensionsStable ? "true" : "false")
                 << ",\"gpuCoverageValid\":" << (gpuCoverageValid ? "true" : "false")
                 << ",\"gpuWarmupSerial\":" << state.report.gpuWarmupSerial
                 << ",\"gpuEndSerial\":" << state.report.gpuEndSerial
                 << ",\"gpuSamplesDropped\":" << state.report.gpuSamplesDropped << ",\"dimensions\":[";
    for (size_t i = 0; i < state.dimensions.size(); ++i) {
        if (i)
            observations << ',';
        const auto& dimensions = state.dimensions[i];
        observations << "{\"frame\":" << dimensions.frame << ",\"viewport\":{\"width\":"
                     << dimensions.viewport.width << ",\"height\":" << dimensions.viewport.height
                     << "},\"swap\":{\"width\":" << dimensions.swap.width << ",\"height\":"
                     << dimensions.swap.height << "}}";
    }
    observations << "]}\n";
    if (!completed || !state.dimensionsStable || !gpuCoverageValid)
        throw std::runtime_error("Editor benchmark did not complete with stable dimensions and complete GPU coverage");
}

} // namespace proto
