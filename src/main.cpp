#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <windows.h>

#include "core/Timer.hpp"
#include "core/Window.hpp"
#include "core/InputManager.hpp"
#include "gfx/core/VulkanContext.hpp"
#include "gfx/core/Swapchain.hpp"
#include "gfx/rendering/Renderer.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include "gfx/rendering/BindlessSystem.hpp"
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
struct VoxelPushConstants {
    core::math::Mat4 viewProj;          // 64 bytes
    core::math::Mat4 lightSpaceMatrix;  // 64 bytes (kept for future shadow pass)
    float chunkOffsetX;                 // 16 bytes total (vec3 + pad)
    float chunkOffsetY;
    float chunkOffsetZ;
    float _pad;
};
static_assert(sizeof(VoxelPushConstants) == 144, "VoxelPushConstants size mismatch");

int main() {
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
        world::ChunkManager chunkManager(geometryManager);
        int worldRadius = 3; // 7×7 = 49 chunks
        int worldSeed   = 42;
        chunkManager.generateWorld(worldRadius, worldRadius, worldSeed);
        // Wait for all async meshing to complete before first frame
        chunkManager.rebuildDirtyChunks(vulkanContext.getDevice());
        std::cout << "ChunkManager created: " << chunkManager.getChunkCount()
                  << " chunks, " << chunkManager.getWorkerThreads() << " worker threads.\n";

        // ---- Legacy World (kept for reference, not rendered) ---------------
        world::BlockRegistry::registerDefaults();

        // ---- Shader paths --------------------------------------------------
        std::string vertPath       = "bin/shaders/simple.vert.spv";
        std::string fragPath       = "bin/shaders/simple.frag.spv";
        std::string shadowVertPath = "bin/shaders/shadow.vert.spv";
        std::string shadowFragPath = "bin/shaders/shadow.frag.spv";
        std::string voxelVertPath  = "bin/shaders/voxel.vert.spv";
        std::string voxelFragPath  = "bin/shaders/voxel.frag.spv";

        if (!std::filesystem::exists(vertPath)) {
            vertPath       = "shaders/simple.vert.spv";
            fragPath       = "shaders/simple.frag.spv";
            shadowVertPath = "shaders/shadow.vert.spv";
            shadowFragPath = "shaders/shadow.frag.spv";
            voxelVertPath  = "shaders/voxel.vert.spv";
            voxelFragPath  = "shaders/voxel.frag.spv";
        }

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
        voxelPCRange.size       = sizeof(VoxelPushConstants);

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
        // Descriptor sets: set=0 (shadow/renderer), set=1 (bindless + palette)
        voxelPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout());
        voxelPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout());
        voxelPipelineConfig.pushConstantRanges.push_back(voxelPCRange);
        gfx::Pipeline voxelPipeline(vulkanContext, voxelPipelineConfig);

        // ---- Voxel Wireframe Pipeline (VK_POLYGON_MODE_LINE) ---------------
        gfx::PipelineConfig voxelWireConfig = voxelPipelineConfig; // copy all settings
        voxelWireConfig.polygonMode = VK_POLYGON_MODE_LINE;
        voxelWireConfig.cullMode    = VK_CULL_MODE_NONE; // show all edges
        gfx::Pipeline voxelWirePipeline(vulkanContext, voxelWireConfig);

        // ---- Wireframe toggle state ----------------------------------------
        bool wireframe = false;

        // ---- Camera --------------------------------------------------------
        // Камера над центром світу, дивиться вперед (+Z напрямок)
        // Чанки від (-48,0,-48) до (48,16,48) у world space
        // Yaw=0 → front=(1,0,0); Yaw=90 → front=(0,0,1)
        // Стартова позиція: над центром, дивимось на -Z (yaw=-90 за замовчуванням)
        // Але чанки є і в +Z і в -Z, тому ставимо камеру вище і дивимось вниз
        // Камера над центром, дивиться вниз під кутом 45°
        // Чанки: cx=-3..3, cz=-3..3 → world coords -48..48 (без bias)
        scene::Camera camera({0.0f, 80.0f, 80.0f}, 60.0f, renderer.getAspectRatio());
        camera.setPitch(-45.0f); // дивимось вниз на ландшафт
        camera.adjustSpeed(20.0f / 5.0f); // set initial speed to 20 m/s (default is 5)

        // ---- Timer ---------------------------------------------------------
        core::Timer timer;

        // ---- Interaction parameters ----------------------------------------
        float reachDistance = 10.0f; // max raycast distance (m)
        int   brushSize     = 1;     // brush cube side length (1 = single voxel)
        bool  autoLOD       = true;  // автоматично перемешувати чанки при зміні LOD

        // ---- Raycast state (persistent across frames for ImGui display) ----
        world::RayResult lastRayHit{};

        // ---- FPS Cap and Smoothing -----------------------------------------
        const double targetFrameTime = 1.0 / 4000.0;
        float displayFPS = 0.0f;
        float displayMs  = 0.0f;

        // ---- Palette Data --------------------------------------------------
        gfx::BindlessSystem::PaletteUBO paletteData{};
        paletteData.colors[0]  = {0.0f,  0.0f,  0.0f,  1.f}; // 0  AIR
        paletteData.colors[1]  = {0.50f, 0.50f, 0.50f, 1.f}; // 1  Stone
        paletteData.colors[2]  = {0.55f, 0.35f, 0.18f, 1.f}; // 2  Dirt
        paletteData.colors[3]  = {0.30f, 0.65f, 0.20f, 1.f}; // 3  Grass
        paletteData.colors[4]  = {0.85f, 0.80f, 0.50f, 1.f}; // 4  Sand
        paletteData.colors[5]  = {0.20f, 0.40f, 0.80f, 1.f}; // 5  Water
        paletteData.colors[6]  = {0.40f, 0.25f, 0.10f, 1.f}; // 6  Wood
        paletteData.colors[7]  = {0.15f, 0.45f, 0.10f, 1.f}; // 7  Leaves
        paletteData.colors[8]  = {0.90f, 0.92f, 0.95f, 1.f}; // 8  Snow
        paletteData.colors[9]  = {0.90f, 0.30f, 0.05f, 1.f}; // 9  Lava
        paletteData.colors[10] = {0.70f, 0.70f, 0.70f, 1.f}; // 10 Cobblestone
        paletteData.colors[11] = {0.95f, 0.90f, 0.60f, 1.f}; // 11 Sandstone
        paletteData.colors[12] = {0.60f, 0.10f, 0.10f, 1.f}; // 12 Brick
        paletteData.colors[13] = {0.20f, 0.20f, 0.20f, 1.f}; // 13 Coal Ore
        paletteData.colors[14] = {0.80f, 0.70f, 0.20f, 1.f}; // 14 Gold Ore
        paletteData.colors[15] = {0.40f, 0.60f, 0.80f, 1.f}; // 15 Diamond Ore

        // ---- Main Loop -----------------------------------------------------
        while (!window.shouldClose()) {
            timer.update();
            float dt = timer.getDeltaTime();

            core::InputManager::get().update();
            window.pollEvents();

            if (reloader.shouldReload()) {
                renderer.reloadShaders();
                reloader.ackReload();
            }

            if (window.shouldClose()) break;

            camera.setAspectRatio(renderer.getAspectRatio());

            // ---- Mouse wheel: adjust camera speed (×1.1 per tick) ----------
            if (!ImGui::GetIO().WantCaptureMouse) {
                float wheel = core::InputManager::get().getMouseWheelDelta();
                if (wheel != 0.0f) {
                    float factor = (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
                    int ticks = static_cast<int>(std::abs(wheel) + 0.5f);
                    for (int i = 0; i < ticks; ++i)
                        camera.adjustSpeed(factor);
                }
            }

            if (!ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard)
                camera.update(dt);

            // ---- LOD update (must be BEFORE rebuildDirtyChunks) -------------
            // updateCamera: оновлює m_cameraPos, порівнює LOD з гістерезисом,
            // викликає markDirty + flushDirty для змінених чанків.
            if (autoLOD) {
                chunkManager.updateCamera(camera.getPosition());
            }

            // ---- Collect async mesh results (non-blocking) -----------------
            chunkManager.rebuildDirtyChunks(vulkanContext.getDevice());

            // ---- Build frustum for this frame ------------------------------
            core::math::Mat4 viewProjForFrustum =
                camera.getProjectionMatrix() * camera.getViewMatrix();
            scene::Frustum frustum;
            frustum.extractPlanes(viewProjForFrustum);

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
                ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(340, 320), ImGuiCond_Once);
                ImGui::Begin("Debug Tools");

                // Smooth FPS display (EWMA filter)
                float currentMs = timer.getDeltaTimeMs();
                float currentFPS = (currentMs > 0.0f) ? (1000.0f / currentMs) : 0.0f;
                if (displayFPS == 0.0f) {
                    displayFPS = currentFPS;
                    displayMs = currentMs;
                } else {
                    displayFPS = displayFPS * 0.95f + currentFPS * 0.05f;
                    displayMs  = displayMs  * 0.95f + currentMs  * 0.05f;
                }

                ImGui::Text("FPS:  %.1f  (%.2f ms)", displayFPS, displayMs);
                ImGui::Separator();
                ImGui::Text("Camera: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
                ImGui::Text("Yaw: %.1f  Pitch: %.1f", camera.getYaw(), camera.getPitch());
                ImGui::Text("Speed:  %.1f m/s  (scroll wheel)", camera.getSpeed());
                ImGui::Separator();

                ImGui::Text("--- ChunkManager ---");
                ImGui::Text("Chunks total:   %u", chunkManager.getChunkCount());
                ImGui::Text("Visible:        %u", chunkManager.getVisibleCount());
                ImGui::Text("Culled:         %u", chunkManager.getCulledCount());
                ImGui::Text("Verts (vis):    %u", chunkManager.getVisibleVertices());
                ImGui::Text("Verts (total):  %u", chunkManager.getTotalVertices());
                ImGui::Text("Rebuild:        %.2f ms", chunkManager.getLastRebuildMs());
                ImGui::Text("Worker threads: %u", chunkManager.getWorkerThreads());
                ImGui::Text("Pending meshes: %d", chunkManager.getPendingMeshes());

                ImGui::Separator();
                ImGui::Text("--- LOD System ---");
                // Auto-LOD прапорець: вмикає/вимикає автоматичне перемешування
                ImGui::Checkbox("Auto-LOD", &autoLOD);
                ImGui::SameLine();
                ImGui::TextDisabled("(re-mesh on camera move)");
                if (!autoLOD) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Force update")) {
                        chunkManager.updateCamera(camera.getPosition());
                    }
                }
                ImGui::SliderFloat("LOD0→1 dist",  &chunkManager.getLodDist0(),      16.0f, 1024.0f, "%.0f blk");
                ImGui::SliderFloat("LOD1→2 dist",  &chunkManager.getLodDist1(),      32.0f, 2048.0f, "%.0f blk");
                ImGui::SliderFloat("Hysteresis",   &chunkManager.getLodHysteresis(),  0.0f,   64.0f, "%.1f blk");
                {
                    auto lodCounts = chunkManager.getLODCounts();
                    ImGui::Text("LOD 0 (full):    %u chunks", lodCounts[0]);
                    ImGui::Text("LOD 1 (half):    %u chunks", lodCounts[1]);
                    ImGui::Text("LOD 2 (quarter): %u chunks", lodCounts[2]);
                }

                ImGui::Separator();
                if (ImGui::Button("Rebuild World")) {
                    chunkManager.generateWorld(worldRadius, worldRadius, worldSeed);
                }
                ImGui::SameLine();
                if (ImGui::Button("New Seed")) {
                    worldSeed = (worldSeed + 1337) % 99999;
                    chunkManager.generateWorld(worldRadius, worldRadius, worldSeed);
                }

                ImGui::Separator();
                ImGui::Text("--- Interaction ---");
                ImGui::SliderFloat("Reach (m)", &reachDistance, 2.0f, 50.0f);
                ImGui::SliderInt("Brush Size", &brushSize, 1, 10);
                ImGui::Text("Brush voxels: %d^3 = %d",
                    brushSize, brushSize * brushSize * brushSize);

                ImGui::Separator();
                ImGui::Text("--- Raycaster ---");
                if (lastRayHit.hit) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "Target: %d, %d, %d",
                        lastRayHit.voxelX, lastRayHit.voxelY, lastRayHit.voxelZ);
                    ImGui::Text("Normal: %+d, %+d, %+d",
                        lastRayHit.normalX, lastRayHit.normalY, lastRayHit.normalZ);
                    ImGui::Text("Dist:   %.2f m", lastRayHit.distance);
                    ImGui::TextDisabled("LMB=remove  MMB/F=place  RMB=look");
                } else {
                    ImGui::TextDisabled("Target: none (max 10m)");
                }
                ImGui::Separator();
                // Wireframe toggle button (toggles between fill and wireframe)
                if (wireframe) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.3f, 0.0f, 1.0f));
                    if (ImGui::Button("[W] Wireframe: ON ")) wireframe = false;
                    ImGui::PopStyleColor(3);
                } else {
                    if (ImGui::Button("[W] Wireframe: OFF")) wireframe = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(fillModeNonSolid)");

                ImGui::End();
            }

            // ---- Light matrices --------------------------------------------
            core::math::Vec3 lightPos = {5.0f, 10.0f, 3.0f};
            core::math::Mat4 lightView = core::math::Mat4::lookAt(
                lightPos, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            core::math::Mat4 lightProj = core::math::Mat4::ortho(
                -10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 20.0f);
            core::math::Mat4 lightSpaceMatrix = lightProj * lightView;

            // ---- Render frame ----------------------------------------------
            VkCommandBuffer commandBuffer = renderer.beginFrame();
            if (commandBuffer) {
                uint32_t currentFrame = renderer.getCurrentFrameIndex();

                // Shadow pass (empty for now)
                renderer.beginShadowPass(commandBuffer);
                renderer.endShadowPass(commandBuffer);

                // Update Palette UBO
                bindlessSystem.updatePalette(currentFrame, paletteData);

                // Main pass
                renderer.beginMainPass(commandBuffer);

                // Bind geometry buffers (shared by all pipelines)
                geometryManager.bind(commandBuffer);

                // Bind descriptor sets (set 0 + set 1)
                VkDescriptorSet descriptorSet = renderer.getDescriptorSet();
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    voxelPipeline.getLayout(), 0, 1, &descriptorSet, 0, nullptr);
                bindlessSystem.bind(commandBuffer, voxelPipeline.getLayout(), currentFrame, 1);

                // ---- Voxel World -------------------------------------------
                if (chunkManager.hasMesh()) {
                    // Select pipeline: wireframe or solid fill
                    gfx::Pipeline& activePipeline = wireframe ? voxelWirePipeline : voxelPipeline;
                    activePipeline.bind(commandBuffer);

                    core::math::Mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();

                    VoxelPushConstants vpc{};
                    vpc.viewProj         = viewProj;
                    vpc.lightSpaceMatrix = lightSpaceMatrix;
                    vpc.chunkOffsetX     = chunkManager.getWorldOriginX();
                    vpc.chunkOffsetY     = chunkManager.getWorldOriginY();
                    vpc.chunkOffsetZ     = chunkManager.getWorldOriginZ();
                    vpc._pad             = 0.0f;
                    vkCmdPushConstants(commandBuffer, activePipeline.getLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(VoxelPushConstants), &vpc);

                    chunkManager.render(commandBuffer, frustum);
                }

                // ---- ImGui -------------------------------------------------
                textRenderer.beginFrame(currentFrame);
                imguiManager.render(commandBuffer);

                renderer.endMainPass(commandBuffer);
                renderer.endFrame(commandBuffer);
            }

            // ---- FPS Limit to 4000 -----------------------------------------
            double elapsed = timer.getDeltaTime();
            if (elapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - elapsed;
                std::this_thread::sleep_for(std::chrono::duration<double>(sleepTime));
            }
        }

        vkDeviceWaitIdle(vulkanContext.getDevice());

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        system("pause");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
