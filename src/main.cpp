#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>

#include "core/Timer.hpp"
#include "core/Window.hpp"
#include "core/InputManager.hpp"
#include "gfx/core/VulkanContext.hpp"
#include "gfx/core/Swapchain.hpp"
#include "gfx/rendering/Renderer.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include "gfx/rendering/BindlessSystem.hpp"
#include "gfx/rendering/DebugRenderer.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include "gfx/resources/Texture.hpp"
#include "gfx/resources/Mesh.hpp"
#include "scene/Camera.hpp"
#include "core/Math.hpp"
#include "ui/TextRenderer.hpp"
#include "core/ShaderHotReloader.hpp"
#include "ui/ImGuiManager.hpp"
#include "imgui.h"
#include "world/BlockType.hpp"
#include "world/World.hpp"
#include "world/ChunkManager.hpp"
#include "world/VoxelData.hpp"
#include "world/Raycaster.hpp"
#include "scene/Frustum.hpp"

// ---------------------------------------------------------------------------
// Push constants for the standard (simple) pipeline
// ---------------------------------------------------------------------------
struct PushConstants {
    core::math::Mat4 viewProj;
    core::math::Mat4 lightSpaceMatrix;
    uint32_t objectIndex;
};

// ---------------------------------------------------------------------------
// Push constants for the voxel pipeline (matches voxel.vert layout)
// ---------------------------------------------------------------------------
struct VoxelGlobalPush {
    core::math::Mat4 viewProj;          // 64 bytes
    core::math::Mat4 lightSpaceMatrix;  // 64 bytes
};
static_assert(sizeof(VoxelGlobalPush) == 128, "VoxelGlobalPush size mismatch");

// ---- CPU and RAM Profiling ------------------------------------------------
static ULARGE_INTEGER lastCPU, lastSysCPU, lastUserCPU;
static int numProcessors;
static HANDLE selfHandle;
static bool cpuInit = false;

static double getProcessCPUUsage() {
    FILETIME ftime, fsys, fuser;
    ULARGE_INTEGER now, sys, user;
    double percent;

    if (!cpuInit) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        numProcessors = sysInfo.dwNumberOfProcessors;

        GetSystemTimeAsFileTime(&ftime);
        memcpy(&lastCPU, &ftime, sizeof(FILETIME));

        selfHandle = GetCurrentProcess();
        GetProcessTimes(selfHandle, &ftime, &ftime, &fsys, &fuser);
        memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
        memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));

        cpuInit = true;
        return 0.0;
    }

    GetSystemTimeAsFileTime(&ftime);
    memcpy(&now, &ftime, sizeof(FILETIME));

    GetProcessTimes(selfHandle, &ftime, &ftime, &fsys, &fuser);
    memcpy(&sys, &fsys, sizeof(FILETIME));
    memcpy(&user, &fuser, sizeof(FILETIME));

    percent = (sys.QuadPart - lastSysCPU.QuadPart) + (user.QuadPart - lastUserCPU.QuadPart);
    percent /= (now.QuadPart - lastCPU.QuadPart);
    percent /= numProcessors;

    lastCPU = now;
    lastUserCPU = user;
    lastSysCPU = sys;

    return percent * 100.0;
}

static size_t getProcessRAMUsageMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
    return 0;
}

static std::string resolveShaderBinaryPath(const char* shaderFileName) {
    const std::filesystem::path shaderPath = std::filesystem::path("bin") / "shaders" / shaderFileName;
    if (std::filesystem::exists(shaderPath)) {
        return shaderPath.generic_string();
    }

    throw std::runtime_error(
        "Missing compiled shader binary: " + shaderPath.generic_string() +
        ". Run 'mingw32-make shaders' before launching the engine.");
}

static std::string prepareMetricsLogPath() {
    const std::filesystem::path logDir = "logs";
    std::filesystem::create_directories(logDir);

    const std::filesystem::path metricsPath = logDir / "metrics_latest.txt";
    if (FILE* file = std::fopen(metricsPath.string().c_str(), "w")) {
        std::fclose(file);
    }

    return metricsPath.generic_string();
}

int main() {
    // Flush stdout after every write so log files are always up-to-date
    // even when output is redirected (full-buffering mode by default).
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    timeBeginPeriod(1);

    // Set working directory to project root (parent of bin/)
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path projectRoot = exeDir.parent_path();
        if (std::filesystem::exists(projectRoot / "bin")) {
            std::filesystem::current_path(projectRoot);
        }
    }

    try {
        const std::string metricsLogPath = prepareMetricsLogPath();

        const uint32_t WIDTH  = 1280;
        const uint32_t HEIGHT = 720;

        core::Window window("ProtoEngine — Voxel World", WIDTH, HEIGHT);
        std::cout << "Window created.\n";

        gfx::VulkanContext vulkanContext(window);
        std::cout << "VulkanContext created.\n";

        gfx::Swapchain swapchain(vulkanContext, window);
        std::cout << "Swapchain created.\n";

        gfx::BindlessSystem bindlessSystem(vulkanContext);
        std::cout << "BindlessSystem created.\n";

        gfx::Renderer renderer(vulkanContext, swapchain, window, bindlessSystem);
        std::cout << "Renderer created.\n";

        gfx::GeometryManager geometryManager(vulkanContext);
        std::cout << "GeometryManager created.\n";

        gfx::Texture checkerTexture(vulkanContext, bindlessSystem);
        checkerTexture.createCheckerboard(256, 256);
        std::cout << "Checkerboard Texture created (ID=" << checkerTexture.getID() << ").\n";

        ui::TextRenderer textRenderer(vulkanContext, renderer);
        std::cout << "TextRenderer created.\n";

        ui::ImGuiManager imguiManager(vulkanContext, window, swapchain);
        std::cout << "ImGuiManager created.\n";

        // ---- Voxel World (ChunkManager) ------------------------------------
        // MeshWorker uses hardware_concurrency() threads by default
        world::ChunkManager chunkManager(vulkanContext, geometryManager);
        int initialWorldRadius = 10;
        chunkManager.setRenderRadius(initialWorldRadius);
        int worldSeed = 42;
        // Initial generation with island defaults
        world::TerrainConfig initCfg;
        initCfg.seed            = worldSeed;
        initCfg.worldRadiusBlks = initialWorldRadius * world::CHUNK_SIZE;
        chunkManager.generateWorld(initialWorldRadius, initialWorldRadius, initCfg);
        // Wait for all async meshing to complete before first frame
        chunkManager.rebuildDirtyChunks(vulkanContext.getDevice(), 0.0f);
        std::cout << "ChunkManager created: " << chunkManager.getChunkCount()
                  << " chunks, " << chunkManager.getWorkerThreads() << " worker threads.\n";

        // ---- Legacy World (kept for reference, not rendered) ---------------
        world::BlockRegistry::registerDefaults();

        // ---- Shader paths --------------------------------------------------
        std::string vertPath       = resolveShaderBinaryPath("simple.vert.spv");
        std::string fragPath       = resolveShaderBinaryPath("simple.frag.spv");
        std::string shadowVertPath = resolveShaderBinaryPath("shadow.vert.spv");
        std::string shadowFragPath = resolveShaderBinaryPath("shadow.frag.spv");
        std::string voxelVertPath  = resolveShaderBinaryPath("voxel.vert.spv");
        std::string voxelFragPath  = resolveShaderBinaryPath("voxel.frag.spv");

        // ---- Hot Reloader --------------------------------------------------
        core::ShaderHotReloader reloader;
        reloader.watch("shaders/simple.vert");
        reloader.watch("shaders/simple.frag");
        reloader.watch("shaders/shadow.vert");
        reloader.watch("shaders/shadow.frag");
        reloader.watch("shaders/voxel.vert");
        reloader.watch("shaders/voxel.frag");
        reloader.start();

        // ---- Push constant ranges ------------------------------------------
        VkPushConstantRange stdPCRange{};
        stdPCRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        stdPCRange.offset     = 0;
        stdPCRange.size       = sizeof(PushConstants);

        VkPushConstantRange voxelPCRange{};
        voxelPCRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        voxelPCRange.offset     = 0;
        voxelPCRange.size       = sizeof(VoxelGlobalPush); // 128 bytes, no VoxelChunkPush

        // ---- Main Pipeline (standard gfx::Vertex) --------------------------
        gfx::PipelineConfig mainPipelineConfig{};
        mainPipelineConfig.colorAttachmentFormats = {swapchain.getImageFormat()};
        mainPipelineConfig.depthAttachmentFormat  = swapchain.getDepthFormat();
        mainPipelineConfig.vertexShaderPath       = vertPath;
        mainPipelineConfig.fragmentShaderPath     = fragPath;
        mainPipelineConfig.enableDepthTest        = true;
        mainPipelineConfig.cullMode               = VK_CULL_MODE_BACK_BIT;
        mainPipelineConfig.frontFace              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        mainPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout());
        mainPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout());
        mainPipelineConfig.pushConstantRanges.push_back(stdPCRange);
        gfx::Pipeline mainPipeline(vulkanContext, mainPipelineConfig);

        // ---- Shadow Pipeline -----------------------------------------------
        gfx::PipelineConfig shadowPipelineConfig{};
        shadowPipelineConfig.colorAttachmentFormats = {};
        shadowPipelineConfig.depthAttachmentFormat  = VK_FORMAT_D32_SFLOAT;
        shadowPipelineConfig.vertexShaderPath       = shadowVertPath;
        shadowPipelineConfig.fragmentShaderPath     = shadowFragPath;
        shadowPipelineConfig.enableDepthTest        = true;
        shadowPipelineConfig.cullMode               = VK_CULL_MODE_FRONT_BIT;
        shadowPipelineConfig.frontFace              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowPipelineConfig.depthBiasEnable        = false;
        shadowPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout());
        shadowPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout());
        shadowPipelineConfig.pushConstantRanges.push_back(stdPCRange);
        gfx::Pipeline shadowPipeline(vulkanContext, shadowPipelineConfig);

        // ---- Voxel Pipeline (VoxelVertex — 8 bytes) ------------------------
        {
            // Verify VoxelVertex binding/attribute descriptions compile
            [[maybe_unused]] auto vb = world::VoxelVertex::getBindingDescription();
            [[maybe_unused]] auto va = world::VoxelVertex::getAttributeDescriptions();
        }

        gfx::PipelineConfig voxelPipelineConfig{};
        voxelPipelineConfig.colorAttachmentFormats = {swapchain.getImageFormat()};
        voxelPipelineConfig.depthAttachmentFormat  = swapchain.getDepthFormat();
        voxelPipelineConfig.vertexShaderPath       = voxelVertPath;
        voxelPipelineConfig.fragmentShaderPath     = voxelFragPath;
        voxelPipelineConfig.enableDepthTest        = true;
        voxelPipelineConfig.cullMode               = VK_CULL_MODE_BACK_BIT;
        voxelPipelineConfig.frontFace              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        // VoxelVertex binding + attributes
        voxelPipelineConfig.bindingDescriptions   = {world::VoxelVertex::getBindingDescription()};
        auto voxelAttrs = world::VoxelVertex::getAttributeDescriptions();
        voxelPipelineConfig.attributeDescriptions = {voxelAttrs[0], voxelAttrs[1]};
        // Descriptor sets: set=0 (shadow/renderer), set=1 (bindless + palette), set=2 (SSBO chunk instances)
        voxelPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout());
        voxelPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout());
        voxelPipelineConfig.descriptorSetLayouts.push_back(chunkManager.getRenderer().getDescriptorSetLayout());
        voxelPipelineConfig.pushConstantRanges.push_back(voxelPCRange);
        // ---- Voxel Depth Pre-Pass Pipeline ---------------------------------
        gfx::PipelineConfig voxelDepthPrePassConfig = voxelPipelineConfig;
        voxelDepthPrePassConfig.colorAttachmentFormats = {}; // Вимкнути запис кольору
        voxelDepthPrePassConfig.depthWriteEnable = VK_TRUE;
        voxelDepthPrePassConfig.depthCompareOp = VK_COMPARE_OP_LESS;
        gfx::Pipeline voxelDepthPrePass(vulkanContext, voxelDepthPrePassConfig);

        // ---- Voxel Color Pass Pipeline -------------------------------------
        // Модифікуємо оригінальний конфіг для основного кольорового пасу
        voxelPipelineConfig.depthWriteEnable = VK_FALSE;
        voxelPipelineConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        gfx::Pipeline voxelPipeline(vulkanContext, voxelPipelineConfig);

        // ---- Voxel Wireframe Pipeline (VK_POLYGON_MODE_LINE) ---------------
        gfx::PipelineConfig voxelWireConfig = voxelPipelineConfig; // copy all settings
        voxelWireConfig.polygonMode = VK_POLYGON_MODE_LINE;
        voxelWireConfig.cullMode    = VK_CULL_MODE_NONE; // show all edges
        gfx::Pipeline voxelWirePipeline(vulkanContext, voxelWireConfig);

        // ---- Wireframe toggle state ----------------------------------------
        bool wireframe = false;

        // Камера над центром світу, дивиться вперед (+Z напрямок)
        // З новою генерацією: baseHeight=64, amplitude=60 → max terrain ~124
        // Камера вище рівня гір, дивиться вниз під кутом 40°
        scene::Camera camera({0.0f, 200.0f, 250.0f}, 60.0f, renderer.getAspectRatio());
        camera.setPitch(-40.0f);
        camera.adjustSpeed(20.0f / 5.0f);

        // ---- Debug Camera
        scene::Camera debugCamera({0.0f, 200.0f, 300.0f}, 60.0f, renderer.getAspectRatio());
        debugCamera.setPitch(-30.0f);
        debugCamera.adjustSpeed(20.0f / 5.0f);
        bool debugCameraMode    = false;
        bool controlMainInDebug = false;

        // ---- Debug Renderer (lines: frustum, camera marker) -----------------
        gfx::DebugRenderer debugRenderer(vulkanContext, nullptr,
            swapchain.getImageFormat(), swapchain.getDepthFormat(),
            "bin/shaders/debug.vert.spv",
            "bin/shaders/debug.frag.spv");

        // ---- Timer ---------------------------------------------------------
        core::Timer timer;

        // ---- Interaction parameters ----------------------------------------
        float reachDistance = 10.0f; // max raycast distance (m)
        int   brushSize     = 1;     // brush cube side length (1 = single voxel)
        bool  autoLOD       = true;  // автоматично перемешувати чанки при зміні LOD
        int   worldRadius   = 10;    // Бажаний розмір світу в радіусі чанків (10 = 21x21 чанків)

        // ---- Persistent terrain generation config (editable via ImGui) ----
        world::TerrainConfig terrainCfg;
        terrainCfg.seed            = worldSeed;
        terrainCfg.worldRadiusBlks = worldRadius * world::CHUNK_SIZE;

        // ---- Raycast state (persistent across frames for ImGui display) ----
        world::RayResult lastRayHit{};

        // ---- FPS Cap and Smoothing -----------------------------------------
        const double targetFrameTime = 1.0 / 4000.0;
        float displayFPS = 0.0f, displayMs = 0.0f;
        float displayCullMs = 0.0f, displayRenderMs = 0.0f;
        double displayCPU = 0.0;
        size_t displayRAM = 0;
        float displayAcquireMs = 0.0f, displayWaitFenceMs = 0.0f, displaySubmitMs = 0.0f, displayPresentMs = 0.0f;
        float displayUpdateMs = 0.0f, displayRecordMs = 0.0f;
        float displayEventsMs = 0.0f, displayLodMs = 0.0f, displayRebuildMs = 0.0f, displayRaycastMs = 0.0f, displayUiMs = 0.0f;
        float statsTimer = 0.0f;

        // ---- Palette Data --------------------------------------------------
        // Must match VoxelData palette indices used in Chunk::fillTerrain:
        //   0=air  1=stone  2=grass  3=dirt  4=sand  5=snow  6=water
        gfx::BindlessSystem::PaletteUBO paletteData{};
        paletteData.colors[0]  = {0.0f,  0.0f,  0.0f,  0.f}; // 0  AIR       (transparent)
        paletteData.colors[1]  = {0.50f, 0.50f, 0.50f, 1.f}; // 1  Stone
        paletteData.colors[2]  = {0.30f, 0.65f, 0.20f, 1.f}; // 2  Grass     (green)
        paletteData.colors[3]  = {0.52f, 0.33f, 0.16f, 1.f}; // 3  Dirt      (brown)
        paletteData.colors[4]  = {0.85f, 0.80f, 0.50f, 1.f}; // 4  Sand
        paletteData.colors[5]  = {0.90f, 0.93f, 0.97f, 1.f}; // 5  Snow
        paletteData.colors[6]  = {0.18f, 0.40f, 0.82f, 1.f}; // 6  Water
        paletteData.colors[7]  = {0.40f, 0.25f, 0.10f, 1.f}; // 7  Wood
        paletteData.colors[8]  = {0.15f, 0.45f, 0.10f, 1.f}; // 8  Leaves
        paletteData.colors[9]  = {0.90f, 0.30f, 0.05f, 1.f}; // 9  Lava
        paletteData.colors[10] = {0.70f, 0.70f, 0.70f, 1.f}; // 10 Cobblestone
        paletteData.colors[11] = {0.95f, 0.90f, 0.60f, 1.f}; // 11 Sandstone
        paletteData.colors[12] = {0.60f, 0.10f, 0.10f, 1.f}; // 12 Brick
        paletteData.colors[13] = {0.20f, 0.20f, 0.20f, 1.f}; // 13 Coal Ore
        paletteData.colors[14] = {0.80f, 0.70f, 0.20f, 1.f}; // 14 Gold Ore
        paletteData.colors[15] = {0.40f, 0.60f, 0.80f, 1.f}; // 15 Diamond Ore

        // ---- Wait for initial world generation -----------------------------
        // Block until all background worker tasks complete
        chunkManager.waitAllWorkers();
        
        // Force one pass of GPU upload before the first frame
        chunkManager.rebuildDirtyChunks(vulkanContext.getDevice(), 0.0f);

        // ---- Main Loop -----------------------------------------------------
        uint64_t absoluteFrame = 0;
        while (!window.shouldClose()) {
            absoluteFrame++;
            timer.update();
            float dt = timer.getDeltaTime();
            float currentTime = static_cast<float>(timer.getTotalTime());

            auto updateStart = std::chrono::high_resolution_clock::now();

            auto t0 = std::chrono::high_resolution_clock::now();
            core::InputManager::get().update();
            window.pollEvents();

            if (reloader.shouldReload()) {
                renderer.reloadShaders();
                reloader.ackReload();
            }

            if (window.shouldClose()) break;
            auto t1 = std::chrono::high_resolution_clock::now();

            camera.setAspectRatio(renderer.getAspectRatio());
            debugCamera.setAspectRatio(renderer.getAspectRatio());

            scene::Camera& activeCamera = debugCameraMode ? debugCamera : camera;

            // ---- Mouse wheel: adjust active camera speed --------------------
            if (!ImGui::GetIO().WantCaptureMouse) {
                float wheel = core::InputManager::get().getMouseWheelDelta();
                if (wheel != 0.0f) {
                    float factor = (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
                    int ticks = static_cast<int>(std::abs(wheel) + 0.5f);
                    for (int i = 0; i < ticks; ++i)
                        activeCamera.adjustSpeed(factor);
                }
            }

            if (!ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard) {
                // In debug mode: controlMainInDebug determines which camera moves
                scene::Camera& moveTarget = (debugCameraMode && controlMainInDebug)
                    ? camera : activeCamera;
                moveTarget.update(dt);
            }

            // ---- Build frustum of the MAIN camera (for streaming/LOD) -------
            // This always uses the main camera regardless of debug mode.
            core::math::Mat4 mainViewProj =
                camera.getProjectionMatrix() * camera.getViewMatrix();
            scene::Frustum mainFrustum;
            mainFrustum.extractPlanes(mainViewProj);

            // ---- Frustum used for RENDERING (i.e. culling visible chunks) ----
            core::math::Mat4 renderViewProj = debugCameraMode
                ? (debugCamera.getProjectionMatrix() * debugCamera.getViewMatrix())
                : mainViewProj;
            scene::Frustum frustum;
            frustum.extractPlanes(renderViewProj);

            // ---- LOD update always uses MAIN camera -------------------------
            if (autoLOD) {
                chunkManager.updateCamera(camera.getPosition(), mainFrustum);
            }
            auto t2 = std::chrono::high_resolution_clock::now();

            // ---- Collect async mesh results (non-blocking) -----------------
            chunkManager.rebuildDirtyChunks(vulkanContext.getDevice(), currentTime);
            geometryManager.update(absoluteFrame);
            auto t3 = std::chrono::high_resolution_clock::now();

            // ---- Raycast from mouse cursor position (screen-to-world) ------
            // Always use the actual mouse cursor position for picking.
            // Camera look (RMB held) rotates the camera, which changes getFront(),
            // but the raycast still uses the cursor — this is correct because
            // when RMB is held the cursor is hidden and stays at center anyway.
            {
                int mx, my;
                core::InputManager::get().getMousePosition(mx, my);
                VkExtent2D ext = swapchain.getExtent();
                int sw = static_cast<int>(ext.width);
                int sh = static_cast<int>(ext.height);

                // Always unproject mouse position — works correctly in both modes:
                // - Free cursor: picks the voxel under the cursor
                // - RMB look mode: cursor is at center → same as getFront()
                core::math::Vec3 rayDir = camera.getRayFromMouse(mx, my, sw, sh);

                lastRayHit = world::raycast(chunkManager,
                                            camera.getPosition(),
                                            rayDir,
                                            reachDistance);
            }
            auto t4 = std::chrono::high_resolution_clock::now();

            // ---- Block interaction (LMB / RMB) — only if ImGui not capturing
            // Use ImGui::IsWindowHovered to avoid acting when cursor is over UI
            bool imguiWantsMouse = ImGui::GetIO().WantCaptureMouse
                                || ImGui::IsAnyItemActive()
                                || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);

            if (!imguiWantsMouse) {
                auto& input = core::InputManager::get();
                int half = brushSize / 2;

                // LMB — remove voxels in brush cube
                if (input.isMouseButtonJustPressed(0) && lastRayHit.hit) {
                    for (int bx = -half; bx <= half; ++bx)
                    for (int by = -half; by <= half; ++by)
                    for (int bz = -half; bz <= half; ++bz) {
                        chunkManager.setVoxel(
                            lastRayHit.voxelX + bx,
                            lastRayHit.voxelY + by,
                            lastRayHit.voxelZ + bz,
                            world::VOXEL_AIR);
                    }
                    chunkManager.flushDirty(); // batch-submit all dirty chunks once
                }

                // MMB (Middle Mouse Button) or F key — place voxel on the hit face.
                // RMB is used for camera look, so place is on MMB to avoid conflict.
                // normalX/Y/Z is the inward normal (direction ray entered the face).
                // To place OUTSIDE the hit voxel, we use +normal (opposite of inward).
                bool placePressed = input.isMouseButtonJustPressed(2)  // MMB
                                 || input.isKeyJustPressed('F');        // F key fallback
                if (placePressed && lastRayHit.hit) {
                    // +normal = outward face normal = adjacent empty voxel position
                    int px = lastRayHit.voxelX + lastRayHit.normalX;
                    int py = lastRayHit.voxelY + lastRayHit.normalY;
                    int pz = lastRayHit.voxelZ + lastRayHit.normalZ;
                    for (int bx = -half; bx <= half; ++bx)
                    for (int by = -half; by <= half; ++by)
                    for (int bz = -half; bz <= half; ++bz) {
                        chunkManager.setVoxel(
                            px + bx, py + by, pz + bz,
                            world::VoxelData::make(3, 255, 0, world::VOXEL_FLAG_SOLID));
                    }
                    chunkManager.flushDirty();
                }
            }

            // ---- ImGui frame -----------------------------------------------
            imguiManager.beginFrame();
            {
                auto pos = camera.getPosition();

                // ---- Shared EWMA smoothing (runs once per frame, feeds all panels) ----
                float currentMs        = timer.getDeltaTimeMs();
                float currentFPS       = (currentMs > 0.0f) ? (1000.0f / currentMs) : 0.0f;
                float currentAcquireMs   = static_cast<float>(renderer.getAcquireTimeMs());
                float currentWaitFenceMs = static_cast<float>(renderer.getWaitFenceTimeMs());
                float currentSubmitMs    = static_cast<float>(renderer.getSubmitTimeMs());
                float currentPresentMs   = static_cast<float>(renderer.getPresentTimeMs());
                float currentEventsMs    = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
                float currentLodMs       = static_cast<float>(std::chrono::duration<double, std::milli>(t2 - t1).count());
                float currentRebuildMs   = static_cast<float>(std::chrono::duration<double, std::milli>(t3 - t2).count());
                float currentRaycastMs   = static_cast<float>(std::chrono::duration<double, std::milli>(t4 - t3).count());

                if (displayFPS == 0.0f) {
                    displayFPS         = currentFPS;       displayMs          = currentMs;
                    displayAcquireMs   = currentAcquireMs; displayWaitFenceMs = currentWaitFenceMs;
                    displaySubmitMs    = currentSubmitMs;  displayPresentMs   = currentPresentMs;
                    displayEventsMs    = currentEventsMs;  displayLodMs       = currentLodMs;
                    displayRebuildMs   = currentRebuildMs; displayRaycastMs   = currentRaycastMs;
                } else {
                    displayFPS         = displayFPS         * 0.95f + currentFPS         * 0.05f;
                    displayMs          = displayMs          * 0.95f + currentMs          * 0.05f;
                    displayAcquireMs   = displayAcquireMs   * 0.95f + currentAcquireMs   * 0.05f;
                    displayWaitFenceMs = displayWaitFenceMs * 0.95f + currentWaitFenceMs * 0.05f;
                    displaySubmitMs    = displaySubmitMs    * 0.95f + currentSubmitMs    * 0.05f;
                    displayPresentMs   = displayPresentMs   * 0.95f + currentPresentMs   * 0.05f;
                    displayEventsMs    = displayEventsMs    * 0.95f + currentEventsMs    * 0.05f;
                    displayLodMs       = displayLodMs       * 0.95f + currentLodMs       * 0.05f;
                    displayRebuildMs   = displayRebuildMs   * 0.95f + currentRebuildMs   * 0.05f;
                    displayRaycastMs   = displayRaycastMs   * 0.95f + currentRaycastMs   * 0.05f;
                }

                statsTimer += currentMs;
                if (statsTimer > 500.0f) {
                    displayCPU = getProcessCPUUsage();
                    displayRAM = getProcessRAMUsageMB();
                    const auto lifecycleStats = chunkManager.getLifecycleStats();
                    const std::string metricsLine =
                        "[Metrics] FPS: " + std::to_string(displayFPS)
                        + " | Visible chunks: " + std::to_string(chunkManager.getVisibleCount())
                        + " | Visible polys: "  + std::to_string(chunkManager.getVisibleVertices())
                        + " | GPU: " + std::to_string(renderer.getGpuFrameTimeMs()) + "ms"
                        + " | CPU: " + std::to_string(displayCPU) + "%"
                        + " | RAM: " + std::to_string(displayRAM) + "MB"
                        + " | Ready: " + std::to_string(lifecycleStats.ready)
                        + " | Generating: " + std::to_string(lifecycleStats.generating)
                        + " | Placeholders: " + std::to_string(lifecycleStats.placeholders)
                        + " | MeshUnassigned: " + std::to_string(lifecycleStats.meshUnassigned)
                        + " | MeshEvicted: " + std::to_string(lifecycleStats.meshEvicted)
                        + " | CachedModified: " + std::to_string(lifecycleStats.cachedModified);
                    std::cout << metricsLine << std::endl;
                    if (FILE* f = std::fopen(metricsLogPath.c_str(), "a")) {
                        std::fprintf(f, "%s\n", metricsLine.c_str());
                        std::fclose(f);
                    }
                    statsTimer = 0.0f;
                }

                // ============================================================
                // Panel 1 — Performance & Metrics
                // ============================================================
                ImVec2 dispSize = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos( ImVec2(10.0f, 10.0f), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(330, 510), ImGuiCond_Once);
                ImGui::Begin("Performance & Metrics");

                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "FPS:  %.1f  (%.2f ms)", displayFPS, displayMs);
                ImGui::Separator();

                if (ImGui::CollapsingHeader("GPU Times", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                        "GPU Frame:   %.4f ms", renderer.getGpuFrameTimeMs());
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                        "vkAcquire:   %.4f ms", displayAcquireMs);
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                        "vkWaitFence: %.4f ms", displayWaitFenceMs);
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                        "vkQueueSub:  %.4f ms", displaySubmitMs);
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                        "vkPresent:   %.4f ms", displayPresentMs);
                }

                if (ImGui::CollapsingHeader("CPU Times", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                        "Update total: %.4f ms", displayUpdateMs);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                        "  Events:     %.4f ms", displayEventsMs);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                        "  LOD Cam:    %.4f ms", displayLodMs);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                        "  Rebuild:    %.4f ms", displayRebuildMs);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                        "  Raycast:    %.4f ms", displayRaycastMs);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                        "  ImGui UI:   %.4f ms", displayUiMs);
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                        "Render Rec:   %.4f ms", displayRecordMs);
                    ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f),
                        "Cull (CPU):   %.4f ms", displayCullMs);
                    ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f),
                        "Render (CPU): %.4f ms", displayRenderMs);
                }

                if (ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                        "CPU Usage: %.1f%%", displayCPU);
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                        "RAM Usage: %zu MB", displayRAM);
                }

                ImGui::Separator();
                if (ImGui::CollapsingHeader("Chunk Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
                    uint32_t visChunks = chunkManager.getVisibleCount();
                    uint32_t visPolys  = chunkManager.getVisibleVertices(); // triangles
                    auto lifecycleStats = chunkManager.getLifecycleStats();
                    ImGui::Text("Total chunks:   %u", chunkManager.getChunkCount());
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "Visible (GPU):  %u chunks", visChunks);
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "Visible polys:  %u tris", visPolys);
                    ImGui::Text("Culled:         %u", chunkManager.getCulledCount());
                    ImGui::Text("Total verts:    %u", chunkManager.getTotalVertices());
                    ImGui::Text("Rebuild:        %.2f ms", chunkManager.getLastRebuildMs());
                    ImGui::Text("Worker threads: %u", chunkManager.getWorkerThreads());
                    ImGui::Text("Pending meshes: %d", chunkManager.getPendingMeshes());
                    ImGui::SeparatorText("Lifecycle");
                    ImGui::Text("Ready:          %u", lifecycleStats.ready);
                    ImGui::Text("Generating:     %u", lifecycleStats.generating);
                    ImGui::Text("Placeholders:   %u", lifecycleStats.placeholders);
                    ImGui::Text("Mesh unassigned:%u", lifecycleStats.meshUnassigned);
                    ImGui::Text("Mesh evicted:   %u", lifecycleStats.meshEvicted);
                    ImGui::Text("Cached modified:%u", lifecycleStats.cachedModified);
                    auto lodCounts = chunkManager.getLODCounts();
                    ImGui::Text("  LOD0 full:    %u", lodCounts[0]);
                    ImGui::Text("  LOD1 half:    %u", lodCounts[1]);
                    ImGui::Text("  LOD2 quarter: %u", lodCounts[2]);
                }

                ImGui::End(); // Performance & Metrics

                // ============================================================
                // Panel 2 — World & Camera  (top-right corner)
                // ============================================================
                ImGui::SetNextWindowPos( ImVec2(dispSize.x - 340.0f, 10.0f), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(330, 530), ImGuiCond_Once);
                ImGui::Begin("World & Camera");

                if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Pos:   %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
                    ImGui::Text("Yaw: %.1f  Pitch: %.1f", camera.getYaw(), camera.getPitch());
                    ImGui::Text("Speed: %.1f m/s  (scroll wheel)", camera.getSpeed());
                }

                if (ImGui::CollapsingHeader("LOD Settings")) {
                    ImGui::SliderFloat("LOD0->1 dist",  &chunkManager.getLodDist0(),       16.0f, 1024.0f, "%.0f blk");
                    ImGui::SliderFloat("LOD1->2 dist",  &chunkManager.getLodDist1(),       32.0f, 2048.0f, "%.0f blk");
                    ImGui::SliderFloat("Hysteresis",    &chunkManager.getLodHysteresis(),   0.0f,   64.0f, "%.1f blk");
                    ImGui::SliderFloat("Unload Radius", &chunkManager.getUnloadRadius(),   64.0f, 4096.0f, "%.0f blk");
                    ImGui::SliderFloat("View Dist",     &chunkManager.getFrustumRadius(),  64.0f, 8192.0f, "%.0f blk");
                }

                if (ImGui::CollapsingHeader("World Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Seed: %d", worldSeed);
                    if (ImGui::InputInt("World Radius", &worldRadius)) {
                        worldRadius = std::clamp(worldRadius, 1, 64);
                    }

                    // --- Phase 2: World Scale & Biome ---
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.4f,0.9f,0.4f,1.f), "Island & Biomes");

                    ImGui::SliderFloat("World Scale",  &terrainCfg.worldScale,   0.25f, 4.0f,  "%.2fx");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Higher = wider / gentler terrain features");

                    ImGui::SliderInt(  "Base Height",  &terrainCfg.baseHeight,   10,   120,   "%d blk");
                    ImGui::SliderFloat("Amplitude",    &terrainCfg.amplitude,    10.f, 120.f, "%.0f blk");
                    ImGui::SliderInt(  "Sea Level",    &terrainCfg.seaLevel,      0,   100,   "%d blk");
                    ImGui::SliderInt(  "Sand Margin",  &terrainCfg.sandMargin,    0,    20,   "%d blk");
                    ImGui::SliderInt(  "Snow Height",  &terrainCfg.snowHeight,   60,   200,   "%d blk");

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f,0.85f,1.0f,1.f), "Mountains & Rivers");
                    ImGui::SliderFloat("Mountain Str.",&terrainCfg.mountainStrength,0.0f,1.5f,"%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0=flat plains  1=dramatic sharp peaks");
                    ImGui::SliderFloat("Grass Cover",  &terrainCfg.stoneErosionThresh,0.3f,1.0f,"%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Erosion threshold for bare rock cliffs.\nHigher = more grass coverage on mountain slopes.");
                    ImGui::SliderFloat("Desert Thresh",&terrainCfg.desertMoistureThresh,-0.8f,0.2f,"%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Moisture below this value becomes desert/sand.\nMore negative = less desert.");
                    ImGui::SliderInt(  "River Depth",  &terrainCfg.riverDepth,    0,    50,   "%d blk");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How deep rivers carve below sea level");
                    ImGui::SliderFloat("River Width",  &terrainCfg.riverWidth,  0.02f,0.35f,"%.3f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Ridged threshold: lower = narrower rivers");

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f,0.85f,1.0f,1.f), "Island Shape");
                    ImGui::SliderFloat("Island Falloff",&terrainCfg.islandFalloff,0.2f,  0.9f, "%.2f");
                    ImGui::SliderFloat("Coast Noise",  &terrainCfg.islandEdgeNoise,0.0f,0.6f, "%.2f");
                    ImGui::Checkbox("Island Mode", &terrainCfg.islandMode);

                    ImGui::Spacing();
                    if (ImGui::Button("Rebuild World")) {
                        terrainCfg.seed            = worldSeed;
                        terrainCfg.worldRadiusBlks = worldRadius * world::CHUNK_SIZE;
                        chunkManager.generateWorld(worldRadius, worldRadius, terrainCfg);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("New Seed")) {
                        worldSeed = (worldSeed + 1337) % 99999;
                        terrainCfg.seed            = worldSeed;
                        terrainCfg.worldRadiusBlks = worldRadius * world::CHUNK_SIZE;
                        chunkManager.generateWorld(worldRadius, worldRadius, terrainCfg);
                    }
                }

                if (ImGui::CollapsingHeader("Interaction", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Reach (m)",  &reachDistance, 2.0f, 50.0f);
                    ImGui::SliderInt("Brush Size",   &brushSize,     1,    10);
                    ImGui::Text("Brush voxels: %d^3 = %d",
                        brushSize, brushSize * brushSize * brushSize);
                }

                if (ImGui::CollapsingHeader("Raycaster", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (lastRayHit.hit) {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                            "Target: %d, %d, %d",
                            lastRayHit.voxelX, lastRayHit.voxelY, lastRayHit.voxelZ);
                        ImGui::Text("Normal: %+d, %+d, %+d",
                            lastRayHit.normalX, lastRayHit.normalY, lastRayHit.normalZ);
                        ImGui::Text("Dist:   %.2f m", lastRayHit.distance);
                        ImGui::TextDisabled("LMB=remove  MMB/F=place  RMB=look");
                    } else {
                        ImGui::TextDisabled("No target in range");
                    }
                }

                ImGui::End(); // World & Camera

                // ============================================================
                // Panel 3 — Render & Debug  (bottom-left, starts collapsed)
                // ============================================================
                ImGui::SetNextWindowPos( ImVec2(10.0f, dispSize.y - 26.0f), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(330, 200), ImGuiCond_Once);
                ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
                ImGui::Begin("Render & Debug");

                // V-Sync
                bool useVSync = swapchain.getVSync();
                if (ImGui::Checkbox("V-Sync", &useVSync))
                    swapchain.setVSync(useVSync);

                // Wireframe
                ImGui::SameLine(0.0f, 20.0f);
                if (wireframe) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.3f, 0.0f, 1.0f));
                    if (ImGui::Button("[W] Wireframe: ON ")) wireframe = false;
                    ImGui::PopStyleColor(3);
                } else {
                    if (ImGui::Button("[W] Wireframe: OFF")) wireframe = true;
                }

                ImGui::Separator();

                // Debug Camera
                if (debugCameraMode) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.3f, 0.1f, 1.0f));
                    if (ImGui::Button("[ EXIT Debug Camera ]"))
                        debugCameraMode = false;
                    ImGui::PopStyleColor();
                    auto dbgPos = debugCamera.getPosition();
                    ImGui::SameLine();
                    ImGui::TextDisabled("@ %.0f,%.0f,%.0f", dbgPos.x, dbgPos.y, dbgPos.z);
                    ImGui::Checkbox("Control Main Camera (WASD)", &controlMainInDebug);
                    if (controlMainInDebug)
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                            ">> Moving MAIN camera (yellow frustum)");
                    else
                        ImGui::TextDisabled("Moving debug camera");
                } else {
                    if (ImGui::Button("Enter Debug Camera"))
                        debugCameraMode = true;
                    ImGui::SameLine();
                    ImGui::TextDisabled("(view frustum from outside)");
                }

                ImGui::End(); // Render & Debug
            }

            // ---- Light matrices --------------------------------------------
            core::math::Vec3 lightPos = {5.0f, 10.0f, 3.0f};
            core::math::Mat4 lightView = core::math::Mat4::lookAt(
                lightPos, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            core::math::Mat4 lightProj = core::math::Mat4::ortho(
                -10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 20.0f);
            core::math::Mat4 lightSpaceMatrix = lightProj * lightView;

            auto updateEnd = std::chrono::high_resolution_clock::now();
            double currentUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
            double currentUiMs = std::chrono::duration<double, std::milli>(updateEnd - t4).count();

            if (displayUpdateMs == 0.0f) {
                displayUpdateMs = static_cast<float>(currentUpdateMs);
                displayUiMs = static_cast<float>(currentUiMs);
            } else {
                displayUpdateMs = displayUpdateMs * 0.95f + static_cast<float>(currentUpdateMs) * 0.05f;
                displayUiMs = displayUiMs * 0.95f + static_cast<float>(currentUiMs) * 0.05f;
            }

            // ---- Render frame ----------------------------------------------
            auto recordStart = std::chrono::high_resolution_clock::now();

            VkCommandBuffer commandBuffer = renderer.beginFrame();
            if (commandBuffer) {
                uint32_t currentFrame = renderer.getCurrentFrameIndex();


                // ---- GPU Compute Culling Pass ------------------------------
                double cullTime = 0.0;
                float shadowDistanceLimit = 150.0f; // Limit shadow distance to 150 blocks
                if (chunkManager.hasMesh()) {
                    auto cullStart = std::chrono::high_resolution_clock::now();
                    chunkManager.cull(commandBuffer, frustum, frustum, activeCamera.getPosition(), shadowDistanceLimit, currentTime, currentFrame);
                    auto cullEnd = std::chrono::high_resolution_clock::now();
                    cullTime = std::chrono::duration<double, std::milli>(cullEnd - cullStart).count();
                }
                if (displayCullMs == 0.0) displayCullMs = cullTime;
                else displayCullMs = displayCullMs * 0.95 + cullTime * 0.05;

                // ---- Depth Pre-Pass ----------------------------------------
                double renderTime = 0.0;
                
                renderer.beginDepthPrePass(commandBuffer);
                if (chunkManager.hasMesh()) {
                    auto renderStart = std::chrono::high_resolution_clock::now();
                    
                    voxelDepthPrePass.bind(commandBuffer);

                    VoxelGlobalPush vpc{};
                    vpc.viewProj         = renderViewProj;
                    vpc.lightSpaceMatrix = lightSpaceMatrix;
                    vkCmdPushConstants(commandBuffer, voxelDepthPrePass.getLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(VoxelGlobalPush), &vpc);

                    VkDescriptorSet descriptorSet = renderer.getDescriptorSet(currentFrame);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        voxelDepthPrePass.getLayout(), 0, 1, &descriptorSet, 0, nullptr);
                    bindlessSystem.bind(commandBuffer, voxelDepthPrePass.getLayout(), currentFrame, 1);

                    chunkManager.renderCamera(commandBuffer, voxelDepthPrePass.getLayout(), currentFrame);
                    
                    auto renderEnd = std::chrono::high_resolution_clock::now();
                    // renderTime += ... (not logged for prepass)
                }
                renderer.endDepthPrePass(commandBuffer);


                // Shadow pass (placeholder for voxel shadows)
                renderer.beginShadowPass(commandBuffer);
                // if (chunkManager.hasMesh()) chunkManager.renderShadow(commandBuffer, shadowPipelineLayout, currentFrame);
                renderer.endShadowPass(commandBuffer);

                // Update Palette UBO
                bindlessSystem.updatePalette(currentFrame, paletteData);

                // Main pass
                renderer.beginMainPass(commandBuffer);

                // ---- Voxel World (Color Pass) ------------------------------
                if (chunkManager.hasMesh()) {
                    auto renderStart = std::chrono::high_resolution_clock::now();
                    
                    // Select pipeline: wireframe or solid fill
                    gfx::Pipeline& activePipeline = wireframe ? voxelWirePipeline : voxelPipeline;
                    activePipeline.bind(commandBuffer);

                    VoxelGlobalPush vpc{};
                    vpc.viewProj         = renderViewProj;
                    vpc.lightSpaceMatrix = lightSpaceMatrix;
                    vkCmdPushConstants(commandBuffer, activePipeline.getLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(VoxelGlobalPush), &vpc);

                    VkDescriptorSet descriptorSet = renderer.getDescriptorSet(currentFrame);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        activePipeline.getLayout(), 0, 1, &descriptorSet, 0, nullptr);
                    bindlessSystem.bind(commandBuffer, activePipeline.getLayout(), currentFrame, 1);

                    chunkManager.renderCamera(commandBuffer, activePipeline.getLayout(), currentFrame);
                    
                    auto renderEnd = std::chrono::high_resolution_clock::now();
                    renderTime += std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
                }
                if (displayRenderMs == 0.0) displayRenderMs = renderTime;
                else displayRenderMs = displayRenderMs * 0.95 + renderTime * 0.05;

                // ---- Debug Geometry (frustum + camera marker) ---------------
                if (debugCameraMode) {
                    debugRenderer.begin();

                    // Invert main camera VP to get world-space frustum corners
                    core::math::Mat4 invMain = core::math::Mat4::inverse(mainViewProj);
                    debugRenderer.addFrustum(invMain, {1.0f, 1.0f, 0.0f}); // yellow
                    debugRenderer.addCameraMarker(invMain, {1.0f, 0.5f, 0.0f}); // orange

                    debugRenderer.draw(commandBuffer, renderViewProj);
                }

                // ---- ImGui -------------------------------------------------
                textRenderer.beginFrame(currentFrame);
                imguiManager.render(commandBuffer);

                renderer.endMainPass(commandBuffer);
                renderer.endFrame(commandBuffer);
            }
            auto recordEnd = std::chrono::high_resolution_clock::now();
            double currentRecordMs = std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();
            if (displayRecordMs == 0.0f) displayRecordMs = static_cast<float>(currentRecordMs);
            else displayRecordMs = displayRecordMs * 0.95f + static_cast<float>(currentRecordMs) * 0.05f;

        }

        vkDeviceWaitIdle(vulkanContext.getDevice());

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "ProtoEngine — Fatal Error", MB_OK | MB_ICONERROR);
        timeEndPeriod(1);
        return EXIT_FAILURE;
    }

    timeEndPeriod(1);
    return EXIT_SUCCESS;
}
