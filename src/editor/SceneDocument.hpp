#pragma once
#include "scene/SceneIO.hpp"
#include "assets/AssetWorkspace.hpp"

#include <memory>
#include <string>
#include <filesystem>

namespace proto {
class FileTransaction;
struct DocumentAction {
    virtual ~DocumentAction() = default;
    virtual void apply(bool forward) = 0;
    virtual size_t bytes() const = 0;
    virtual std::string label() const = 0;
};

class SceneDocument {
  public:
    struct ProjectView {
        Scene scene;
        EntityId selection;
        std::filesystem::path path;
        std::string savedBytes;
        std::shared_ptr<AssetWorkspace> workspace;
    };

    Scene scene{Scene::demo()};
    EntityId selection;
    const std::filesystem::path& path() const { return path_; }
    bool dirty() const { return state_ != savedState_ || gestureChanged_; }
    bool gestureActive() const { return gesture_.has_value() || bool(materialGesture_); }
    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < history_.size(); }
    size_t undoCount() const { return cursor_; }
    size_t historyBytes() const { return historyBytes_; }
    void newScene(bool preserveFileHistory = false);
    void load(const std::filesystem::path& path);
    void loadProject(const std::filesystem::path& path, std::shared_ptr<AssetWorkspace> workspace,
                     bool preserveFileHistory = true);
    void save(const std::filesystem::path& path);
    EntityId create(EntityRecord record);
    void erase(EntityId id);
    void edit(EntityRecord record, std::string label = "Властивості");
    void reparent(EntityId child, EntityId parent, bool keepWorld = true);
    void setActiveCamera(EntityId id);
    void beginGesture(EntityId id);
    void preview(EntityRecord record);
    void endGesture();
    void cancelGesture();
    void executeExternal(std::shared_ptr<DocumentAction> action);
    void undo();
    void redo();
    void lighting(const LightingSettings& settings);
    std::shared_ptr<AssetWorkspace> workspace();
    EntityId instantiate(std::shared_ptr<const ModelBundle> model);
    void material(AssetId id, const MaterialValues& values, bool history = true);
    void beginMaterialGesture(AssetId id);
    void previewMaterial(AssetId id, const MaterialValues& values);
    void bindProjectWorkspace(std::shared_ptr<AssetWorkspace> workspace);
    ProjectView prepareProjectRelocation(const std::filesystem::path& newScenePath,
                                         std::shared_ptr<AssetWorkspace> workspace) const;
    void publishProjectView(ProjectView&& view) noexcept;
    void relocateProject(const std::filesystem::path& newScenePath, std::shared_ptr<AssetWorkspace> workspace);
    void refreshProjectAssets();
    void saveProject(const std::filesystem::path& path, const std::filesystem::path& projectRoot);
    void validateSavedFile() const;

  private:
    enum class Kind { Create, Erase, Edit, Camera, Material, Lighting, External };
    struct Command {
        Kind kind;
        std::vector<EntityRecord> before, after;
        EntityId cameraBefore, cameraAfter;
        uint64_t beforeState{}, afterState{};
        std::string label;
        AssetId materialId;
        MaterialValues materialBefore, materialAfter;
        std::optional<std::vector<AssetId>> sourcesBefore, sourcesAfter;
        LightingSettings lightingBefore, lightingAfter;
        std::shared_ptr<DocumentAction> external;
        std::shared_ptr<FileTransaction> materialTransaction;
        size_t bytes() const;
    };
    void commit(Command command);
    void apply(const Command& command, bool forward);
    void resetHistory(bool saved, bool preserveFileHistory = false);
    std::shared_ptr<AssetCatalog> loadProjectAssets(const std::shared_ptr<AssetWorkspace>& workspace,
                                                    const std::vector<AssetId>& sources) const;
    std::filesystem::path path_;
    std::string savedBytes_;
    std::vector<Command> history_;
    size_t cursor_{}, historyBytes_{};
    uint64_t nextState_{2}, state_{1}, savedState_{};
    std::optional<EntityRecord> gesture_;
    bool gestureChanged_{};
    std::shared_ptr<const MaterialAsset> materialGesture_;
    std::shared_ptr<AssetWorkspace> workspace_;
};
} // namespace proto
