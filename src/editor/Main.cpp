#include "editor/Editor.hpp"
#include "editor/ClipboardProbe.hpp"
#include "renderer/AssetRenderer.hpp"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace proto {
namespace {
struct Options {
    bool smoke{};
    bool m0{}, sceneSmoke{}, studio{}, sceneCamera{}, gizmoSmoke{}, playSmoke{}, exportSmoke{};
    bool benchmarkInteractive{};
    uint64_t frames{};
    double benchmarkSeconds{5}, benchmarkWarmupSeconds{2};
    float uiScale{1};
    std::filesystem::path capture, report, scene, assetSmoke, lightingSmoke, project, projectSmoke, benchmark,
        acceptanceSmoke;
    bool automated() const { return smoke || frames != 0 || !benchmark.empty(); }
};
Options parse(int count, wchar_t** args) {
    Options result;
    for (int i = 1; i < count; ++i) {
        const std::wstring_view arg(args[i]);
        const auto value = [&]() -> const wchar_t* {
            if (++i >= count)
                throw std::runtime_error("Missing value for " + utf8(arg));
            return args[i];
        };
        if (arg == L"--smoke-test") {
            result.smoke = true;
            result.m0 = true;
            result.frames = 82;
        } else if (arg == L"--scene-smoke") {
            result.smoke = true;
            result.sceneSmoke = true;
            result.frames = 82;
        } else if (arg == L"--m0")
            result.m0 = true;
        else if (arg == L"--scene")
            result.scene = value();
        else if (arg == L"--project")
            result.project = value();
        else if (arg == L"--gizmo-smoke") {
            result.gizmoSmoke = true;
            result.frames = 48;
        } else if (arg == L"--project-smoke") {
            result.projectSmoke = value();
            result.frames = 100;
        } else if (arg == L"--asset-smoke") {
            result.assetSmoke = value();
            result.frames = 100;
        } else if (arg == L"--lighting-smoke") {
            result.lightingSmoke = value();
            result.frames = 4;
        } else if (arg == L"--play-smoke") {
            result.playSmoke = true;
            result.frames = 100;
        } else if (arg == L"--export-smoke") {
            result.exportSmoke = true;
            result.frames = 50;
        } else if (arg == L"--acceptance-smoke") {
            result.acceptanceSmoke = value();
            result.frames = 50;
        } else if (arg == L"--benchmark")
            result.benchmark = value();
        else if (arg == L"--benchmark-seconds") {
            const std::wstring input(value());
            size_t parsed{};
            result.benchmarkSeconds = std::stod(input, &parsed);
            if (parsed != input.size() || !(result.benchmarkSeconds > 0) || result.benchmarkSeconds > 3600)
                throw std::runtime_error("--benchmark-seconds must be finite and in (0, 3600]");
        } else if (arg == L"--benchmark-warmup") {
            const std::wstring input(value());
            size_t parsed{};
            result.benchmarkWarmupSeconds = std::stod(input, &parsed);
            if (parsed != input.size() || !(result.benchmarkWarmupSeconds >= 0) || result.benchmarkWarmupSeconds > 3600)
                throw std::runtime_error("--benchmark-warmup must be finite and in [0, 3600]");
        } else if (arg == L"--benchmark-interactive")
            result.benchmarkInteractive = true;
        else if (arg == L"--studio")
            result.studio = true;
        else if (arg == L"--scene-camera")
            result.sceneCamera = true;
        else if (arg == L"--frames") {
            const std::wstring input(value());
            size_t parsed{};
            result.frames = std::stoull(input, &parsed);
            if (parsed != input.size() || result.frames < 4 || result.frames > 1000000)
                throw std::runtime_error("--frames must be between 4 and 1000000");
        } else if (arg == L"--capture")
            result.capture = value();
        else if (arg == L"--report")
            result.report = value();
        else if (arg == L"--ui-scale") {
            const std::wstring input(value());
            size_t parsed{};
            result.uiScale = std::stof(input, &parsed);
            if (parsed != input.size() || !(result.uiScale >= 0.75f && result.uiScale <= 2))
                throw std::runtime_error("--ui-scale must be between 0.75 and 2");
        } else
            throw std::runtime_error("Unknown argument: " + utf8(arg));
    }
    if (result.smoke && result.frames < 82)
        throw std::runtime_error("Smoke test requires at least 82 frames");
    if (result.m0 && (result.sceneSmoke || !result.scene.empty()))
        throw std::runtime_error("M0 diagnostics cannot load an M1 scene");
    if (!result.assetSmoke.empty() && (result.m0 || result.sceneSmoke || !result.scene.empty()))
        throw std::runtime_error("Asset smoke requires its own scene");
    if (!result.lightingSmoke.empty() &&
        (result.m0 || result.smoke || result.studio || !result.scene.empty() || !result.assetSmoke.empty()))
        throw std::runtime_error("Lighting smoke requires its own scene and scene lighting");
    if (result.benchmarkInteractive && result.benchmark.empty())
        throw std::runtime_error("--benchmark-interactive requires --benchmark OUTPUT");
    if (!result.acceptanceSmoke.empty() &&
        (result.m0 || result.smoke || result.studio || result.sceneCamera || result.gizmoSmoke ||
         result.playSmoke || result.exportSmoke || !result.benchmark.empty() || !result.scene.empty() ||
         !result.project.empty() || !result.assetSmoke.empty() || !result.lightingSmoke.empty() ||
         !result.projectSmoke.empty()))
        throw std::runtime_error("--acceptance-smoke requires an independent Editor session");
    if (!result.benchmark.empty() &&
        (result.m0 || result.smoke || result.sceneSmoke || result.studio || result.sceneCamera || result.gizmoSmoke ||
         result.playSmoke || result.exportSmoke || result.frames != 0 || !result.scene.empty() || !result.project.empty() ||
         !result.assetSmoke.empty() || !result.lightingSmoke.empty() || !result.projectSmoke.empty()))
        throw std::runtime_error("--benchmark requires an independent empty Editor session");
    const int workflows = int(result.smoke || result.m0) + int(!result.assetSmoke.empty()) +
                          int(!result.lightingSmoke.empty()) + int(result.gizmoSmoke) +
                          int(!result.projectSmoke.empty());
    if ((result.gizmoSmoke || !result.projectSmoke.empty() || !result.project.empty()) &&
        (workflows > 1 || !result.scene.empty() || (!result.project.empty() && workflows)))
        throw std::runtime_error("Project and gizmo diagnostics require an independent session");
    return result;
}
struct ComApartment {
    ComApartment() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
            throw std::runtime_error("COM initialization failed");
    }
    ~ComApartment() { CoUninitialize(); }
};
struct Window {
    GLFWwindow* handle{};
    bool initialized{};
    unsigned closeEvents{};
    unsigned framebufferEvents{};
    explicit Window(Diagnostics& log) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        vkCheck(volkInitialize(), "Load system Vulkan driver");
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
        static Diagnostics* glfwLog{};
        glfwLog = &log;
        glfwSetErrorCallback(
            [](int code, const char* text) { glfwLog->write("GLFW", std::to_string(code) + ": " + text); });
        if (!glfwInit()) {
            volkFinalize();
            throw std::runtime_error("GLFW initialization failed");
        }
        initialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        handle = glfwCreateWindow(1360, 860, "Proto Engine — v0.1", nullptr, nullptr);
        if (!handle) {
            glfwTerminate();
            volkFinalize();
            throw std::runtime_error("Native window creation failed");
        }
        glfwSetWindowSizeLimits(handle, 760, 520, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwSetWindowUserPointer(handle, this);
        glfwSetWindowCloseCallback(
            handle, [](GLFWwindow* window) { ++static_cast<Window*>(glfwGetWindowUserPointer(window))->closeEvents; });
        glfwSetFramebufferSizeCallback(handle, [](GLFWwindow* window, int, int) {
            ++static_cast<Window*>(glfwGetWindowUserPointer(window))->framebufferEvents;
        });
        log.write("INFO", "Native GLFW window created; per-monitor DPI requested");
    }
    ~Window() {
        if (handle)
            glfwDestroyWindow(handle);
        if (initialized)
            glfwTerminate();
        volkFinalize();
    }
};
struct Report {
    bool passed{}, validation{}, glyphs{}, clipboard{}, minimized{}, close{}, capture{}, m0{}, sceneSavedReopened{},
        sceneUiPick{}, sceneUiDrag{};
    unsigned resizes{}, swapchains{}, scaleChanges{}, minimizedWaits{};
    uint64_t frames{};
    double cpuMs{}, gpuMs{};
    size_t allocationBytes{};
    PixelChecks pixels;
    SceneReadback scenePixels;
    bool assets{}, assetWorkflow{}, lighting{}, lightingWorkflow{}, m4{}, gizmoWorkflow{}, projectWorkflow{}, m5{},
        playWorkflow{}, m6{}, exportWorkflow{}, m8{}, acceptanceWorkflow{};
    LightingStats lightingStats;
    size_t maxUploadBytes{}, gpuMeshes{}, gpuImages{};
    std::string device, error, clipboardRoundtrip{"not_requested"};
};
void saveReport(const std::filesystem::path& path, const Report& report, const Diagnostics& log) {
    if (path.empty())
        return;
    ensureParent(path);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot write test report");
    out << std::boolalpha << "{\n  \"milestone\": "
        << jsonString(report.m8         ? "M8"
                      : report.m6       ? "M6"
                      : report.m5       ? "M5"
                      : report.m4       ? "M4"
                      : report.m0       ? "M0"
                      : report.lighting ? "M3"
                      : report.assets   ? "M2"
                                        : "M1")
        << ",\n  \"passed\": " << report.passed << ",\n  \"device\": " << jsonString(report.device)
        << ",\n  \"validation_enabled\": " << report.validation
        << ",\n  \"synchronization_validation\": " << report.validation
        << ",\n  \"validation_errors\": " << log.validationErrors.load()
        << ",\n  \"validation_warnings\": " << log.validationWarnings.load()
        << ",\n  \"loader_warnings\": " << log.loaderWarnings.load() << ",\n  \"rendered_frames\": " << report.frames
        << ",\n  \"framebuffer_events\": " << report.resizes << ",\n  \"swapchain_creations\": " << report.swapchains
        << ",\n  \"minimized_without_rendering\": " << report.minimized
        << ",\n  \"minimized_event_waits\": " << report.minimizedWaits
        << ",\n  \"native_close_callback\": " << report.close << ",\n  \"ukrainian_glyphs\": " << report.glyphs
        << ",\n  \"clipboard_backend_connected\": " << report.clipboard
        << ",\n  \"clipboard_roundtrip\": " << jsonString(report.clipboardRoundtrip)
        << ",\n  \"ui_scale_changes\": " << report.scaleChanges << ",\n  \"cpu_frame_ms_last\": " << report.cpuMs
        << ",\n  \"gpu_frame_ms_last\": " << report.gpuMs << ",\n  \"vma_allocation_bytes\": " << report.allocationBytes
        << ",\n  \"capture_saved\": " << report.capture
        << ",\n  \"scene_saved_reopened\": " << report.sceneSavedReopened
        << ",\n  \"scene_ui_selection\": " << report.sceneUiPick
        << ",\n  \"scene_ui_drag_undo_redo\": " << report.sceneUiDrag
        << ",\n  \"asset_workflow\": " << report.assetWorkflow
        << ",\n  \"lighting_workflow\": " << report.lightingWorkflow
        << ",\n  \"gizmo_workflow\": " << report.gizmoWorkflow
        << ",\n  \"project_workflow\": " << report.projectWorkflow << ",\n  \"play_workflow\": " << report.playWorkflow
        << ",\n  \"export_workflow\": " << report.exportWorkflow
        << ",\n  \"acceptance_workflow\": " << report.acceptanceWorkflow
        << ",\n  \"lighting\": {\"lights\": " << report.lightingStats.lights
        << ", \"shadow_faces\": " << report.lightingStats.shadowFaces
        << ", \"cache_hits\": " << report.lightingStats.shadowCacheHits
        << ", \"batches\": " << report.lightingStats.batches
        << ", \"shadow_bytes\": " << report.lightingStats.shadowBytes
        << ", \"tile_overflows\": " << report.lightingStats.tileOverflows << "}"
        << ",\n  \"max_upload_bytes_per_frame\": " << report.maxUploadBytes
        << ",\n  \"gpu_shared_meshes\": " << report.gpuMeshes << ",\n  \"gpu_shared_images\": " << report.gpuImages
        << ",\n  \"scene_gpu\": {\"geometry_pixels\": " << report.scenePixels.geometryPixels
        << ", \"depth_samples\": " << report.scenePixels.depthSamples
        << ", \"color_samples\": " << report.scenePixels.colorSamples
        << ", \"finite_depth\": " << report.scenePixels.finiteDepth
        << ", \"matches_cpu\": " << report.scenePixels.matchesCpu << "}"
        << ",\n  \"pixels\": {\"y_up\": " << report.pixels.yUp << ", \"front_face\": " << report.pixels.frontFace
        << ", \"back_face_culled\": " << report.pixels.backFaceCulled
        << ", \"depth_occlusion\": " << report.pixels.depthOcclusion
        << ", \"clear_depth\": " << report.pixels.clearDepth << "},\n  \"error\": " << jsonString(report.error)
        << "\n}\n";
}
int run(const Options& options) {
    const auto directory = executableDirectory();
    const auto reportPath = !options.report.empty() ? options.report
                            : !options.benchmark.empty()
                                ? directory / "test-results" / "editor-benchmark-main.json"
                            : options.automated() ? directory / "test-results" / "run.json"
                                                  : std::filesystem::path{};
    auto logPath = reportPath.empty() ? directory / "logs" / "ProtoEditor.log" : reportPath;
    if (!reportPath.empty())
        logPath.replace_extension(".log");
    Diagnostics log(logPath);
    Report report;
    report.m0 = options.m0;
    report.m5 = options.playSmoke;
    report.m6 = options.exportSmoke;
    report.m8 = !options.acceptanceSmoke.empty();
    report.m4 = options.gizmoSmoke || !options.project.empty() || !options.projectSmoke.empty();
    report.lighting = !options.m0 && !options.studio && !options.sceneSmoke && options.assetSmoke.empty();
    // Benchmark runs omit the automatic classic smoke capture.  An explicit
    // --capture is allowed after the timed window and is therefore outside
    // the measured samples.
    const auto capturePath = !options.capture.empty() ? options.capture
                             : !options.benchmark.empty() ? std::filesystem::path{}
                             : options.automated()         ? directory / "test-results" / "editor.png"
                                                           : std::filesystem::path{};
    try {
#ifdef PROTO_DEBUG
        const auto validationDirectory = directory / "validation";
        if (!std::filesystem::exists(validationDirectory / "VkLayer_khronos_validation.json"))
            throw std::runtime_error("Debug validation files missing; rebuild after scripts/Setup-Validation.ps1");
        setVulkanValidationPath(validationDirectory);
        constexpr bool validation = true;
#else
        constexpr bool validation = false;
#endif
        {
            ComApartment com;
            Window window(log);
            VulkanRenderer renderer(log);
            renderer.initialize(window.handle, directory / "shaders", validation);
            Editor editor(window.handle, renderer, directory / "settings" / "editor.ini", options.automated(),
                          options.m0);
            if (!options.benchmark.empty())
                editor.startBenchmark(options.benchmark, options.benchmarkSeconds, options.benchmarkWarmupSeconds,
                                      options.benchmarkInteractive);
            if (!options.scene.empty())
                editor.openScene(options.scene);
            if (!options.project.empty())
                editor.openProject(options.project);
            if (options.gizmoSmoke)
                editor.startGizmoSmoke();
            if (options.playSmoke)
                editor.startPlaySmoke(reportPath.parent_path());
            if (options.exportSmoke)
                editor.startExportSmoke(reportPath.parent_path());
            if (!options.acceptanceSmoke.empty())
                editor.startAcceptanceSmoke(reportPath.parent_path(), options.acceptanceSmoke);
            if (!options.projectSmoke.empty())
                editor.startProjectSmoke(reportPath.parent_path(), options.projectSmoke);
            if (options.sceneCamera)
                editor.viewSceneCamera(true);
            editor.studioLighting(options.studio || options.sceneSmoke);
            if (!options.assetSmoke.empty())
                editor.startAssetSmoke(options.assetSmoke, reportPath.parent_path() /
                                                               (reportPath.stem().wstring() + L"-project") /
                                                               L"Material Lab.scene.json");
            if (!options.lightingSmoke.empty())
                editor.startLightingSmoke(options.lightingSmoke, reportPath.parent_path() /
                                                                     (reportPath.stem().wstring() + L"-project") /
                                                                     L"Lighting Lab.scene.json");
            report.lighting = !options.m0 && !options.studio && !options.sceneSmoke && options.assetSmoke.empty();
            report.assets = !options.assetSmoke.empty() || !editor.document().scene.modelSources.empty();
            report.device = renderer.deviceName();
            report.validation = renderer.validationEnabled();
            report.glyphs = editor.ukrainianGlyphs();
            report.clipboard = editor.clipboardConnected();
            if (options.smoke) {
                report.clipboardRoundtrip = probeClipboard(window.handle);
                log.write("TEST", "Clipboard UTF-8 roundtrip: " + report.clipboardRoundtrip);
                if (report.clipboardRoundtrip.starts_with("failed"))
                    throw std::runtime_error("Clipboard smoke test failed");
            }
            VkExtent2D viewportSize{720, 450};
            int stage{};
            bool closePosted{}, scaledCaptured{};
            Clock::time_point minimizedAt{};
            uint64_t beforeMinimize{};
            float uiScale = options.uiScale;
            double cpuMs{};
            CpuMetric uiMetric{"editor.ui"}, submitMetric{"renderer.submit_present"};
            const auto started = Clock::now();
            while (true) {
                editor.pollPlay();
                if (editor.playerActive())
                    glfwWaitEventsTimeout(.05);
                glfwPollEvents();
                if (glfwWindowShouldClose(window.handle)) {
                    if (options.automated() || options.m0 || editor.closeApproved())
                        break;
                    glfwSetWindowShouldClose(window.handle, GLFW_FALSE);
                    editor.requestClose();
                }
                if (editor.closeApproved())
                    break;
                const auto frames = renderer.renderedFrames();
                if (options.sceneSmoke)
                    editor.sceneSmokeStep(frames, reportPath.parent_path() / "m1-demo.scene.json");
                if (!options.assetSmoke.empty())
                    editor.assetSmokeStep(frames);
                if (!options.lightingSmoke.empty())
                    editor.lightingSmokeStep(frames);
                if (options.gizmoSmoke)
                    editor.gizmoSmokeStep(frames);
                if (options.playSmoke)
                    editor.playSmokeStep(frames);
                if (options.exportSmoke)
                    editor.exportSmokeStep(frames);
                if (!options.acceptanceSmoke.empty())
                    editor.acceptanceSmokeStep(frames);
                if (!options.projectSmoke.empty())
                    editor.projectSmokeStep(frames);
                const auto benchmarkTimeoutMs =
                    30000.0 + (options.benchmarkSeconds + options.benchmarkWarmupSeconds) * 1000.0;
                if (options.automated() &&
                    milliseconds(started) > (!options.benchmark.empty()
                                                   ? benchmarkTimeoutMs
                                                   : !options.acceptanceSmoke.empty() ? 400000
                                                   : options.exportSmoke             ? 150000
                                                   : options.playSmoke                ? 210000
                                                   : options.lightingSmoke.empty()   ? 60000
                                                                                       : 180000))
                    throw std::runtime_error("Automated window test timed out");
                if (options.smoke) {
                    if (stage == 0 && frames >= 6) {
                        glfwSetWindowSize(window.handle, 1000, 700);
                        stage = 1;
                    }
                    if (stage == 1 && frames >= 18) {
                        glfwSetWindowSize(window.handle, 1440, 900);
                        stage = 2;
                    }
                    if (stage == 2 && frames >= 30) {
                        glfwIconifyWindow(window.handle);
                        minimizedAt = Clock::now();
                        beforeMinimize = frames;
                        stage = 3;
                        log.write("TEST", "Minimize: rendering pauses, event loop waits");
                    }
                    if (stage == 3 && milliseconds(minimizedAt) > 250) {
                        report.minimized = report.minimizedWaits > 0 && renderer.renderedFrames() == beforeMinimize;
                        glfwRestoreWindow(window.handle);
                        stage = 4;
                    }
                    if (stage == 4 && frames >= 42) {
                        uiScale = 1.5f;
                        stage = 5;
                    }
                    if (stage == 5 && frames >= 58) {
                        uiScale = 1;
                        stage = 6;
                    }
                }
                if (options.frames && frames >= options.frames && !closePosted &&
                    (!options.playSmoke || (editor.playSmokePassed() && report.capture)) &&
                    (!options.exportSmoke || (editor.exportSmokePassed() && report.capture)) &&
                    (options.acceptanceSmoke.empty() || (editor.acceptanceSmokePassed() && report.capture)) &&
                    (options.assetSmoke.empty() || (editor.assetSmokePassed() && report.capture)) &&
                    (options.lightingSmoke.empty() || (editor.lightingSmokePassed() && report.capture)) &&
                    (!options.gizmoSmoke || (editor.gizmoSmokePassed() && report.capture)) &&
                    (options.projectSmoke.empty() || (editor.projectSmokePassed() && report.capture))) {
                    if (!PostMessageW(glfwGetWin32Window(window.handle), WM_CLOSE, 0, 0))
                        throw std::runtime_error("Native close message failed");
                    closePosted = true;
                    continue;
                }
                if (!options.benchmark.empty() && editor.benchmarkFinished() && !closePosted &&
                    (capturePath.empty() || report.capture)) {
                    if (!PostMessageW(glfwGetWin32Window(window.handle), WM_CLOSE, 0, 0))
                        throw std::runtime_error("Native close message failed");
                    closePosted = true;
                    continue;
                }
                auto frameStart = Clock::time_point{};
                if (!options.benchmark.empty()) {
                    // Benchmark timing starts before the renderer fence wait;
                    // the Editor hook also snapshots the warmup GPU boundary
                    // before this frame is submitted.
                    frameStart = Clock::now();
                    editor.benchmarkBeginFrame();
                }
                if (!renderer.prepare(viewportSize)) {
                    if (stage == 3)
                        ++report.minimizedWaits;
                    glfwWaitEventsTimeout(options.automated() ? 0.02 : 0.15);
                    continue;
                }
                if (options.benchmark.empty())
                    frameStart = Clock::now();
                {
                    CpuMarker marker(uiMetric);
                    viewportSize = editor.build(cpuMs, uiScale);
                }
                std::filesystem::path capture;
                if (!capturePath.empty() && !report.capture && frames >= (options.frames ? options.frames - 3 : 10) &&
                    (options.benchmark.empty() || editor.benchmarkFinished()) &&
                    (!options.playSmoke || editor.playSmokePassed()) &&
                    (!options.exportSmoke || editor.exportSmokePassed()) &&
                    (options.acceptanceSmoke.empty() || editor.acceptanceSmokePassed()) &&
                    (options.assetSmoke.empty() || editor.assetSmokePassed()) &&
                    (options.lightingSmoke.empty() || editor.lightingSmokePassed()) &&
                    (!options.gizmoSmoke || editor.gizmoSmokePassed()) &&
                    (options.projectSmoke.empty() || editor.projectSmokePassed()))
                    capture = capturePath;
                if (!options.lightingSmoke.empty() && !editor.lightingSmokePassed())
                    capture = editor.lightingSmokeCapture();
                const bool scaled = options.smoke && frames >= 52 && !scaledCaptured;
                if (scaled) {
                    capture = capturePath;
                    capture.replace_filename(capturePath.stem().wstring() + L"-150.png");
                }
                if (!capture.empty())
                    editor.prepareCapture();
                const auto benchmarkViewportExtent = renderer.viewportExtent();
                const auto benchmarkSwapExtent = renderer.swapExtent();
                bool rendered{};
                {
                    CpuMarker marker(submitMetric);
                    rendered =
                        renderer.draw([&](VkCommandBuffer cmd) { editor.record(cmd); }, capture, editor.renderView());
                }
                report.maxUploadBytes = std::max(report.maxUploadBytes, renderer.assetRenderer()->uploadedBytes());
                if (rendered) {
                    if (!capture.empty()) {
                        if (scaled)
                            scaledCaptured = true;
                        else if (options.lightingSmoke.empty() || capture == capturePath)
                            report.capture = true;
                        if (!options.lightingSmoke.empty() && !editor.lightingSmokePassed())
                            editor.lightingSmokeCaptured();
                        if (options.benchmark.empty() &&
                            !(options.m0 ? renderer.pixelChecks().passed() : renderer.sceneReadback().passed()))
                            throw std::runtime_error("GPU pixel/depth readback failed; inspect viewport PNG and log");
                    }
                }
                cpuMs = milliseconds(frameStart);
                if (!options.benchmark.empty())
                    editor.benchmarkFrame(cpuMs, uiMetric.lastMs, submitMetric.lastMs, renderer.frameStats().frameWaitMs,
                                          benchmarkViewportExtent, benchmarkSwapExtent, rendered);
            }
            renderer.waitIdle();
            if (!options.benchmark.empty())
                editor.finishBenchmark(renderer.renderedFrames(), renderer.sceneRenderFrames());
            report.frames = renderer.renderedFrames();
            report.resizes = window.framebufferEvents;
            report.swapchains = renderer.swapchainRebuilds();
            report.close = window.closeEvents > 0;
            report.scaleChanges = editor.scaleChanges();
            report.cpuMs = cpuMs;
            report.gpuMs = renderer.gpuMilliseconds();
            report.allocationBytes = renderer.allocatedBytes();
            report.pixels = renderer.pixelChecks();
            report.scenePixels = renderer.sceneReadback();
            report.sceneSavedReopened = editor.sceneSmokePassed();
            report.sceneUiPick = editor.selectionProbePassed();
            report.sceneUiDrag = editor.gestureProbePassed();
            report.assetWorkflow = editor.assetSmokePassed();
            report.gpuMeshes = renderer.assetRenderer()->meshCount();
            report.gpuImages = renderer.assetRenderer()->imageCount();
            report.lightingWorkflow = editor.lightingSmokePassed();
            report.gizmoWorkflow = editor.gizmoSmokePassed();
            report.projectWorkflow = editor.projectSmokePassed();
            report.playWorkflow = editor.playSmokePassed();
            report.exportWorkflow = editor.exportSmokePassed();
            report.acceptanceWorkflow = editor.acceptanceSmokePassed();
            report.lightingStats = renderer.assetRenderer()->lightingStats();
            for (const auto& metric : {uiMetric, submitMetric})
                log.write(
                    "CPU MARKER",
                    std::string(metric.name) + " mean_ms=" +
                        std::to_string(metric.samples ? metric.totalMs / static_cast<double>(metric.samples) : 0));
            report.passed = report.glyphs && report.clipboard &&
                            (!options.benchmark.empty() || capturePath.empty() ||
                             (report.capture && (options.m0 ? report.pixels.passed() : report.scenePixels.passed())));
            if (options.sceneSmoke)
                report.passed &= report.sceneSavedReopened;
            if (!options.assetSmoke.empty())
                report.passed &= report.assetWorkflow && report.scenePixels.colorSamples == 9 &&
                                 report.maxUploadBytes <= 4 * 1024 * 1024;
            if (!options.lightingSmoke.empty())
                report.passed &= report.lightingWorkflow && report.maxUploadBytes <= 4 * 1024 * 1024;
            if (options.gizmoSmoke)
                report.passed &= report.gizmoWorkflow;
            if (options.playSmoke)
                report.passed &= editor.playSmokePassed();
            if (options.exportSmoke)
                report.passed &= editor.exportSmokePassed();
            if (!options.acceptanceSmoke.empty())
                report.passed &= report.acceptanceWorkflow;
            if (!options.projectSmoke.empty())
                report.passed &= report.projectWorkflow;
            if (options.automated())
                report.passed &= report.frames >= options.frames && report.close;
            if (options.smoke)
                report.passed &= report.minimized && report.resizes >= 2 && report.swapchains >= 3 &&
                                 report.scaleChanges >= 3 && scaledCaptured;
        }
        // Includes validation errors emitted during backend/device destruction.
        report.passed &= log.validationErrors == 0;
        if (!report.passed)
            report.error = "Acceptance check failed; see individual report fields";
    } catch (const std::exception& error) {
        report.passed = false;
        report.error = error.what();
        log.write("ERROR", report.error);
        if (!options.automated())
            MessageBoxW(
                nullptr,
                L"Proto Engine не вдалося запустити. Подробиці у файлі logs/ProtoEditor.log поруч із програмою.",
                L"Proto Engine", MB_OK | MB_ICONERROR);
    }
    log.write(report.passed ? "INFO" : "ERROR", report.passed ? "Session completed successfully" : "Session failed");
    saveReport(reportPath, report, log);
    return report.passed ? 0 : 1;
}
} // namespace
} // namespace proto
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count{};
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        return 1;
    try {
        const auto options = proto::parse(count, args);
        LocalFree(args);
        args = nullptr;
        return proto::run(options);
    } catch (const std::exception& error) {
        if (args)
            LocalFree(args);
        OutputDebugStringA(error.what());
        return 1;
    }
}
