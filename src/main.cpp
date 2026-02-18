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
        float cameraSpeed = 20.0f;

        // ---- Timer ---------------------------------------------------------
        core::Timer timer;

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
            if (!ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard)
                camera.update(dt);

            // ---- Collect async mesh results (non-blocking) -----------------
            chunkManager.rebuildDirtyChunks(vulkanContext.getDevice());

            // ---- Build frustum for this frame ------------------------------
            core::math::Mat4 viewProjForFrustum =
                camera.getProjectionMatrix() * camera.getViewMatrix();
            scene::Frustum frustum;
            frustum.extractPlanes(viewProjForFrustum);

            // ---- ImGui frame -----------------------------------------------
            imguiManager.beginFrame();
            {
                auto pos = camera.getPosition();
                ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_Once);
                ImGui::Begin("Debug Tools");

                ImGui::Text("FPS:  %.1f  (%.2f ms)", timer.getFPS(), timer.getDeltaTimeMs());
                ImGui::Separator();
                ImGui::Text("Camera: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
                ImGui::Text("Yaw: %.1f  Pitch: %.1f", camera.getYaw(), camera.getPitch());
                ImGui::SliderFloat("Speed", &cameraSpeed, 1.0f, 100.0f);
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
                if (ImGui::Button("Regenerate")) {
                    chunkManager.generateWorld(worldRadius, worldRadius, worldSeed);
                }
                ImGui::SameLine();
                if (ImGui::Button("New Seed")) {
                    worldSeed = (worldSeed + 1337) % 99999;
                    chunkManager.generateWorld(worldRadius, worldRadius, worldSeed);
                }

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
                    voxelPipeline.bind(commandBuffer);

                    core::math::Mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();

                    VoxelPushConstants vpc{};
                    vpc.viewProj         = viewProj;
                    vpc.lightSpaceMatrix = lightSpaceMatrix;
                    // chunkOffset = -bias: converts biased uint8 coords back to true world coords
                    vpc.chunkOffsetX     = chunkManager.getWorldOriginX();
                    vpc.chunkOffsetY     = chunkManager.getWorldOriginY();
                    vpc.chunkOffsetZ     = chunkManager.getWorldOriginZ();
                    vpc._pad             = 0.0f;
                    vkCmdPushConstants(commandBuffer, voxelPipeline.getLayout(),
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
        }

        vkDeviceWaitIdle(vulkanContext.getDevice());

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        system("pause");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
