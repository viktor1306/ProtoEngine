#include <iostream>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "core/Window.hpp"
#include "core/InputManager.hpp"
#include "gfx/VulkanContext.hpp"
#include "gfx/Swapchain.hpp"
#include "gfx/Renderer.hpp"
#include "gfx/Pipeline.hpp"
#include "gfx/BindlessSystem.hpp"
#include "gfx/GeometryManager.hpp"
#include "gfx/Texture.hpp"
#include "gfx/Mesh.hpp"
#include "scene/Camera.hpp"
#include "core/Math.hpp"
#include "ui/TextRenderer.hpp"
#include "core/ShaderHotReloader.hpp"

const std::vector<gfx::Vertex> cubeVertices = {
    // Front Face (Normal +Z)
    // V0 (BL) -> UV(0, 1)
    {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
    // V1 (BR) -> UV(1, 1)
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    // V2 (TR) -> UV(1, 0)
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    // V3 (TL) -> UV(0, 0)
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},

    // Back Face (Normal -Z)
    // Same pattern: BL(0,1), BR(1,1), TR(1,0), TL(0,0) relative to looking at face
    // Looking at Back: X goes Right to Left? No, View Space.
    // Let's standard map:
    // V4 (BR relative to front, BL looking at back): {0.5, -0.5, -0.5} -> UV(0, 1)
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
    // V5 (BL relative to front, BR looking at back): {-0.5, -0.5, -0.5} -> UV(1, 1)
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    // V6 (TL relative to front, TR looking at back): {-0.5, 0.5, -0.5} -> UV(1, 0)
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    // V7 (TR relative to front, TL looking at back): {0.5, 0.5, -0.5} -> UV(0, 0)
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},

    // Top Face (Normal +Y)
    // Map X to U, -Z to V? 
    // TL: {-0.5, 0.5, -0.5} -> 0,0
    // TR: {0.5, 0.5, -0.5} -> 1,0
    // BL: {-0.5, 0.5, 0.5} -> 0,1
    // BR: {0.5, 0.5, 0.5} -> 1,1
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}, // BL
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}, // BR
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}}, // TR
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}}, // TL

    // Bottom Face (Normal -Y)
    {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},

    // Right Face (Normal +X)
    {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},

    // Left Face (Normal -X)
    {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}}
};

const std::vector<uint32_t> cubeIndices = {
    0, 1, 2, 2, 3, 0,       // Front
    4, 5, 6, 6, 7, 4,       // Back
    8, 9, 10, 10, 11, 8,    // Top
    12, 13, 14, 14, 15, 12, // Bottom
    16, 17, 18, 18, 19, 16, // Right
    20, 21, 22, 22, 23, 20  // Left
};

const std::vector<gfx::Vertex> planeVertices = {
    {{-10.0f, 0.0f, -10.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f}},
    {{ 10.0f, 0.0f, -10.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {10.0f, 0.0f}},
    {{ 10.0f, 0.0f,  10.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {10.0f, 10.0f}},
    {{-10.0f, 0.0f,  10.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {0.0f, 10.0f}}
};

const std::vector<uint32_t> planeIndices = {
    0, 3, 2, 2, 1, 0
};

struct PushConstants {
    core::math::Mat4 viewProj;
    core::math::Mat4 lightSpaceMatrix;
    uint32_t objectIndex;
};

int main() {
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
        mainPipelineConfig.colorAttachmentFormats = {swapchain.getDateFormat()};
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
        
        // MESH LOADING via GeometryManager
        // Note: pointers returned, we should manage them (e.g. unique_ptr or just raw delete at end)
        std::unique_ptr<gfx::Mesh> cubeMesh(geometryManager.uploadMesh(cubeVertices, cubeIndices));
        std::unique_ptr<gfx::Mesh> planeMesh(geometryManager.uploadMesh(planeVertices, planeIndices));

        scene::Camera camera({0.0f, 2.0f, 5.0f}, 60.0f, renderer.getAspectRatio());


        // Simple time tracking
        auto startTime = std::chrono::high_resolution_clock::now();
        auto lastTime = startTime;

        while (!window.shouldClose()) {
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
            
            auto currentTime = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

            camera.setAspectRatio(renderer.getAspectRatio());
            camera.update(window, dt);

            // Light Matrix
            core::math::Vec3 lightPos = {5.0f, 10.0f, 3.0f}; 
            core::math::Mat4 lightView = core::math::Mat4::lookAt(lightPos, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            core::math::Mat4 lightProj = core::math::Mat4::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 20.0f);
            core::math::Mat4 lightSpaceMatrix = lightProj * lightView;

            VkCommandBuffer commandBuffer = renderer.beginFrame();
            if (commandBuffer) {
                
                // --- Shadow Pass ---
                renderer.beginShadowPass(commandBuffer);
                shadowPipeline.bind(commandBuffer);
                
                // BIND GLOBAL GEOMETRY
                geometryManager.bind(commandBuffer);

                uint32_t currentFrame = renderer.getCurrentFrameIndex();

                {
                    // Draw Cube (Shadow)
                    core::math::Mat4 model = core::math::Mat4::translate({0.0f, 1.0f, 0.0f});
                    model = model * core::math::Mat4::rotate(std::chrono::duration<float>(currentTime.time_since_epoch()).count(), {0.0f, 1.0f, 0.0f});

                    gfx::BindlessSystem::ObjectDataSSBO cubeData{};
                    cubeData.modelMatrix = model;
                    cubeData.textureID = checkerTexture.getID();
                    bindlessSystem.updateObject(currentFrame, 0, cubeData);

                    struct ShadowPC {
                        core::math::Mat4 lightSpaceMatrix;
                        uint32_t objectIndex;
                    } shadowPC;
                    shadowPC.lightSpaceMatrix = lightSpaceMatrix;
                    shadowPC.objectIndex = 0;
                    
                    vkCmdPushConstants(commandBuffer, shadowPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPC), &shadowPC);
                    
                    bindlessSystem.bind(commandBuffer, shadowPipeline.getLayout(), currentFrame, 1);
                    
                    cubeMesh->draw(commandBuffer);

                    // Draw Plane (Shadow)
                    model = core::math::Mat4::translate({0.0f, 0.0f, 0.0f});
                    
                    gfx::BindlessSystem::ObjectDataSSBO planeData{};
                    planeData.modelMatrix = model;
                    planeData.textureID = checkerTexture.getID(); 
                    bindlessSystem.updateObject(currentFrame, 1, planeData);

                    shadowPC.objectIndex = 1;
                    vkCmdPushConstants(commandBuffer, shadowPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPC), &shadowPC);
                    planeMesh->draw(commandBuffer);
                }
                renderer.endShadowPass(commandBuffer);

                // --- Main Pass ---
                renderer.beginMainPass(commandBuffer);
                mainPipeline.bind(commandBuffer);
                
                // BIND GLOBAL GEOMETRY
                geometryManager.bind(commandBuffer);

                VkDescriptorSet descriptorSet = renderer.getDescriptorSet();
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline.getLayout(), 0, 1, &descriptorSet, 0, nullptr);
                
                bindlessSystem.bind(commandBuffer, mainPipeline.getLayout(), currentFrame, 1);

                core::math::Mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
                
                // Draw Cube (Main)
                {
                    PushConstants pc{};
                    pc.viewProj = viewProj;
                    pc.lightSpaceMatrix = lightSpaceMatrix;
                    pc.objectIndex = 0; 
                    
                    vkCmdPushConstants(commandBuffer, mainPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
                    cubeMesh->draw(commandBuffer);
                }

                // Draw Plane (Main)
                {
                    PushConstants pc{};
                    pc.viewProj = viewProj;
                    pc.lightSpaceMatrix = lightSpaceMatrix;
                    pc.objectIndex = 1;

                    vkCmdPushConstants(commandBuffer, mainPipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
                    planeMesh->draw(commandBuffer);
                }
                
                // --- Text UI ---

                float white[] = {1.0f, 1.0f, 1.0f};
                
                static float fpsAccumulator = 0.0f;
                static int fpsCounter = 0;
                static float displayFps = 0.0f;
                
                fpsAccumulator += dt;
                fpsCounter++;
                
                if (fpsAccumulator >= 0.5f) { // Update every 0.5s
                    displayFps = fpsCounter / fpsAccumulator; // Approx FPS
                    // Or just use instant FPS averaged
                    displayFps = 1.0f / dt;
                    
                    fpsAccumulator = 0.0f;
                    fpsCounter = 0;
                }

                // Begin Frame for Text (resets offsets)
                textRenderer.beginFrame(renderer.getCurrentFrameIndex());

                std::stringstream ss;
                ss << "FPS: " << std::fixed << std::setprecision(1) << displayFps;
                textRenderer.renderText(commandBuffer, ss.str(), -0.95f, -0.90f, 0.05f, white);

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
