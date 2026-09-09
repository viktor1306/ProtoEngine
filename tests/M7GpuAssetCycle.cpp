#include "benchmark/BenchmarkScenes.hpp"
#include "runtime/SceneViewBuilder.hpp"
#include "runtime/PlayerPresenter.hpp"
#include "renderer/AssetRenderer.hpp"
#include "scene/SceneIO.hpp"
#include <windows.h>
#include <psapi.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <iostream>
#include <sstream>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Window {
    GLFWwindow* handle{};
    Window() {
        vkCheck(volkInitialize(), "Vulkan loader");
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
        check(glfwInit(), "GLFW init failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        handle = glfwCreateWindow(800, 450, "M7 resource lifetime", nullptr, nullptr);
        check(handle != nullptr, "GPU lifetime window failed");
    }
    ~Window() { if (handle) glfwDestroyWindow(handle); glfwTerminate(); volkFinalize(); }
};
struct Memory {
    uint64_t working{}, privateBytes{};
};
Memory processMemory() {
    PROCESS_MEMORY_COUNTERS_EX data{};
    data.cb = sizeof(data);
    check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&data), sizeof(data)),
          "Process memory query failed");
    return {data.WorkingSetSize, data.PrivateUsage};
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2 || argc == 4, "usage: ProtoM7GpuAssetCycle output.json [--validation-dir PATH]");
        const auto output = std::filesystem::absolute(argv[1]);
        auto logPath = output;
        logPath.replace_extension(".log");
        Diagnostics log(logPath);
        const bool validation = argc == 4;
        if (validation) {
            check(std::wstring_view(argv[2]) == L"--validation-dir", "Unknown GPU lifecycle argument");
            check(SetEnvironmentVariableW(L"VK_LAYER_PATH", argv[3]), "Cannot set validation path");
        }
        std::ostringstream rows;
        uint64_t minimum = UINT64_MAX, maximum{}, peak{};
        size_t meshCapacity{}, imageCapacity{};
        {
            Window window;
            VulkanRenderer renderer(log);
            const auto shaders = executableDirectory() / "shaders";
            renderer.initialize(window.handle, shaders, validation);
            renderer.setVSync(false);
            PlayerPresenter presenter(renderer, shaders);
            struct Idle {
                VulkanRenderer& renderer;
                ~Idle() { try { renderer.waitIdle(); } catch (...) {} }
            } idle{renderer};
            RenderView view;
            const auto render = [&](BenchmarkScene& scene, unsigned frames) {
                unsigned done{}, attempts{};
                while (done < frames) {
                    check(++attempts < 10000 && !glfwWindowShouldClose(window.handle), "GPU lifecycle frame interrupted");
                    glfwPollEvents();
                    if (!renderer.prepare({640,360})) { glfwWaitEventsTimeout(.01); continue; }
                    buildRuntimeView(scene.scene, {640,360}, view);
                    applyPlayerGraphics(view, {640,360}, scene.graphics);
                    if (renderer.draw([&](VkCommandBuffer cmd) { presenter.record(renderer, cmd); }, {}, &view)) ++done;
                    check(renderer.assetRenderer()->error().empty(), "Asset upload failed during lifecycle test");
                }
            };
            for (unsigned cycle = 0; cycle < 12; ++cycle) {
                const auto loadStart = Clock::now();
                auto scene = makeBenchmarkScene("small");
                std::weak_ptr<const ModelBundle> model;
                std::weak_ptr<const MeshAsset> mesh;
                std::weak_ptr<const TexturePixels> pixels;
                {
                    const auto& source = scene.scene.assets->models.at(scene.scene.modelSources.front());
                    model = source;
                    mesh = source->meshes.front();
                    pixels = source->textures.front()->pixels;
                }
                render(scene, 4);
                check(renderer.assetRenderer()->pending() == 0, "Small assets did not finish uploading");
                renderer.waitIdle();
                const auto loadMs = milliseconds(loadStart);
                peak = std::max(peak, renderer.memoryStats().allocationBytes);
                const auto retireStart = Clock::now();
                scene = makeBenchmarkScene("empty");
                view = RenderView{};
                render(scene, 6);
                renderer.waitIdle();
                const auto retireMs = milliseconds(retireStart);
                check(model.expired() && mesh.expired() && pixels.expired(), "Unloaded assets retained an owner after all frame slots retired");
                const auto memory = renderer.memoryStats();
                const auto process = processMemory();
                if (cycle == 0) {
                    meshCapacity = renderer.assetRenderer()->meshCount();
                    imageCapacity = renderer.assetRenderer()->imageCount();
                } else {
                    check(renderer.assetRenderer()->meshCount() == meshCapacity &&
                              renderer.assetRenderer()->imageCount() == imageCapacity,
                          "GPU resource cache grows after each unloaded catalog");
                    minimum = std::min(minimum, memory.allocationBytes);
                    maximum = std::max(maximum, memory.allocationBytes);
                }
                if (cycle) rows << ',';
                rows << "{\"cycle\":" << cycle << ",\"owners_expired\":true,\"load_ms\":" << loadMs
                     << ",\"retire_ms\":" << retireMs << ",\"vma_bytes\":" << memory.allocationBytes
                     << ",\"working_set_bytes\":" << process.working << ",\"private_bytes\":" << process.privateBytes << '}';
            }
            check(maximum - minimum <= 65536, "VMA allocations did not plateau after the initial load/unload cycle");
        }
        check(log.validationErrors.load() == 0, "GPU lifecycle validation error");
        atomicWrite(output, "{\"passed\":true,\"cycles\":12,\"validation\":" + std::string(validation ? "true" : "false") +
                    ",\"validation_errors\":0,\"peak_loaded_vma_bytes\":" + std::to_string(peak) +
                    ",\"unloaded_min_vma_bytes\":" + std::to_string(minimum) +
                    ",\"unloaded_max_vma_bytes\":" + std::to_string(maximum) + ",\"samples\":[" + rows.str() + "]}");
        std::cout << "12 GPU asset load/unload cycles passed; tracked owners expired and allocations plateaued\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
