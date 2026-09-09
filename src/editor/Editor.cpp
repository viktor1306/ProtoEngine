#include "editor/Editor.hpp"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace proto {
namespace {
constexpr auto hierarchyTitle = "Ієрархія###Hierarchy";
constexpr auto viewportTitle = "3D-вікно###Viewport";
constexpr auto inspectorTitle = "Властивості###Inspector";
constexpr auto filesTitle = "Файли проєкту###Files";
constexpr ImVec4 accent{0.33f, 0.80f, 0.70f, 1};
void muted(const char* text) {
    ImGui::TextDisabled("%s", text);
}
} // namespace
Editor::Editor(GLFWwindow* window, VulkanRenderer& renderer, const std::filesystem::path& settings, bool transient,
               bool m0)
    : window_(window), renderer_(renderer), m0_(m0), automatedInput_(transient) {
    if (!m0_)
        document_.newScene();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Each successfully initialized stage is torn down if a later stage fails.
    try {
        auto& io = ImGui::GetIO();
        // Diagnostic input is paced explicitly by frames. Consume its whole
        // batch before clearing external events on the next diagnostic frame.
        if (automatedInput_)
            io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigDpiScaleFonts = false; // Font AND spacing are scaled together below.
        if (transient)
            io.IniFilename = nullptr;
        else {
            ensureParent(settings);
            iniPath_ = utf8(settings.wstring());
            io.IniFilename = iniPath_.c_str();
        }
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.FontSizeBase = 17;
        style.WindowRounding = 0;
        style.ChildRounding = 3;
        style.FrameRounding = 4;
        style.GrabRounding = 3;
        style.TabRounding = 3;
        style.PopupRounding = 4;
        style.WindowPadding = {14, 12};
        style.FramePadding = {8, 5};
        style.ItemSpacing = {9, 9};
        style.WindowBorderSize = 0;
        style.TabBarBorderSize = 1;
        style.Colors[ImGuiCol_WindowBg] = {0.075f, 0.083f, 0.10f, 1};
        style.Colors[ImGuiCol_ChildBg] = {0.065f, 0.073f, 0.09f, 1};
        style.Colors[ImGuiCol_PopupBg] = {0.10f, 0.11f, 0.13f, 1};
        style.Colors[ImGuiCol_MenuBarBg] = {0.095f, 0.105f, 0.125f, 1};
        style.Colors[ImGuiCol_Header] = {0.15f, 0.25f, 0.25f, 1};
        style.Colors[ImGuiCol_HeaderHovered] = {0.18f, 0.33f, 0.32f, 1};
        style.Colors[ImGuiCol_HeaderActive] = {0.19f, 0.39f, 0.36f, 1};
        style.Colors[ImGuiCol_Tab] = {0.09f, 0.10f, 0.12f, 1};
        style.Colors[ImGuiCol_TabSelected] = {0.15f, 0.21f, 0.23f, 1};
        style.Colors[ImGuiCol_TabSelectedOverline] = accent;
        style.Colors[ImGuiCol_TabDimmedSelected] = {0.11f, 0.14f, 0.17f, 1};
        style.Colors[ImGuiCol_DockingPreview] = {0.33f, 0.80f, 0.70f, 0.5f};
        style.Colors[ImGuiCol_Separator] = {0.20f, 0.23f, 0.28f, 1};
        style.Colors[ImGuiCol_Text] = {0.87f, 0.90f, 0.94f, 1};
        style.Colors[ImGuiCol_TextDisabled] = {0.52f, 0.58f, 0.65f, 1};
        baseStyle_ = style;
        const auto path = utf8(windowsFont().wstring());
        auto* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 17);
        if (!font)
            throw std::runtime_error("Windows Segoe UI font could not be loaded");
        ukrainianGlyphs_ = true;
        for (const ImWchar code :
             std::array<ImWchar, 10>{0x0490, 0x0491, 0x0404, 0x0454, 0x0406, 0x0456, 0x0407, 0x0457, 0x0423, 0x043A})
            ukrainianGlyphs_ &= font->IsGlyphInFont(code);
        if (!ukrainianGlyphs_)
            throw std::runtime_error("Required Ukrainian glyphs missing from system font");
        platformInitialized_ = ImGui_ImplGlfw_InitForVulkan(window_, true);
        if (!platformInitialized_)
            throw std::runtime_error("ImGui GLFW backend initialization failed");
        const auto& platform = ImGui::GetPlatformIO();
        clipboardConnected_ = platform.Platform_GetClipboardTextFn && platform.Platform_SetClipboardTextFn;
        const auto& context = renderer_.context();
        pipelineFormat_ = context.surfaceFormat;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = context.instance;
        info.PhysicalDevice = context.physicalDevice;
        info.Device = context.device;
        info.QueueFamily = context.queueFamily;
        info.Queue = context.queue;
        info.MinImageCount = 2;
        info.ImageCount = context.imageCount;
        info.DescriptorPoolSize = 64;
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &pipelineFormat_;
        info.CheckVkResultFn = [](VkResult result) { vkCheck(result, "ImGui Vulkan backend"); };
        rendererInitialized_ = ImGui_ImplVulkan_Init(&info);
        if (!rendererInitialized_)
            throw std::runtime_error("ImGui Vulkan backend initialization failed");
    } catch (...) {
        if (rendererInitialized_)
            ImGui_ImplVulkan_Shutdown();
        if (platformInitialized_)
            ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw;
    }
}
Editor::~Editor() {
    shutdownPlay();
    // Renderer outlives Editor; descriptors and font images must be released first.
    vkDeviceWaitIdle(renderer_.context().device);
    if (rendererInitialized_) {
        for (const auto texture : textures_)
            if (texture)
                ImGui_ImplVulkan_RemoveTexture(texture);
        ImGui_ImplVulkan_Shutdown();
    }
    if (platformInitialized_)
        ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}
void Editor::applyScale(float scale) {
    if (std::abs(scale - appliedScale_) < 0.001f)
        return;
    auto& style = ImGui::GetStyle();
    style = baseStyle_;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    appliedScale_ = scale;
    ++scaleChanges_;
}
void Editor::makeLayout(ImGuiID dockspace, ImVec2 size) {
    // DockBuilder is isolated here because it is an ImGui internal API.
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID main = dockspace;
    const auto bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.25f, nullptr, &main);
    const auto left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.19f, nullptr, &main);
    const auto right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f, nullptr, &main);
    ImGui::DockBuilderDockWindow(hierarchyTitle, left);
    ImGui::DockBuilderDockWindow(viewportTitle, main);
    ImGui::DockBuilderDockWindow(inspectorTitle, right);
    ImGui::DockBuilderDockWindow(filesTitle, bottom);
    ImGui::DockBuilderDockWindow("Код C++###Code", bottom);
    ImGui::DockBuilderDockWindow("C++ / Player — журнал###BuildLog", bottom);
    ImGui::DockBuilderFinish(dockspace);
    resetLayout_ = false;
}
void Editor::hierarchy() {
    if (!m0_) {
        sceneHierarchy();
        return;
    }
    ImGui::Begin(hierarchyTitle);
    muted("Вбудована перевірка M0");
    ImGui::Separator();
    if (ImGui::Selectable("Трикутник", selected_))
        selected_ = true;
    ImGui::Spacing();
    ImGui::TextWrapped("Сценові об’єкти та ієрархія з’являться на етапі M1.");
    ImGui::End();
}
void Editor::inspector(double cpuFrameMs) {
    if (!m0_) {
        sceneInspector();
        return;
    }
    ImGui::Begin(inspectorTitle);
    ImGui::TextColored(accent, "Тестовий трикутник");
    muted("Вбудована геометрія");
    ImGui::SeparatorText("Рендеринг");
    ImGui::Text("API: Vulkan 1.3");
    ImGui::Text("Глибина: 0 .. 1");
    ImGui::Text("Передня грань: CCW");
    ImGui::Text("Кадрів у роботі: 2");
    ImGui::SeparatorText("Діагностика");
    ImGui::Text("CPU кадр: %.2f ms", cpuFrameMs);
    if (renderer_.hasGpuTiming())
        ImGui::Text("GPU: %.2f ms", renderer_.gpuMilliseconds());
    else
        muted("GPU timer: недоступний");
    ImGui::Text("VMA ресурси: %.1f MiB", static_cast<double>(allocationBytes_) / (1024 * 1024));
    ImGui::Text("Масштаб UI: %.0f%%", appliedScale_ * 100);
    ImGui::TextColored(renderer_.validationEnabled() ? accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s",
                       renderer_.validationEnabled() ? "Validation + sync: увімкнено" : "Release: validation вимкнено");
    ImGui::SeparatorText("Введення / Ctrl+C, Ctrl+V");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##Input", input_, sizeof(input_));
    muted("Українські символи: Ґ Є І Ї");
    ImGui::End();
}
void Editor::files() {
    if (!m0_) {
        projectFiles();
        return;
    }
    ImGui::Begin(filesTitle);
    muted("ПРОЄКТ / РЕСУРСИ");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Проєкт ще не відкрито");
    ImGui::TextWrapped(
        "Це каркас редактора M0. Створення проєктів, імпорт і файлові операції додаватимуться наступними етапами.");
    ImGui::Spacing();
    muted("Панелі можна переміщати за вкладку та змінювати їхній розмір.");
    ImGui::End();
}
VkExtent2D Editor::build(double cpuFrameMs, float userScale) {
    if (!m0_) {
        pollPlay();
        pollImport();
        if (!playBusy())
            pollProject();
    }
    float xScale{}, yScale{};
    glfwGetWindowContentScale(window_, &xScale, &yScale);
    applyScale(std::max(xScale, yScale) * userScale);
    const auto& context = renderer_.context();
    if (pipelineFormat_ != context.surfaceFormat) {
        pipelineFormat_ = context.surfaceFormat;
        ImGui_ImplVulkan_PipelineInfo pipeline{};
        pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        pipeline.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pipeline.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        pipeline.PipelineRenderingCreateInfo.pColorAttachmentFormats = &pipelineFormat_;
        ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
    }
    const auto frame = renderer_.frameIndex();
    if (generations_[frame] != renderer_.viewportGeneration()) {
        if (textures_[frame])
            ImGui_ImplVulkan_RemoveTexture(textures_[frame]);
        textures_[frame] =
            ImGui_ImplVulkan_AddTexture(renderer_.viewportView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        generations_[frame] = renderer_.viewportGeneration();
    }
    if (milliseconds(memorySample_) > 1000) {
        allocationBytes_ = renderer_.allocatedBytes();
        memorySample_ = Clock::now();
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    // Native diagnostics must not inherit mouse/keyboard activity from the
    // user working in another window. M1's explicit injected gestures follow.
    if (automatedInput_)
        ImGui::GetIO().ClearEventsQueue();
    if (testInput_) {
        auto input = std::move(testInput_);
        testInput_ = {};
        input();
    }
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(accent, "PROTO ENGINE");
        ImGui::Separator();
        if (ImGui::BeginMenu("Файл")) {
            ImGui::BeginDisabled(playBusy());
            if (!m0_) {
                if (ImGui::MenuItem("Новий проєкт…"))
                    request(Action::NewProject);
                if (ImGui::MenuItem("Відкрити проєкт…"))
                    request(Action::OpenProject);
                ImGui::Separator();
                if (ImGui::MenuItem("Нова сцена", "Ctrl+N"))
                    request(Action::New);
                if (ImGui::MenuItem("Відкрити сцену…", "Ctrl+O"))
                    request(Action::Open);
                if (ImGui::MenuItem("Зберегти", "Ctrl+S"))
                    saveScene();
                if (ImGui::MenuItem("Зберегти як…", "Ctrl+Shift+S"))
                    saveScene(true);
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Вийти", "Alt+F4"))
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Вигляд")) {
            ImGui::BeginDisabled(playBusy());
            if (ImGui::MenuItem("Відновити розташування панелей"))
                resetLayout_ = true;
            if (!m0_) {
                ImGui::MenuItem("Сітка", nullptr, &showGrid_);
                ImGui::MenuItem("Через камеру сцени", nullptr, &useSceneCamera_);
                ImGui::MenuItem("Студійний перегляд матеріалів", nullptr, &useStudioLighting_);
                if (ImGui::MenuItem("Налаштування освітлення…")) {
                    lightingDraft_ = document_.scene.lighting;
                    lightingDraftScene_ = document_.scene.id;
                    showLightingSettings_ = true;
                }
                if (ImGui::MenuItem("Фокус на об’єкті", "F"))
                    focusSelection();
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (!m0_)
            sceneMenu();
        if (!m0_)
            playToolbar();
        muted(m0_ ? "M0 · Основа редактора" : "Proto Engine · v0.1");
        ImGui::EndMainMenuBar();
    }
    const auto* viewport = ImGui::GetMainViewport();
    const auto dockspace = ImGui::GetID("ProtoDockspace");
    if (!ImGui::DockBuilderGetNode(dockspace) || resetLayout_)
        makeLayout(dockspace, viewport->WorkSize);
    ImGui::DockSpaceOverViewport(dockspace, viewport);
    ImGui::BeginDisabled(playBusy());
    hierarchy();
    inspector(cpuFrameMs);
    files();
    ImGui::EndDisabled();
    if (!m0_) {
        codePanel();
        buildLogPanel();
        behaviorStringProbe();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(viewportTitle, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto area = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(area.x, 1.0f), std::max(area.y, 1.0f));
    const auto imagePosition = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(textures_[frame])), size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 labelPos(imagePosition.x + 16 * appliedScale_, imagePosition.y + 13 * appliedScale_);
    if (m0_) {
        draw->AddText(labelPos, IM_COL32(133, 155, 177, 255), "VULKAN 1.3   /   ТЕСТОВИЙ КАДР");
        draw->AddText(ImVec2(labelPos.x, imagePosition.y + size.y - 30 * appliedScale_), IM_COL32(116, 139, 161, 255),
                      "Y ↑   ·   RGB   ·   Depth 0.25");
    } else if (!playBusy()) {
        viewportPosition_ = imagePosition;
        viewportSize_ = size;
        dropProjectAsset();
        prepareCameraView();
        transformGizmo(imagePosition, size);
        sceneViewport(imagePosition, size);
        lightGizmos(imagePosition, size);
        draw->AddText(ImVec2(labelPos.x, labelPos.y + 40 * appliedScale_), IM_COL32(153, 177, 194, 255),
                      (useStudioLighting_ || assetSmoke_) ? "СТУДІЙНИЙ ПЕРЕГЛЯД МАТЕРІАЛІВ"
                                                          : "ОСВІТЛЕННЯ СЦЕНИ   /   ДИНАМІЧНІ ТІНІ");
        draw->AddText(ImVec2(labelPos.x, imagePosition.y + size.y - 30 * appliedScale_), IM_COL32(137, 158, 176, 255),
                      "ПКМ: огляд   ·   СКМ: рух   ·   Колесо: масштаб   ·   F: фокус");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (!m0_) {
        if (showLightingSettings_ && !playBusy())
            lightingSettings();
        if (!playBusy())
            prepareSceneView();
        scenePopups();
        if (!playBusy())
            projectPopups();
    }
    const auto scale = ImGui::GetIO().DisplayFramebufferScale;
    ImGui::Render();
    return {static_cast<uint32_t>(std::max(size.x * scale.x, 1.0f)),
            static_cast<uint32_t>(std::max(size.y * scale.y, 1.0f))};
}
void Editor::record(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}
} // namespace proto
