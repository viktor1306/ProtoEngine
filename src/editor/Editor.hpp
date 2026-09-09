#pragma once
#include "renderer/VulkanRenderer.hpp"
#include "editor/SceneDocument.hpp"
#include "editor/EditorCamera.hpp"
#include <imgui.h>
#include <array>

namespace proto {
struct LightingSmokeState;
struct GizmoState;
struct GizmoSmokeState;
struct ProjectUiState;
struct ProjectSmokeState;
struct PlayUiState;
struct PlaySmokeState;
struct ExportSmokeState;
struct CodeUiState;
struct BehaviorSchema;
class ProjectSession;
struct EditorBenchmarkState;
struct M8AcceptanceState;
class Editor {
  public:
    Editor(GLFWwindow* window, VulkanRenderer& renderer, const std::filesystem::path& settings, bool transient,
           bool m0 = false);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;
    VkExtent2D build(double cpuFrameMs, float userScale = 1);
    void record(VkCommandBuffer cmd);
    bool ukrainianGlyphs() const { return ukrainianGlyphs_; }
    bool clipboardConnected() const { return clipboardConnected_; }
    float appliedScale() const { return appliedScale_; }
    unsigned scaleChanges() const { return scaleChanges_; }
    const RenderView* renderView() const { return m0_ ? nullptr : &sceneView_; }
    SceneDocument& document() { return document_; }
    void requestClose();
    bool closeApproved() const { return closeApproved_; }
    void sceneSmokeStep(uint64_t frame, const std::filesystem::path& path);
    bool sceneSmokePassed() const { return sceneSmokePassed_; }
    bool selectionProbePassed() const { return uiProbe_.picked; }
    bool gestureProbePassed() const { return uiProbe_.dragged; }
    void startAssetSmoke(const std::filesystem::path& source, const std::filesystem::path& scene);
    void assetSmokeStep(uint64_t frame);
    bool assetSmokePassed() const { return assetSmokePassed_; }
    void startLightingSmoke(const std::filesystem::path& source, const std::filesystem::path& scene);
    void lightingSmokeStep(uint64_t frame);
    std::filesystem::path lightingSmokeCapture() const;
    void lightingSmokeCaptured();
    bool lightingSmokePassed() const;
    bool gizmoActive() const;
    void startGizmoSmoke();
    void gizmoSmokeStep(uint64_t frame);
    bool gizmoSmokePassed() const;
    void openProject(const std::filesystem::path& path);
    void createProject(const std::filesystem::path& path, const std::string& name);
    void startProjectSmoke(const std::filesystem::path& directory, const std::filesystem::path& model);
    void projectSmokeStep(uint64_t frame);
    bool projectSmokePassed() const;
    void pollPlay();
    bool playBusy() const;
    bool playerActive() const;
    void startPlaySmoke(const std::filesystem::path& root);
    void playSmokeStep(uint64_t frame);
    bool playSmokePassed() const;
    void startExportSmoke(const std::filesystem::path& root);
    void exportSmokeStep(uint64_t frame);
    bool exportSmokePassed() const;
    void startAcceptanceSmoke(const std::filesystem::path& output, const std::filesystem::path& fixtureRoot);
    void acceptanceSmokeStep(uint64_t frame);
    bool acceptanceSmokePassed() const;
    void prepareCapture() {
        if (!m0_)
            prepareSceneView(true);
    }
    void openScene(const std::filesystem::path& path) {
        document_.load(path);
        focusSelection();
    }
    void studioLighting(bool value) { useStudioLighting_ = value; }
    void viewSceneCamera(bool value) { useSceneCamera_ = value; }

    // M7 keeps the benchmark session inside the existing native editor.  The
    // transient session never opens a project or writes editor settings; the
    // hooks below only collect timing data and advance a deterministic camera
    // path when requested.
    void startBenchmark(const std::filesystem::path& output, double seconds, double warmupSeconds,
                        bool interactive);
    bool benchmarkActive() const;
    bool benchmarkFinished() const;
    void benchmarkBeginFrame();
    void benchmarkFrame(double wallMs, double uiMs, double submitMs, double frameWaitMs, VkExtent2D viewportExtent,
                        VkExtent2D swapExtent, bool rendered);
    void finishBenchmark(uint64_t rawFrames, uint64_t sceneFrames);

  private:
    enum class Action { None, New, Open, Close, NewProject, OpenProject, OpenFileScene };
    void request(Action action);
    void perform(Action action);
    bool saveScene(bool saveAs = false);
    void showError(const std::exception& e);
    void sceneMenu();
    void sceneHierarchy();
    void sceneNode(EntityHandle handle);
    void sceneInspector();
    void sceneFiles();
    void projectFiles();
    void projectPopups();
    void pollProject();
    void refreshProjectFiles(std::shared_ptr<ProjectSession> project = {}, ProjectUiState* target = nullptr);
    void acceptProject(std::shared_ptr<ProjectSession> candidate);
    void openProjectScene(const std::filesystem::path& path);
    void performProjectAction(Action action);
    void executeFilePlan(const struct FilePlan& plan);
    void operateFiles(int operation, const std::vector<std::string>& sources, const std::string& destination,
                      const std::optional<std::string>& newName = {});
    void navigateFiles(const std::string& directory);
    void dropProjectAsset();
    bool fileInputFocused() const;
    void behaviorInspector();
    void behaviorStringProbe();
    const BehaviorSchema* behaviorSchema() const;
    void requestBuild(bool play);
    void requestExport();
    void stopPlay();
    void shutdownPlay();
    void playToolbar();
    void buildLogPanel();
    void openCode(const std::filesystem::path& path, size_t line = 0);
    void codePanel();
    bool codeDirty() const;
    bool saveCode();
    bool codeInputFocused() const;
    void codeSmokeContract();
    std::shared_ptr<CodeUiState> prepareCodeRefresh(const std::vector<struct PathMove>& moves = {},
                                                    bool forward = true) const;
    bool codeSaving_{};
    void importModel();
    void pollImport();
    void materialInspector(const MeshRenderer& mesh);
    void addLight(bool point);
    void lightInspector(EntityRecord& record);
    void lightingSettings();
    void lightGizmos(ImVec2 position, ImVec2 size);
    void transformGizmo(ImVec2 position, ImVec2 size);
    void assetColorProbes();
    void lightingColorProbes();
    void scenePopups();
    void sceneViewport(ImVec2 position, ImVec2 size);
    void prepareSceneView(bool withProbes = false);
    void prepareCameraView();
    void addObject(int kind);
    void focusSelection();
    void reparentObject(EntityId child, EntityId parent);
    void propertyGesture(bool changed, const EntityRecord& record);
    void applyScale(float scale);
    void makeLayout(ImGuiID dockspace, ImVec2 size);
    void hierarchy();
    void inspector(double cpuFrameMs);
    void files();
    GLFWwindow* window_;
    VulkanRenderer& renderer_;
    ImGuiStyle baseStyle_;
    std::string iniPath_;
    std::array<VkDescriptorSet, VulkanRenderer::framesInFlight> textures_{};
    std::array<uint64_t, VulkanRenderer::framesInFlight> generations_{};
    VkFormat pipelineFormat_{};
    bool platformInitialized_{};
    bool rendererInitialized_{};
    bool ukrainianGlyphs_{};
    bool clipboardConnected_{};
    bool resetLayout_{};
    bool selected_{true};
    float appliedScale_{};
    unsigned scaleChanges_{};
    size_t allocationBytes_{};
    Clock::time_point memorySample_{};
    char input_[256] = "Україна · Ґґ Єє Іі Її";
    bool m0_{}, closeApproved_{}, showConfirm_{}, showError_{}, useSceneCamera_{}, showGrid_{true}, sceneSmokePassed_{};
    bool automatedInput_{};
    Action pending_{Action::None};
    // The session lock outlives both document journals and the import future.
    std::shared_ptr<ProjectSession> project_;
    SceneDocument document_;
    EditorCamera camera_;
    RenderView sceneView_;
    std::function<void()> queuedEdit_;
    std::function<void()> testInput_;
    ImVec2 viewportPosition_{}, viewportSize_{}, positionInputMin_{}, positionInputMax_{};
    struct UiProbe {
        EntityId id;
        float originalX{};
        size_t history{};
        ImVec2 mouse{};
        bool picked{}, dragged{};
        int stage{};
    } uiProbe_;
    std::string error_, status_, lastTitle_;
    std::optional<std::pair<EntityId, EntityId>> reparentFallback_;
    int smokeStage_{};
    ImportJob importJob_;
    AssetId importScene_;
    bool importInstance_{true};
    AssetId materialMesh_;
    int materialSlot_{};
    bool assetSmoke_{}, assetSmokePassed_{};
    int assetSmokeStage_{};
    uint64_t assetSmokeFrame_{};
    bool showLightingSettings_{}, useStudioLighting_{};
    LightingSettings lightingDraft_;
    AssetId lightingDraftScene_;
    std::shared_ptr<LightingSmokeState> lightingSmoke_;
    std::shared_ptr<GizmoState> gizmo_;
    bool gizmoInputConsumed_{};
    std::shared_ptr<GizmoSmokeState> gizmoSmoke_;
    std::shared_ptr<ProjectUiState> projectUi_;
    std::shared_ptr<ProjectSmokeState> projectSmoke_;
    std::shared_ptr<CodeUiState> codeUi_;
    std::shared_ptr<PlayUiState> playUi_;
    std::shared_ptr<PlaySmokeState> playSmoke_;
    std::shared_ptr<ExportSmokeState> exportSmoke_;
    std::shared_ptr<M8AcceptanceState> acceptanceSmoke_;
    ImVec2 exportButtonMin_{}, exportButtonMax_{};
    struct BehaviorStringProbeState {
        bool enabled{}, passed{};
        bool textQueued{};
        unsigned stage{};
        uint64_t frames{};
        std::string value;
        ImVec2 inputMin{}, inputMax{};
    } stringProbe_;
    struct CodePanelProbeState {
        bool enabled{}, passed{};
        bool textQueued{};
        unsigned stage{};
        uint64_t frames{};
        std::string original, edited, external;
        ImVec2 inputMin{}, inputMax{}, saveMin{}, saveMax{};
        bool inputActive{}, inputFocused{}, inputEdited{};
        ImGuiID inputId{};
    } codePanelProbe_;
    std::filesystem::path pendingScenePath_;
    std::shared_ptr<EditorBenchmarkState> benchmark_;
};
} // namespace proto
