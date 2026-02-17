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

struct PushConstants {
    core::math::Mat4 viewProj;
    core::math::Mat4 lightSpaceMatrix;
    uint32_t objectIndex;
};

int main() {
    // Set working directory to the parent of the executable (project root)
    // so that relative paths like "bin/shaders/..." and "bin/fonts/..." always work,
    // regardless of whether the exe is launched from bin/ or the project root.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        // exe is in <project>/bin/ — go one level up to project root
        std::filesystem::path projectRoot = exeDir.parent_path();
        if (std::filesystem::exists(projectRoot / "bin")) {
            std::filesystem::current_path(projectRoot);
        }
        // If already at project root (e.g. launched via VS Code debugger), do nothing
    }

    try {
        const uint32_t WIDTH = 1280;
        const uint32_t HEIGHT = 720;
        
        core::Window window("Vulkan Voxel Engine", WIDTH, HEIGHT);
        std::cout << "Window created." << std::endl;
        
        gfx::VulkanContext vulkanContext(window);
        std::cout << "VulkanContext created." << std::endl;
        
        gfx::Swapchain swapchain(vulkanContext, window);
        std::cout << "Swapchain created." << std::endl;
        
        gfx::BindlessSystem bindlessSystem(vulkanContext);
        std::cout << "BindlessSystem created." << std::endl;
        
        gfx::Renderer renderer(vulkanContext, swapchain, window, bindlessSystem);
        std::cout << "Renderer created." << std::endl;

        gfx::GeometryManager geometryManager(vulkanContext);
        std::cout << "GeometryManager created." << std::endl;
        
        gfx::Texture checkerTexture(vulkanContext, bindlessSystem);
        checkerTexture.createCheckerboard(256, 256);
        std::cout << "Checkerboard Texture created (ID=" << checkerTexture.getID() << ")." << std::endl;

        ui::TextRenderer textRenderer(vulkanContext, renderer);
        std::cout << "TextRenderer created." << std::endl;

        ui::ImGuiManager imguiManager(vulkanContext, window, swapchain);
        std::cout << "ImGuiManager created." << std::endl;

        float cameraSpeed = 5.0f; // Exposed to ImGui slider

        // --- Voxel World ---
        world::BlockRegistry::registerDefaults();
        world::World voxelWorld(geometryManager);
        voxelWorld.generateTestWorld();
        std::cout << "Voxel World created." << std::endl;


        std::string vertPath = "bin/shaders/simple.vert.spv";
        std::string fragPath = "bin/shaders/simple.frag.spv";
        std::string shadowVertPath = "bin/shaders/shadow.vert.spv";
        std::string shadowFragPath = "bin/shaders/shadow.frag.spv";
        
        if (!std::filesystem::exists(vertPath)) {
            vertPath = "shaders/simple.vert.spv";
            fragPath = "shaders/simple.frag.spv";
            shadowVertPath = "shaders/shadow.vert.spv";
            shadowFragPath = "shaders/shadow.frag.spv";
        }

        // HOT RELOADER
        core::ShaderHotReloader reloader;
        reloader.watch("shaders/simple.vert");
        reloader.watch("shaders/simple.frag");
        reloader.watch("shaders/shadow.vert");
        reloader.watch("shaders/shadow.frag"); // If exists
        reloader.start();

        // Main Pipeline
        gfx::PipelineConfig mainPipelineConfig{};
        mainPipelineConfig.colorAttachmentFormats = {swapchain.getImageFormat()};
        mainPipelineConfig.depthAttachmentFormat = swapchain.getDepthFormat();
        
        mainPipelineConfig.vertexShaderPath = vertPath;
        mainPipelineConfig.fragmentShaderPath = fragPath;
        mainPipelineConfig.enableDepthTest = true;
        mainPipelineConfig.cullMode = VK_CULL_MODE_BACK_BIT;
        mainPipelineConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // Standard mesh winding
        mainPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout()); // Set 0: Shadows
        mainPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout()); // Set 1: Bindless
        
        // Push Constants
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(PushConstants);
        
        mainPipelineConfig.pushConstantRanges.push_back(pushConstantRange);
        
        gfx::Pipeline mainPipeline(vulkanContext, mainPipelineConfig);

        // Shadow Pipeline
        gfx::PipelineConfig shadowPipelineConfig{};
        // Shadow pass is depth-only, so no color attachments
        shadowPipelineConfig.colorAttachmentFormats = {}; 
        shadowPipelineConfig.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT; 
        
        shadowPipelineConfig.pushConstantRanges.push_back(pushConstantRange);
        
        // Shadow pipeline needs Bindless Set (Set 1) for ObjectBuffer.
        // We also provide Set 0 to keep indices consistent, even if unused.
        shadowPipelineConfig.descriptorSetLayouts.push_back(renderer.getDescriptorSetLayout());
        shadowPipelineConfig.descriptorSetLayouts.push_back(bindlessSystem.getDescriptorSetLayout());

        shadowPipelineConfig.vertexShaderPath = shadowVertPath;
        shadowPipelineConfig.fragmentShaderPath = shadowFragPath;
        shadowPipelineConfig.enableDepthTest = true;
        shadowPipelineConfig.cullMode = VK_CULL_MODE_FRONT_BIT; // Render Back Faces to Shadow Map
        shadowPipelineConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowPipelineConfig.depthBiasEnable = false; // Disable HW Bias to avoid Peter Panning
        shadowPipelineConfig.depthBiasConstant = 0.0f;
        shadowPipelineConfig.depthBiasSlope = 0.0f;
        
        gfx::Pipeline shadowPipeline(vulkanContext, shadowPipelineConfig);
        
        scene::Camera camera({0.0f, 12.0f, 20.0f}, 60.0f, renderer.getAspectRatio());

        core::Timer timer;

        while (!window.shouldClose()) {
            timer.update();
            float dt = timer.getDeltaTime();

            core::InputManager::get().update();
            window.pollEvents();
            
            // Check for Hot Reload
            if (reloader.shouldReload()) {
                renderer.reloadShaders();
                reloader.ackReload();
            }

            if (window.shouldClose()) {
                break;
            }

            camera.setAspectRatio(renderer.getAspectRatio());

            // Only update camera when ImGui is not capturing mouse/keyboard
            if (!ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard)
                camera.update(dt);

            // --- ImGui frame begin (before any ImGui::* calls) ---
            imguiManager.beginFrame();

            // Debug Tools window
            {
                auto pos = camera.getPosition();
                ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(300, 160), ImGuiCond_Once);
                ImGui::Begin("Debug Tools");
                ImGui::Text("FPS:    %.1f", timer.getFPS());
                ImGui::Text("dt:     %.2f ms", timer.getDeltaTimeMs());
                ImGui::Separator();
                ImGui::Text("Camera: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
                ImGui::Text("Yaw: %.1f  Pitch: %.1f", camera.getYaw(), camera.getPitch());
                ImGui::Separator();
                ImGui::SliderFloat("Camera Speed", &cameraSpeed, 1.0f, 50.0f);
                ImGui::Separator();
                ImGui::Text("--- World Stats ---");
                ImGui::Text("Chunks:   %u", voxelWorld.getChunkCount());
                ImGui::Text("Vertices: %u", voxelWorld.getTotalVertices());
                ImGui::Text("Indices:  %u", voxelWorld.getTotalIndices());
                if (ImGui::Button("Regenerate World (Random)")) {
                    vkDeviceWaitIdle(vulkanContext.getDevice());
                    geometryManager.reset();
                    voxelWorld.getChunks()[0]->fillRandom();
                    voxelWorld.rebuildMeshes();
                }
                if (ImGui::Button("Regenerate World (Terrain)")) {
                    vkDeviceWaitIdle(vulkanContext.getDevice());
                    geometryManager.reset();
                    voxelWorld.generateTestWorld();
                }
                ImGui::End();
            }

            // Light Matrix
            core::math::Vec3 lightPos = {5.0f, 10.0f, 3.0f}; 
            core::math::Mat4 lightView = core::math::Mat4::lookAt(lightPos, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            core::math::Mat4 lightProj = core::math::Mat4::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 20.0f);
            core::math::Mat4 lightSpaceMatrix = lightProj * lightView;

            VkCommandBuffer commandBuffer = renderer.beginFrame();
            if (commandBuffer) {
                
                uint32_t currentFrame = renderer.getCurrentFrameIndex();

                // --- Shadow Pass (empty — no shadow casters for now) ---
                renderer.beginShadowPass(commandBuffer);
                renderer.endShadowPass(commandBuffer);

                // --- Main Pass ---
                renderer.beginMainPass(commandBuffer);
                mainPipeline.bind(commandBuffer);
                geometryManager.bind(commandBuffer);

                VkDescriptorSet descriptorSet = renderer.getDescriptorSet();
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    mainPipeline.getLayout(), 0, 1, &descriptorSet, 0, nullptr);
                bindlessSystem.bind(commandBuffer, mainPipeline.getLayout(), currentFrame, 1);

                core::math::Mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();

                // --- Voxel World ---
                {
                    // Upload identity model matrix for the world chunk (objectIndex = 0)
                    gfx::BindlessSystem::ObjectDataSSBO worldData{};
                    worldData.modelMatrix = core::math::Mat4::identity();
                    worldData.textureID   = 0; // vertex color only
                    bindlessSystem.updateObject(currentFrame, 0, worldData);

                    PushConstants pc{};
                    pc.viewProj         = viewProj;
                    pc.lightSpaceMatrix = lightSpaceMatrix;
                    pc.objectIndex      = 0;
                    vkCmdPushConstants(commandBuffer, mainPipeline.getLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &pc);
                    voxelWorld.render(commandBuffer);
                }

                // --- Text UI (SDF) ---
                // FPS is already shown in ImGui Debug Tools window — no need to duplicate here.
                // Keep TextRenderer active for future in-world labels / HUD elements.
                textRenderer.beginFrame(renderer.getCurrentFrameIndex());

                // --- ImGui render (inside active vkCmdBeginRendering block) ---
                imguiManager.render(commandBuffer);

                renderer.endMainPass(commandBuffer);
                renderer.endFrame(commandBuffer);
            }
        }
        
        vkDeviceWaitIdle(vulkanContext.getDevice());

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        system("pause"); // Keep console open
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
