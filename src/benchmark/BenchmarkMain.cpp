#include "benchmark/BenchmarkScenes.hpp"
#include "benchmark/BenchmarkStats.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "runtime/PlayerGraphics.hpp"
#include "runtime/PlayerPresenter.hpp"
#include "runtime/SceneViewBuilder.hpp"
#include "renderer/AssetRenderer.hpp"
#include "renderer/VulkanRenderer.hpp"
#include <Proto/Build.hpp>

#include <GLFW/glfw3.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace proto::benchmark {
namespace {
constexpr uint32_t targetWidth = 1280;
constexpr uint32_t targetHeight = 720;
constexpr uint32_t windowWidth = 320;
constexpr uint32_t windowHeight = 180;
constexpr uint64_t resourceSampleInterval = 30;
constexpr size_t maxRawSamples = 250000;

struct Options {
    std::string scenario{"small"};
    double warmupSeconds{2};
    double seconds{5};
    uint64_t frames{};
    uint64_t captureFrame{240};
    bool vSync{};
    bool viewOnly{};
    std::filesystem::path output;
    std::filesystem::path capture;
    std::filesystem::path validationDir;
};

double finiteSeconds(std::wstring_view value, const char* option, double minimum, double maximum) {
    size_t parsed{};
    double result{};
    try {
        result = std::stod(std::wstring(value), &parsed);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + option);
    }
    if (parsed != value.size() || !std::isfinite(result))
        throw std::runtime_error(std::string("Invalid ") + option);
    return std::clamp(result, minimum, maximum);
}

uint64_t boundedInteger(std::wstring_view value, const char* option, uint64_t minimum, uint64_t maximum) {
    size_t parsed{};
    unsigned long long result{};
    try {
        result = std::stoull(std::wstring(value), &parsed, 10);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + option);
    }
    if (parsed != value.size() || result < minimum || result > maximum)
        throw std::runtime_error(std::string("Invalid ") + option);
    return static_cast<uint64_t>(result);
}

Options parseOptions(int argc, wchar_t** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument(argv[i]);
        const auto next = [&]() -> std::wstring_view {
            if (++i >= argc)
                throw std::runtime_error("Missing benchmark argument value");
            return argv[i];
        };
        if (argument == L"--scenario")
            result.scenario = utf8(next());
        else if (argument == L"--warmup")
            result.warmupSeconds = finiteSeconds(next(), "--warmup", 0, 3600);
        else if (argument == L"--seconds")
            result.seconds = finiteSeconds(next(), "--seconds", .001, 3600);
        else if (argument == L"--frames")
            result.frames = boundedInteger(next(), "--frames", 1, 10000000);
        else if (argument == L"--output")
            result.output = next();
        else if (argument == L"--capture")
            result.capture = next();
        else if (argument == L"--capture-frame")
            result.captureFrame = boundedInteger(next(), "--capture-frame", 0, 10000000);
        else if (argument == L"--validation-dir")
            result.validationDir = next();
        else if (argument == L"--vsync")
            result.vSync = true;
        else if (argument == L"--view-only")
            result.viewOnly = true;
        else if (argument == L"--help" || argument == L"-h") {
            std::cout << "ProtoBenchmark --scenario NAME --warmup SEC --seconds SEC --output PATH "
                         "[--capture PATH --capture-frame N] [--validation-dir PATH] [--vsync] [--frames N] "
                         "[--view-only]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown benchmark argument: " + utf8(argument));
        }
    }
    if (result.output.empty())
        throw std::runtime_error("--output is required");
    if (result.capture.empty() && result.captureFrame != 240)
        throw std::runtime_error("--capture-frame requires --capture");
    if (result.viewOnly && !result.capture.empty())
        throw std::runtime_error("--capture cannot be combined with --view-only");
    // Validate the name before opening a window or Vulkan loader.
    (void)makeBenchmarkScene(result.scenario);
    return result;
}

std::string commandLine() {
    const auto* raw = GetCommandLineW();
    return raw ? utf8(raw) : std::string{};
}

std::string benchmarkSourceHash() {
#ifdef PROTO_BENCHMARK_SOURCE_HASH
    return PROTO_BENCHMARK_SOURCE_HASH;
#else
    return "not-embedded";
#endif
}

std::string shaderHash(const std::filesystem::path& directory) {
    static constexpr std::string_view names[] = {"triangle.vert.spv", "triangle.frag.spv", "scene.vert.spv",
                                                  "scene.frag.spv",    "pbr.vert.spv",     "pbr.frag.spv",
                                                  "lit.vert.spv",      "lit.frag.spv",     "shadow.vert.spv",
                                                  "shadow.frag.spv",   "camera_depth.vert.spv", "camera_depth.frag.spv",
                                                  "light_tiles.comp.spv", "tone.vert.spv", "tone.frag.spv",
                                                  "player_present.vert.spv", "player_present.frag.spv"};
    std::string input;
    for (const auto name : names) {
        const auto bytes = assetBytes(directory / std::filesystem::path(name), 64 * 1024 * 1024);
        input += std::string(name) + ':' + sha256(bytes) + ';';
    }
    return sha256(input);
}

double processCpuMilliseconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return std::numeric_limits<double>::quiet_NaN();
    ULARGE_INTEGER kernelValue{}, userValue{};
    kernelValue.LowPart = kernel.dwLowDateTime;
    kernelValue.HighPart = kernel.dwHighDateTime;
    userValue.LowPart = user.dwLowDateTime;
    userValue.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernelValue.QuadPart + userValue.QuadPart) / 10000.0;
}

BenchmarkProcessSample processSample(double cpuBaseline = std::numeric_limits<double>::quiet_NaN()) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    BenchmarkProcessSample result;
    const auto cpu = processCpuMilliseconds();
    if (std::isfinite(cpu)) {
        result.valid = true;
        result.cpuMilliseconds = std::isfinite(cpuBaseline) ? std::max(0.0, cpu - cpuBaseline) : cpu;
    }
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                              sizeof(memory)))
        return result;
    result.workingSetBytes = static_cast<uint64_t>(memory.WorkingSetSize);
    result.privateBytes = static_cast<uint64_t>(memory.PrivateUsage);
    return result;
}

BenchmarkRenderSample renderSample(const RenderView& view, const AssetRenderer* assets) {
    BenchmarkRenderSample result;
    result.rawInstances = std::max(view.imported.size(), view.casters.size());
    result.visibleInstances = view.imported.size();
    result.culledInstances = result.rawInstances >= result.visibleInstances
                                 ? result.rawInstances - result.visibleInstances
                                 : 0;
    if (!assets)
        return result;
    const auto& stats = assets->lightingStats();
    result.rawInstances = stats.rawInstances;
    result.visibleInstances = stats.visibleInstances;
    result.culledInstances = stats.culledInstances;
    result.lights = stats.lights;
    result.depthDrawCalls = stats.depthDrawCalls;
    result.shadowDrawCalls = stats.shadowDrawCalls;
    result.baseDrawCalls = stats.baseDrawCalls;
    result.lightingDrawCalls = stats.lightingDrawCalls;
    result.toneMapDrawCalls = stats.toneMapDrawCalls;
    result.legacyDrawCalls = stats.legacyDrawCalls;
    // PlayerPresenter records one fullscreen draw for every real renderer
    // frame once the offscreen target exists. Keep it separate from scene
    // pass counters so the scene breakdown remains comparable to Editor.
    result.presenterDrawCalls = assets ? 1u : 0u;
    result.drawCalls = stats.drawCalls;
    result.totalDrawCalls = stats.totalDrawCalls;
    result.allDrawCalls = result.totalDrawCalls + result.presenterDrawCalls;
    result.dispatchCount = stats.dispatchCount;
    result.shadowFaces = stats.shadowFaces;
    result.shadowCacheHits = stats.shadowCacheHits;
    result.shadowMisses = stats.shadowMisses;
    result.shadowBatches = stats.batches;
    result.assetUploadBytes = assets->uploadedBytes();
    result.dynamicLightingUploadBytes = stats.dynamicLightingUploadBytes;
    result.gatherMilliseconds = stats.gatherMs;
    result.planMilliseconds = stats.planMs;
    result.uploadFrameMilliseconds = stats.uploadFrameMs;
    return result;
}

void appendGpuSamples(VulkanRenderer& renderer, BenchmarkReport& report, uint64_t minimumSerial,
                      uint64_t maximumSerial = std::numeric_limits<uint64_t>::max()) {
    const auto completed = renderer.consumeGpuTimestampSamples();
    for (const auto& sample : completed) {
        if (sample.serial <= minimumSerial || sample.serial > maximumSerial)
            continue;
        if (std::none_of(report.gpuSamples.begin(), report.gpuSamples.end(),
                         [&](const auto& existing) { return existing.serial == sample.serial; })) {
            report.gpuSamples.push_back({sample.serial, sample.milliseconds});
        }
    }
}

void appendSample(BenchmarkSampleReservoir& reservoir, BenchmarkSample sample) {
    reservoir.add(std::move(sample));
}

struct BenchmarkWindow {
    GLFWwindow* handle{};
    BenchmarkWindow() {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        vkCheck(volkInitialize(), "Benchmark Vulkan loader");
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
        if (!glfwInit()) {
            volkFinalize();
            throw std::runtime_error("Benchmark GLFW initialization failed");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        handle = glfwCreateWindow(windowWidth, windowHeight, "ProtoBenchmark", nullptr, nullptr);
        if (!handle) {
            glfwTerminate();
            volkFinalize();
            throw std::runtime_error("Cannot create benchmark GLFW window");
        }
    }
    ~BenchmarkWindow() {
        if (handle)
            glfwDestroyWindow(handle);
        glfwTerminate();
        volkFinalize();
    }
};

std::filesystem::path shaderDirectory() {
    const auto executable = executableDirectory() / "shaders";
    if (std::filesystem::exists(executable / "pbr.vert.spv"))
        return executable;
    const auto source = std::filesystem::current_path() / "shaders";
    if (std::filesystem::exists(source / "pbr.vert.spv"))
        return source;
    throw std::runtime_error("Compiled benchmark shaders are missing");
}

void prepareView(BenchmarkScene& scene, RenderView& view) {
    buildRuntimeView(scene.scene, VkExtent2D{targetWidth, targetHeight}, view);
    applyPlayerGraphics(view, VkExtent2D{targetWidth, targetHeight}, scene.graphics);
}

void advanceBenchmarkSceneAt(BenchmarkScene& scene, double seconds) {
    scene.advanceAt(seconds);
}

BenchmarkReport cpuBenchmark(const Options& options, const std::string& command) {
    auto scene = makeBenchmarkScene(options.scenario);
    BenchmarkReport report;
    report.scenario = scene.scenario;
    report.mode = "view-only";
    report.commandLine = command;
    report.device = "cpu-only";
    report.buildId = proto::sdk::buildId;
    report.contentHash = scene.contentHash;
    report.manifestJson = benchmarkSceneManifest(scene);
    report.sourceHash = benchmarkSourceHash();
    report.shaderHash = "not-loaded";
    report.vSync = options.vSync;
    report.warmupSeconds = options.warmupSeconds;
    report.requestedSeconds = options.seconds;
    report.sampleCap = maxRawSamples;
    report.sampleStride = 1;

    RenderView view;
    uint64_t frame = 0;
    double cpuBaseline = std::numeric_limits<double>::quiet_NaN();
    BenchmarkSampleReservoir reservoir(maxRawSamples);
    const auto runPhase = [&](double seconds, bool timed) {
        const auto phaseStart = Clock::now();
        const auto timeout = std::chrono::duration<double>(seconds + 30.0);
        uint64_t phaseFrames{};
        for (;;) {
            const auto now = Clock::now();
            if ((timed && (std::chrono::duration<double>(now - phaseStart).count() >= seconds ||
                           (options.frames && phaseFrames >= options.frames))) ||
                (!timed && std::chrono::duration<double>(now - phaseStart).count() >= seconds))
                break;
            if (now - phaseStart > timeout)
                throw std::runtime_error("CPU benchmark phase timed out");
            const auto frameStart = Clock::now();
            const auto advanceStart = Clock::now();
            const double phaseSeconds = phaseFrames ? std::chrono::duration<double>(now - phaseStart).count() : 0.0;
            advanceBenchmarkSceneAt(scene, phaseSeconds);
            ++frame;
            const auto advanceMs = milliseconds(advanceStart);
            const auto viewStart = Clock::now();
            prepareView(scene, view);
            const auto viewMs = milliseconds(viewStart);
            ++phaseFrames;
            if (!timed)
                continue;
            BenchmarkSample sample;
            sample.frame = phaseFrames - 1;
            sample.wallMilliseconds = milliseconds(frameStart);
            sample.sceneAdvanceMilliseconds = advanceMs;
            sample.viewBuildMilliseconds = viewMs;
            sample.render = renderSample(view, nullptr);
            sample.process = (phaseFrames % resourceSampleInterval == 1) ? processSample(cpuBaseline)
                                                                          : BenchmarkProcessSample{};
            appendSample(reservoir, std::move(sample));
            ++report.timedFrameCount;
        }
        return milliseconds(phaseStart);
    };
    runPhase(options.warmupSeconds, false);
    cpuBaseline = processCpuMilliseconds();
    const auto timedDuration = runPhase(options.seconds, true);
    report.warmupFrameCount = frame - report.timedFrameCount;
    report.rawFrameCount = report.timedFrameCount;
    report.actualDurationMilliseconds = timedDuration;
    report.samples = reservoir.take();
    report.sampleSelection = "uniform_reservoir";
    report.gpuTimingStatus = "omitted-view-only";
    return report;
}

BenchmarkReport rendererBenchmark(const Options& options, const std::string& command) {
    auto scene = makeBenchmarkScene(options.scenario);
    const auto shaders = shaderDirectory();
    const auto paths = benchmarkOutputPaths(options.output);
    auto logPath = paths.json;
    logPath.replace_extension(L".log");
    Diagnostics log(logPath);
    if (!options.validationDir.empty()) {
        if (!std::filesystem::exists(options.validationDir / "VkLayer_khronos_validation.json"))
            throw std::runtime_error("Benchmark validation files missing");
        SetEnvironmentVariableW(L"VK_LAYER_PATH", options.validationDir.c_str());
    }

    BenchmarkReport report;
    report.scenario = scene.scenario;
    report.commandLine = command;
    report.buildId = proto::sdk::buildId;
    report.contentHash = scene.contentHash;
    report.manifestJson = benchmarkSceneManifest(scene);
    report.sourceHash = benchmarkSourceHash();
    report.shaderHash = shaderHash(shaders);
    report.warmupSeconds = options.warmupSeconds;
    report.requestedSeconds = options.seconds;
    report.vSync = options.vSync;
    report.validation = !options.validationDir.empty();
    report.sampleCap = maxRawSamples;
    report.sampleStride = 1;

    BenchmarkWindow window;
    VulkanRenderer renderer(log);
    renderer.initialize(window.handle, shaders, report.validation);
    renderer.setGpuTimestampCollectionEnabled(true);
    renderer.setVSync(options.vSync);
    report.device = renderer.deviceName();
    PlayerPresenter presenter(renderer, shaders);
    RenderView view;
    double cpuBaseline = std::numeric_limits<double>::quiet_NaN();
    BenchmarkSampleReservoir reservoir(maxRawSamples);
    const auto draw = [&](const std::filesystem::path& capture = std::filesystem::path{}, RenderView* drawView = nullptr) {
        return renderer.draw([&](VkCommandBuffer commandBuffer) { presenter.record(renderer, commandBuffer); }, capture,
                             drawView ? drawView : &view);
    };
    const auto runFrame = [&](double phaseSeconds, bool timed) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.handle))
            return false;
        const auto frameStart = Clock::now();
        if (!renderer.prepare({targetWidth, targetHeight}))
            return false;
        const auto waitMs = renderer.frameStats().frameWaitMs;
        const auto advanceStart = Clock::now();
        advanceBenchmarkSceneAt(scene, phaseSeconds);
        const auto advanceMs = milliseconds(advanceStart);
        const auto viewStart = Clock::now();
        prepareView(scene, view);
        const auto viewMs = milliseconds(viewStart);
        if (!draw())
            return false;
        if (!timed)
            return true;
        BenchmarkSample sample;
        sample.frame = report.timedFrameCount;
        sample.wallMilliseconds = milliseconds(frameStart);
        sample.frameWaitMilliseconds = waitMs;
        sample.sceneAdvanceMilliseconds = advanceMs;
        sample.viewBuildMilliseconds = viewMs;
        sample.renderSubmitMilliseconds = renderer.frameStats().renderSubmitMs;
        sample.render = renderSample(view, renderer.assetRenderer());
        if (report.timedFrameCount % resourceSampleInterval == 0) {
            sample.process = processSample(cpuBaseline);
            const auto memory = renderer.memoryStats();
            sample.vma.valid = true;
            sample.vma.budgetAvailable = memory.budgetAvailable;
            sample.vma.allocationBytes = memory.allocationBytes;
            sample.vma.peakAllocationBytes = memory.peakAllocationBytes;
            sample.vma.heapUsageBytes = memory.heapUsageBytes;
            sample.vma.heapBudgetBytes = memory.heapBudgetBytes;
        }
        appendGpuSamples(renderer, report, report.gpuWarmupSerial, std::numeric_limits<uint64_t>::max());
        appendSample(reservoir, std::move(sample));
        ++report.timedFrameCount;
        return true;
    };
    const auto warmupStart = Clock::now();
    uint64_t warmupFrame{};
    while (std::chrono::duration<double>(Clock::now() - warmupStart).count() < options.warmupSeconds) {
        const auto elapsed = std::chrono::duration<double>(Clock::now() - warmupStart).count();
        if (!runFrame(warmupFrame ? elapsed : 0.0, false))
            throw std::runtime_error("Benchmark warmup could not render a frame");
        ++warmupFrame;
        appendGpuSamples(renderer, report, std::numeric_limits<uint64_t>::max());
    }
    report.warmupFrameCount = warmupFrame;
    report.gpuWarmupSerial = renderer.submittedGpuTimestampSerial();
    cpuBaseline = processCpuMilliseconds();

    const auto timedStart = Clock::now();
    uint64_t timedFrame{};
    while (std::chrono::duration<double>(Clock::now() - timedStart).count() < options.seconds &&
           (!options.frames || timedFrame < options.frames)) {
        const auto elapsed = std::chrono::duration<double>(Clock::now() - timedStart).count();
        if (!runFrame(timedFrame ? elapsed : 0.0, true))
            throw std::runtime_error("Benchmark timed phase could not render a frame");
        ++timedFrame;
    }
    report.actualDurationMilliseconds = milliseconds(timedStart);
    report.rawFrameCount = report.timedFrameCount;
    report.gpuEndSerial = renderer.submittedGpuTimestampSerial();
    renderer.waitIdle();
    appendGpuSamples(renderer, report, report.gpuWarmupSerial, report.gpuEndSerial);
    report.gpuSamplesDropped = renderer.droppedGpuTimestampSamples();

    std::sort(report.gpuSamples.begin(), report.gpuSamples.end(),
              [](const auto& left, const auto& right) { return left.serial < right.serial; });
    const auto completeGpuCoverage = [&]() {
        if (!renderer.hasGpuTiming() || !report.timedFrameCount)
            return true;
        if (report.gpuSamplesDropped || renderer.gpuTimestampDrainOmitted())
            return false;
        if (report.gpuEndSerial < report.gpuWarmupSerial)
            return false;
        const auto expected = report.gpuEndSerial - report.gpuWarmupSerial;
        if (expected != report.gpuSamples.size())
            return false;
        for (uint64_t serial = report.gpuWarmupSerial + 1; serial <= report.gpuEndSerial; ++serial)
            if (report.gpuSamples[static_cast<size_t>(serial - report.gpuWarmupSerial - 1)].serial != serial)
                return false;
        return true;
    };
    if (!completeGpuCoverage())
        throw std::runtime_error("Benchmark GPU timestamp coverage is incomplete");

    if (!options.capture.empty()) {
        auto captureScene = makeBenchmarkScene(options.scenario);
        captureScene.advance(options.captureFrame);
        RenderView captureView;
        if (!renderer.prepare({targetWidth, targetHeight}))
            throw std::runtime_error("Benchmark capture target is unavailable");
        buildRuntimeView(captureScene.scene, VkExtent2D{targetWidth, targetHeight}, captureView);
        applyPlayerGraphics(captureView, VkExtent2D{targetWidth, targetHeight}, captureScene.graphics);
        ensureParent(options.capture);
        if (!draw(options.capture, &captureView))
            throw std::runtime_error("Benchmark capture could not render a frame");
        renderer.waitIdle();
        // Capture serials are intentionally drained and excluded from the
        // timed GPU range above.
        (void)renderer.consumeGpuTimestampSamples();
    }
    report.samples = reservoir.take();
    report.sampleSelection = "uniform_reservoir";
    report.gpuTimingStatus = !renderer.hasGpuTiming()
                                 ? "unavailable"
                                 : (renderer.droppedGpuTimestampSamples() ? "omitted-dropped"
                                 : (renderer.gpuTimestampDrainOmitted() ? "omitted-pending"
                                                                         : (report.gpuSamples.empty()
                                                                                ? "available-no-samples"
                                                                                : "complete")));
    return report;
}

} // namespace
} // namespace proto::benchmark

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        const auto options = proto::benchmark::parseOptions(argc, argv);
        const auto command = proto::benchmark::commandLine();
        auto report = options.viewOnly ? proto::benchmark::cpuBenchmark(options, command)
                                       : proto::benchmark::rendererBenchmark(options, command);
        proto::benchmark::writeBenchmarkReports(options.output, report);
        std::cout << "ProtoBenchmark completed: " << report.timedFrameCount << " frames, "
                  << report.actualDurationMilliseconds << " ms\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[BENCHMARK ERROR] " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "[BENCHMARK ERROR] Unknown C++ exception\n";
        return 2;
    }
}
