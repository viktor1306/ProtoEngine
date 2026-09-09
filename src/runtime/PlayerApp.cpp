#include <Proto/Player.hpp>
#include <Proto/Build.hpp>
#include "runtime/BehaviorRuntime.hpp"
#include "runtime/RuntimeSnapshot.hpp"
#include "runtime/RuntimePackage.hpp"
#include "runtime/PlayerGraphics.hpp"
#include "runtime/PlayerGraphicsUi.hpp"
#include "runtime/SceneViewBuilder.hpp"
#include "runtime/PlayerPresenter.hpp"
#include "renderer/VulkanRenderer.hpp"
#include "renderer/AssetRenderer.hpp"
#include "assets/AssetIO.hpp"
#include "scene/SceneIO.hpp"
#include <windows.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

namespace proto::sdk {
namespace {
struct Options {
    bool describe{}, headless{}, testInput{}, validatePackage{}, graphicsSmoke{};
    uint64_t frames{};
    double fixedDt{};
    std::filesystem::path snapshot, package, capture, report, validationDir;
    std::wstring stopEvent, readyEvent;
};
Options parse(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        const auto value = [&]() {
            if (++i >= argc)
                throw std::runtime_error("Missing Player argument value");
            return argv[i];
        };
        if (arg == L"--describe-behaviors")
            o.describe = true;
        else if (arg == L"--snapshot")
            o.snapshot = value();
        else if (arg == L"--package")
            o.package = value();
        else if (arg == L"--validate-package")
            o.validatePackage = true;
        else if (arg == L"--graphics-smoke")
            o.graphicsSmoke = true;
        else if (arg == L"--validation-dir")
            o.validationDir = value();
        else if (arg == L"--capture")
            o.capture = value();
        else if (arg == L"--report")
            o.report = value();
        else if (arg == L"--stop-event")
            o.stopEvent = value();
        else if (arg == L"--ready-event")
            o.readyEvent = value();
        else if (arg == L"--headless")
            o.headless = true;
        else if (arg == L"--test-input")
            o.testInput = true;
        else if (arg == L"--frames") {
            const std::wstring s(value());
            size_t n{};
            o.frames = std::stoull(s, &n);
            if (n != s.size() || o.frames < 1 || o.frames > 1000000)
                throw std::runtime_error("Invalid Player frame count");
        } else if (arg == L"--fixed-dt") {
            const std::wstring s(value());
            size_t n{};
            o.fixedDt = std::stod(s, &n);
            if (n != s.size() || !(o.fixedDt > 0 && o.fixedDt <= .25))
                throw std::runtime_error("Invalid Player fixed dt");
        } else
            throw std::runtime_error("Unknown Player argument: " + utf8(arg));
    }
    if (o.describe && argc != 2)
        throw std::runtime_error("Describe accepts no runtime options");
    if (!o.snapshot.empty() && !o.package.empty())
        throw std::runtime_error("Player accepts either --snapshot or --package, not both");
    if (o.validatePackage && !o.snapshot.empty())
        throw std::runtime_error("--validate-package cannot validate an M5 snapshot");
    if (o.validatePackage && o.headless)
        throw std::runtime_error("--validate-package cannot be combined with --headless");
    if (o.headless && !o.frames)
        throw std::runtime_error("Headless diagnostic requires bounded --frames");
    if (o.graphicsSmoke && !o.snapshot.empty())
        throw std::runtime_error("--graphics-smoke requires a packaged Player");
    if (o.graphicsSmoke && o.validatePackage)
        throw std::runtime_error("--graphics-smoke cannot validate without a window");
    if (o.graphicsSmoke && !o.frames)
        o.frames = 120;
    return o;
}
struct Event {
    HANDLE handle{};
    Event(const std::wstring& name, DWORD access) {
        if (!name.empty()) {
            handle = OpenEventW(access, FALSE, name.c_str());
            if (!handle)
                throw std::runtime_error("Cannot open Player session event");
        }
    }
    ~Event() {
        if (handle)
            CloseHandle(handle);
    }
    bool signaled() const { return handle && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }
    void signal() const {
        if (handle && !SetEvent(handle))
            throw std::runtime_error("Cannot signal Player ready");
    }
};
struct Window {
    GLFWwindow* handle{};
    explicit Window(std::string title = "Proto Player · M5") {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        vkCheck(volkInitialize(), "Player Vulkan loader");
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
        if (!glfwInit()) {
            volkFinalize();
            throw std::runtime_error("Player GLFW initialization failed");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        handle = glfwCreateWindow(1100, 720, title.c_str(), nullptr, nullptr);
        if (!handle) {
            glfwTerminate();
            volkFinalize();
            throw std::runtime_error("Cannot create Player window");
        }
    }
    ~Window() {
        if (handle)
            glfwDestroyWindow(handle);
        glfwTerminate();
        volkFinalize();
    }
};
RuntimeInput keyboard(GLFWwindow* window, const RuntimeInput& before, uint64_t frame, bool diagnostic) {
    static constexpr int keys[]{GLFW_KEY_W,     GLFW_KEY_A,          GLFW_KEY_S,     GLFW_KEY_D,    GLFW_KEY_Q,
                                GLFW_KEY_E,     GLFW_KEY_UP,         GLFW_KEY_DOWN,  GLFW_KEY_LEFT, GLFW_KEY_RIGHT,
                                GLFW_KEY_SPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_ESCAPE};
    static_assert(std::size(keys) == size_t(Key::Count));
    RuntimeInput input;
    if (diagnostic) {
        input.down[size_t(Key::Space)] = frame == 30;
        input.down[size_t(Key::D)] = frame >= 30 && frame < 60;
    } else if (window && glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
        for (size_t i = 0; i < std::size(keys); ++i)
            input.down[i] = glfwGetKey(window, keys[i]) == GLFW_PRESS;
    }
    for (size_t i = 0; i < input.down.size(); ++i)
        input.pressed[i] = input.down[i] && !before.down[i];
    return input;
}
} // namespace
int RunPlayer(int argc, wchar_t** argv, Registration registerBehaviors) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::filesystem::path reportPath;
    bool showPackageStartupMessage{};
    try {
        const auto o = parse(argc, argv);
        reportPath = o.report;
        showPackageStartupMessage = !o.describe && o.snapshot.empty() && !o.validatePackage && !o.headless && !o.graphicsSmoke &&
                                    !o.frames && o.report.empty();
        BehaviorRegistry registry;
        registerBehaviors(registry);
        const auto schema = describeRegistry(registry, buildId);
        if (o.describe) {
            std::cout << encodeBehaviorSchema(schema) << std::flush;
            return 0;
        }
        const auto directory = executableDirectory();
        const auto packageInput = o.package.empty() ? directory / "Data" / "runtime.json" : o.package;
        std::optional<RuntimePackage> package;
        std::filesystem::path graphicsPath, userGraphicsPath;
        bool packageMode = !o.validatePackage && o.snapshot.empty();
        if (!o.snapshot.empty())
            packageMode = false;
        if (o.validatePackage || packageMode) {
            package.emplace(loadRuntimePackage(packageInput, buildId));
            graphicsPath = package->graphicsPath;
            userGraphicsPath = package->userGraphicsPath;
            const auto defaults = decodePlayerGraphics(readDocument(graphicsPath));
            if (o.validatePackage) {
                validateSceneBehaviors(package->scene, schema);
                std::cout << "{\"passed\":true,\"package\":" << jsonString(utf8(packageInput.wstring()))
                          << ",\"profile\":" << jsonString(defaults.profile) << "}\n";
                return 0;
            }
        }
        Event stop(o.stopEvent, SYNCHRONIZE), ready(o.readyEvent, EVENT_MODIFY_STATE);
        Scene scene;
        if (o.snapshot.empty())
            scene = std::move(package->scene);
        else
            scene = loadRuntimeSnapshot(o.snapshot, buildId);
        auto logFile = reportPath.empty() ? o.snapshot.parent_path() / "Player.log" : reportPath;
        if (packageMode && reportPath.empty())
            logFile = package->root / "Player.log";
        logFile.replace_extension(".log");
        Diagnostics log(logFile);
        PlayerGraphics graphics;
        if (packageMode) {
            graphics = decodePlayerGraphics(readDocument(graphicsPath));
            if (!userGraphicsPath.empty() && std::filesystem::exists(userGraphicsPath)) {
                try {
                    graphics = decodePlayerGraphics(readDocument(userGraphicsPath));
                } catch (const std::exception& error) {
                    log.write("WARNING", std::string("Ignoring invalid graphics.user.json: ") + error.what());
                }
            }
        }
        BehaviorRuntime runtime(scene, registry, [&](std::string_view message) { log.write("BEHAVIOR", message); });
        uint64_t frames{};
        double gpuMs{}, maxUpdateMs{};
        size_t gpuBytes{};
        uint64_t shadowFaces{}, shadowHits{};
        uint64_t shadowPoolBytes{}, sunShadowPoolBytes{}, pointShadowPoolBytes{};
        size_t mipUploadBytes{};
        bool graphicsSmokeCancel{}, graphicsSmokeFailure{}, graphicsSmokeRecovery{};
        uint32_t graphicsSmokeFailureCount{}, graphicsSmokeAttempts{};
        bool validationEnabled{};
        VkExtent2D finalSwapExtent{}, finalViewportExtent{};
        VkPresentModeKHR finalPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        std::string initialScene = encodeScene(scene);
        RuntimeInput input;
        runtime.start();
        if (o.headless) {
            ready.signal();
            while (frames < o.frames && !stop.signaled()) {
                input = keyboard(nullptr, input, frames, o.testInput);
                runtime.update(o.fixedDt > 0 ? o.fixedDt : 1.0 / 60, input);
                maxUpdateMs = std::max(maxUpdateMs, runtime.metrics().updateMs);
                ++frames;
            }
        } else {
            std::filesystem::path validationPath;
            bool validation{};
            if (!o.validationDir.empty()) {
                validationPath = o.validationDir;
                validation = true;
            }
#ifdef PROTO_DEBUG
            else if (!o.snapshot.empty()) {
                // M5 snapshot diagnostics retain their existing Debug
                // validation contract. Packaged Player runs stay portable and
                // opt in with --validation-dir when GPU QA needs callbacks.
                validationPath = directory / "validation";
                validation = true;
            }
#endif
            if (validation) {
                if (!std::filesystem::exists(validationPath / "VkLayer_khronos_validation.json"))
                    throw std::runtime_error("Player Vulkan validation files missing");
                setVulkanValidationPath(validationPath);
            }
            validationEnabled = validation;
            const auto title = packageMode ? package->name + " · F1: графіка" : "Proto Player · M5";
            Window window(title);
            VulkanRenderer renderer(log);
            const auto shaderDirectory = packageMode ? package->shaderDirectory : directory / "shaders";
            renderer.initialize(window.handle, shaderDirectory, validation);
            if (packageMode)
                renderer.setVSync(graphics.vSync);
            PlayerPresenter presenter(renderer, shaderDirectory);
            std::unique_ptr<PlayerGraphicsUi> graphicsUi;
            if (packageMode)
                graphicsUi = std::make_unique<PlayerGraphicsUi>(window.handle, userGraphicsPath, graphics, log);
            std::optional<PlayerGraphics> smokeTarget;
            bool graphicsSmokeStarted{};
            enum class SmokeFailureStage { None, BeforeUpload, AfterUpload, AfterLighting };
            const uint32_t smokeFailureAttempts = std::max(4u, renderer.context().imageCount + 1u);
            uint32_t smokeAttempt{};
            SmokeFailureStage smokeDeferredFailure = SmokeFailureStage::None;
            if (o.graphicsSmoke) {
                graphicsSmokeCancel = graphicsUi->smokePresetCancel();
                smokeTarget = graphics;
                smokeTarget->textureTopMipDrop = smokeTarget->textureTopMipDrop ? 0 : 1;
                smokeTarget->profile = "Custom";
            }
            struct IdleBeforePresenterDestruction {
                VulkanRenderer& renderer;
                ~IdleBeforePresenterDestruction() {
                    try {
                        renderer.waitIdle();
                    } catch (...) {
                    }
                }
            } idleGuard{renderer};
            RenderView view;
            auto previous = Clock::now();
            bool announced{};
            bool f1Before{};
            std::optional<PlayerGraphics> pendingGraphics;
            uint32_t pendingRenderedFrames{};
            PlayerGraphics previousGraphics = graphics;
            while (!stop.signaled() && !glfwWindowShouldClose(window.handle) && (!o.frames || frames < o.frames)) {
                glfwPollEvents();
                if (graphicsUi) {
                    const bool f1 = glfwGetKey(window.handle, GLFW_KEY_F1) == GLFW_PRESS;
                    if (f1 && !f1Before)
                        graphicsUi->toggle();
                    f1Before = f1;
                    PlayerGraphics applied;
                    if (graphicsUi->consumeApply(applied)) {
                        if (pendingGraphics) {
                            // Keep the last committed rollback baseline while
                            // both in-flight targets finish the current
                            // candidate. A second Apply from a reopened panel
                            // cannot replace that transactional baseline.
                            graphicsUi->setCurrent(graphics);
                            log.write("WARNING", "Graphics apply ignored while a previous apply is pending");
                        } else {
                            previousGraphics = graphics;
                            graphics = std::move(applied);
                            pendingGraphics = graphics;
                            pendingRenderedFrames = 0;
                            smokeDeferredFailure = SmokeFailureStage::None;
                            AssetRenderer::setAllocationFailureForTesting(false);
                            if (o.graphicsSmoke && graphicsSmokeStarted) {
                                ++graphicsSmokeAttempts;
                                switch (smokeAttempt++) {
                                case 0:
                                    AssetRenderer::setAllocationFailureForTesting(true);
                                    break;
                                case 1:
                                    AssetRenderer::setAllocationFailureAfterUploadForTesting(true);
                                    break;
                                case 2:
                                    smokeDeferredFailure = SmokeFailureStage::BeforeUpload;
                                    break;
                                case 3:
                                    smokeDeferredFailure = SmokeFailureStage::AfterLighting;
                                    break;
                            default:
                                if (smokeAttempt <= smokeFailureAttempts)
                                    AssetRenderer::setAllocationFailureAfterUploadForTesting(true);
                                break;
                                }
                            }
                            renderer.assetRenderer()->clearError();
                            renderer.setVSync(graphics.vSync);
                            log.write("PLAYER", "Graphics settings applied");
                        }
                    }
                }
                int width{}, height{};
                glfwGetFramebufferSize(window.handle, &width, &height);
                const auto scaledExtent = VkExtent2D{
                    static_cast<uint32_t>(std::max(1.0, std::floor(double(std::max(width, 1)) * graphics.renderScale))),
                    static_cast<uint32_t>(std::max(1.0, std::floor(double(std::max(height, 1)) * graphics.renderScale)))};
                bool prepared{};
                try {
                    prepared = width && height &&
                               renderer.prepare(packageMode
                                                    ? scaledExtent
                                                    : VkExtent2D{uint32_t(std::max(width, 1)),
                                                                 uint32_t(std::max(height, 1))});
                } catch (const std::exception& error) {
                    if (renderer.fatal())
                        throw std::runtime_error(renderer.fatalReason());
                    if (!pendingGraphics)
                        throw;
                    graphics = previousGraphics;
                    renderer.setVSync(graphics.vSync);
                    graphicsUi->setCurrent(graphics);
                    renderer.assetRenderer()->clearError();
                    pendingGraphics.reset();
                    pendingRenderedFrames = 0;
                    AssetRenderer::setAllocationFailureForTesting(false);
                    if (o.graphicsSmoke && graphicsSmokeStarted) {
                        graphicsSmokeFailure = true;
                        ++graphicsSmokeFailureCount;
                        smokeDeferredFailure = SmokeFailureStage::None;
                        if (smokeAttempt <= smokeFailureAttempts)
                            graphicsUi->smokeApply(*smokeTarget);
                    }
                    log.write("WARNING", std::string("Graphics apply rolled back before draw: ") + error.what());
                    continue;
                }
                if (!prepared) {
                    glfwWaitEventsTimeout(.05);
                    previous = Clock::now();
                    continue;
                }
                const auto now = Clock::now();
                const double dt = o.fixedDt > 0
                                      ? o.fixedDt
                                      : std::clamp(std::chrono::duration<double>(now - previous).count(), 0.0, .1);
                previous = now;
                input = keyboard(window.handle, input, frames, o.testInput);
                if (input.pressed[size_t(Key::Escape)])
                    break;
                runtime.update(dt, input);
                maxUpdateMs = std::max(maxUpdateMs, runtime.metrics().updateMs);
                buildRuntimeView(scene, renderer.viewportExtent(), view);
                if (packageMode)
                    applyPlayerGraphics(view, renderer.viewportExtent(), graphics);
                const auto capture =
                    !o.capture.empty() && o.frames && frames + 1 == o.frames ? o.capture : std::filesystem::path{};
                bool rendered{};
                try {
                    rendered = renderer.draw([&](VkCommandBuffer cmd) { presenter.record(renderer, cmd); }, capture, &view);
                } catch (const std::exception& error) {
                    if (renderer.fatal())
                        throw std::runtime_error(renderer.fatalReason());
                    if (pendingGraphics) {
                        graphics = previousGraphics;
                        renderer.setVSync(graphics.vSync);
                        graphicsUi->setCurrent(graphics);
                        renderer.assetRenderer()->clearError();
                        pendingGraphics.reset();
                        pendingRenderedFrames = 0;
                        AssetRenderer::setAllocationFailureForTesting(false);
                        if (o.graphicsSmoke && graphicsSmokeStarted) {
                            graphicsSmokeFailure = true;
                            ++graphicsSmokeFailureCount;
                            smokeDeferredFailure = SmokeFailureStage::None;
                            if (smokeAttempt <= smokeFailureAttempts)
                                graphicsUi->smokeApply(*smokeTarget);
                        }
                        log.write("WARNING", std::string("Graphics apply rolled back: ") + error.what());
                        continue;
                    }
                    throw;
                }
                if (pendingGraphics && !renderer.assetRenderer()->error().empty()) {
                    graphics = previousGraphics;
                    renderer.setVSync(graphics.vSync);
                    graphicsUi->setCurrent(graphics);
                    log.write("WARNING", std::string("Graphics apply rolled back: ") +
                                             renderer.assetRenderer()->error());
                    renderer.assetRenderer()->clearError();
                    pendingGraphics.reset();
                    pendingRenderedFrames = 0;
                    AssetRenderer::setAllocationFailureForTesting(false);
                    if (o.graphicsSmoke && graphicsSmokeStarted) {
                        graphicsSmokeFailure = true;
                        ++graphicsSmokeFailureCount;
                        smokeDeferredFailure = SmokeFailureStage::None;
                        if (smokeAttempt <= smokeFailureAttempts)
                            graphicsUi->smokeApply(*smokeTarget);
                    }
                } else if (rendered && pendingGraphics) {
                    ++pendingRenderedFrames;
                    const bool resourcesReady = renderer.assetRenderer()->pending() == 0;
                    if (pendingRenderedFrames >= VulkanRenderer::framesInFlight && resourcesReady) {
                        if (!graphicsUi->commitApplied(graphics))
                            log.write("WARNING", "Graphics applied in memory but user settings could not be saved");
                        if (o.graphicsSmoke && graphicsSmokeFailure)
                            graphicsSmokeRecovery = true;
                        pendingGraphics.reset();
                        pendingRenderedFrames = 0;
                    }
                }
                if (rendered) {
                    mipUploadBytes = std::max(mipUploadBytes, renderer.assetRenderer()->uploadedBytes());
                    ++frames;
                    if (!announced) {
                        ready.signal();
                        announced = true;
                        log.write("PLAYER", "Ready");
                    }
                    const auto stats = renderer.assetRenderer()->lightingStats();
                    shadowFaces += stats.shadowFaces;
                    shadowHits += stats.shadowCacheHits;
                    shadowPoolBytes = stats.shadowBytes;
                    sunShadowPoolBytes = stats.sunShadowBytes;
                    pointShadowPoolBytes = stats.pointShadowBytes;
                    if (o.graphicsSmoke && pendingGraphics && pendingRenderedFrames == 1 &&
                        smokeDeferredFailure != SmokeFailureStage::None) {
                        if (smokeDeferredFailure == SmokeFailureStage::BeforeUpload)
                            AssetRenderer::setAllocationFailureForTesting(true);
                        else if (smokeDeferredFailure == SmokeFailureStage::AfterUpload)
                            AssetRenderer::setAllocationFailureAfterUploadForTesting(true);
                        else if (smokeDeferredFailure == SmokeFailureStage::AfterLighting)
                            AssetRenderer::setAllocationFailureAfterLightingForTesting(true);
                        smokeDeferredFailure = SmokeFailureStage::None;
                    }
                    if (o.graphicsSmoke && !graphicsSmokeStarted && frames >= 8) {
                        // Let both in-flight targets, shadow pools, material
                        // caches, and mip uploads reach a real rendered state
                        // before the diagnostic mutates the texture policy.
                        graphicsSmokeStarted = true;
                        graphicsUi->smokeApply(*smokeTarget);
                    }
                }
                if (o.graphicsSmoke && graphicsSmokeRecovery &&
                    (o.capture.empty() || !o.frames || frames >= o.frames))
                    break;
            }
            AssetRenderer::setAllocationFailureForTesting(false);
            renderer.waitIdle();
            gpuMs = renderer.gpuMilliseconds();
            gpuBytes = renderer.allocatedBytes();
            finalSwapExtent = renderer.swapExtent();
            finalViewportExtent = renderer.viewportExtent();
            finalPresentMode = renderer.presentMode();
            if (o.graphicsSmoke &&
                (!graphicsSmokeCancel || graphicsSmokeFailureCount < smokeFailureAttempts || !graphicsSmokeRecovery ||
                 graphicsSmokeAttempts < smokeFailureAttempts + 1))
                throw std::runtime_error("Graphics smoke did not complete every rollback/recovery stage");
        }
        runtime.stop();
        if (log.validationErrors != 0)
            throw std::runtime_error("Player Vulkan validation errors");
        if (!reportPath.empty()) {
            const auto metrics = runtime.metrics();
            atomicWrite(reportPath,
                        "{\"passed\":true,\"sdkBuildId\":" + jsonString(buildId) +
                            ",\"frames\":" + std::to_string(frames) + ",\"starts\":" + std::to_string(metrics.starts) +
                            ",\"updates\":" + std::to_string(metrics.updates) +
                            ",\"stops\":" + std::to_string(metrics.stops) +
                            ",\"behavior_update_ms_max\":" + std::to_string(maxUpdateMs) +
                            ",\"gpu_ms_last\":" + std::to_string(gpuMs) + ",\"gpu_bytes\":" + std::to_string(gpuBytes) +
                            ",\"shadow_faces_total\":" + std::to_string(shadowFaces) + ",\"shadow_cache_hits_total\":" +
                            std::to_string(shadowHits) + ",\"validation_errors\":0,\"validation_enabled\":" +
                            (validationEnabled ? "true" : "false") + ",\"graphics\":{\"renderScale\":" +
                            std::to_string(graphics.renderScale) + ",\"vSync\":" +
                            (graphics.vSync ? "true" : "false") + ",\"viewDistance\":" +
                            std::to_string(graphics.viewDistance) + ",\"sunResolution\":" +
                            std::to_string(graphics.sunShadows.resolution) + ",\"sunCascades\":" +
                            std::to_string(graphics.sunShadows.cascades) + ",\"pointResolution\":" +
                            std::to_string(graphics.pointShadows.resolution) + ",\"pointPoolMiB\":" +
                            std::to_string(graphics.pointShadows.poolBudgetMiB) + ",\"swapWidth\":" +
                            std::to_string(finalSwapExtent.width) + ",\"swapHeight\":" +
                            std::to_string(finalSwapExtent.height) + ",\"viewportWidth\":" +
                            std::to_string(finalViewportExtent.width) + ",\"viewportHeight\":" +
                            std::to_string(finalViewportExtent.height) + ",\"presentMode\":" +
                            std::to_string(static_cast<int>(finalPresentMode)) + ",\"mipUploadBytes\":" +
                            std::to_string(mipUploadBytes) + ",\"textureTopMipDrop\":" +
                            std::to_string(graphics.textureTopMipDrop) + ",\"shadowPoolBytes\":" +
                            std::to_string(shadowPoolBytes) + ",\"sunShadowPoolBytes\":" +
                            std::to_string(sunShadowPoolBytes) + ",\"pointShadowPoolBytes\":" +
                            std::to_string(pointShadowPoolBytes) + ",\"graphicsSmokeAttempts\":" +
                            std::to_string(graphicsSmokeAttempts) + ",\"graphicsSmokeFailureCount\":" +
                            std::to_string(graphicsSmokeFailureCount) + ",\"graphicsSmokeCancel\":" +
                            (graphicsSmokeCancel ? "true" : "false") + ",\"graphicsSmokeFailure\":" +
                            (graphicsSmokeFailure ? "true" : "false") + ",\"graphicsSmokeRecovery\":" +
                            (graphicsSmokeRecovery ? "true" : "false") + "},\"initialScene\":" + initialScene +
                            ",\"finalScene\":" + encodeScene(scene) + "}");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[PLAYER ERROR] " << e.what() << std::endl;
        if (showPackageStartupMessage)
            MessageBoxW(nullptr, utf8Path(e.what()).c_str(), L"Proto Player", MB_OK | MB_ICONERROR);
        if (!reportPath.empty())
            try {
                atomicWrite(reportPath, "{\"passed\":false,\"error\":" + jsonString(e.what()) + "}");
            } catch (...) {
            }
        return 1;
    } catch (...) {
        std::cerr << "[PLAYER ERROR] Unknown C++ exception" << std::endl;
        return 2;
    }
}
} // namespace proto::sdk
