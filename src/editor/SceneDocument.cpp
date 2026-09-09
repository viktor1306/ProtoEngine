#include "editor/SceneDocument.hpp"
#include "behavior/BehaviorSchema.hpp"
#include <algorithm>
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "project/FileTransactions.hpp"

#include <yyjson.h>
#include <initializer_list>
#include <filesystem>
#include <numeric>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace proto {
namespace {
namespace fs = std::filesystem;

constexpr size_t maxExternalActionBytes = 64u * 1024u * 1024u;

bool samePath(const fs::path& left, const fs::path& right) {
    const auto a = fs::absolute(left).lexically_normal();
    const auto b = fs::absolute(right).lexically_normal();
    if (a == b)
        return true;
    std::error_code error;
    return fs::exists(a, error) && fs::exists(b, error) && fs::equivalent(a, b, error) && !error;
}

std::pair<fs::path, std::string> projectScenePath(const fs::path& projectRoot, const fs::path& input) {
    const auto root = fs::absolute(projectRoot).lexically_normal();
    const auto resolved = fs::absolute(input).lexically_normal();
    const auto relative = projectRelative(root, resolved);
    const auto key = projectPathKey(relative);
    if (!key.starts_with("SCENES/") || !key.ends_with(".SCENE.JSON"))
        throw std::runtime_error("Сцена проєкту повинна бути всередині папки Scenes і мати розширення .scene.json");
    return {resolved, relative};
}

std::string projectAssetRoot(const fs::path& projectRoot, const fs::path& scenePath) {
    const auto relative = fs::absolute(projectRoot).lexically_normal().lexically_relative(scenePath.parent_path());
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("Ресурси проєкту не можна представити відносним шляхом сцени");
    return utf8(relative.generic_wstring());
}

std::string sceneMetadata(const AssetId id) {
    return "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"assetId\":" + jsonString(id.string()) +
           ",\"kind\":\"Scene\"}\n";
}

AssetId sceneMetadataOwnerBytes(const std::string& bytes);

AssetId sceneMetadataOwner(const fs::path& path) {
    return sceneMetadataOwnerBytes(readDocument(path));
}

bool rootMatches(const fs::path& left, const fs::path& right) {
    return samePath(fs::absolute(left).lexically_normal(), fs::absolute(right).lexically_normal());
}

void strictKeys(Json* object, std::initializer_list<std::string_view> allowed, const char* context) {
    if (!object || !yyjson_is_obj(object))
        throw std::runtime_error(std::string(context) + ": очікувався JSON object");
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second || std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error(std::string(context) + ": непідтримуване або дубльоване поле");
    }
}

std::string requiredString(Json* object, const char* name, const char* context) {
    auto* value = yyjson_obj_get(object, name);
    if (!value || !yyjson_is_str(value))
        throw std::runtime_error(std::string(context) + ": очікувалося текстове поле " + name);
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

struct ProjectSceneHeader {
    AssetId id;
    std::string assetRoot;
    std::vector<AssetId> sources;
};

ProjectSceneHeader projectSceneHeader(const std::string& bytes) {
    JsonDoc document(bytes);
    auto* root = document.root();
    strictKeys(root,
               {"format", "formatVersion", "sceneId", "name", "activeCamera", "entities", "assetRoot", "modelSources",
                "lighting"},
               "Сцена проєкту");
    if (requiredString(root, "format", "Сцена проєкту") != "proto.scene")
        throw std::runtime_error("Це не файл proto.scene");
    auto* version = yyjson_obj_get(root, "formatVersion");
    if (!version || !yyjson_is_uint(version) || yyjson_get_uint(version) != 1)
        throw std::runtime_error("Непідтримувана версія сцени");
    ProjectSceneHeader result;
    result.id = AssetId::parse(requiredString(root, "sceneId", "Сцена проєкту"));
    if (auto* assetRoot = yyjson_obj_get(root, "assetRoot")) {
        if (!yyjson_is_str(assetRoot))
            throw std::runtime_error("assetRoot сцени має бути текстом");
        result.assetRoot = {yyjson_get_str(assetRoot), yyjson_get_len(assetRoot)};
    }
    if (auto* sources = yyjson_obj_get(root, "modelSources")) {
        if (!yyjson_is_arr(sources) || yyjson_arr_size(sources) > 4096)
            throw std::runtime_error("Некоректний список modelSources");
        size_t index{}, count{};
        Json* value{};
        yyjson_arr_foreach(sources, index, count, value) {
            if (!yyjson_is_str(value))
                throw std::runtime_error("modelSources має містити AssetId");
            const auto id = AssetId::parse({yyjson_get_str(value), yyjson_get_len(value)});
            if (std::find(result.sources.begin(), result.sources.end(), id) != result.sources.end())
                throw std::runtime_error("Дубльований model source");
            result.sources.push_back(id);
        }
    }
    return result;
}

AssetId sceneMetadataOwnerBytes(const std::string& bytes) {
    JsonDoc document(bytes);
    auto* root = document.root();
    strictKeys(root, {"format", "formatVersion", "assetId", "kind"}, "Sidecar сцени");
    if (requiredString(root, "format", "Sidecar сцени") != "proto.assetmeta")
        throw std::runtime_error("Некоректний sidecar сцени");
    auto* version = yyjson_obj_get(root, "formatVersion");
    if (!version || !yyjson_is_uint(version) || yyjson_get_uint(version) != 1)
        throw std::runtime_error("Некоректна версія sidecar сцени");
    if (requiredString(root, "kind", "Sidecar сцени") != "Scene")
        throw std::runtime_error("Некоректний тип sidecar сцени");
    return AssetId::parse(requiredString(root, "assetId", "Sidecar сцени"));
}

std::string normalizedProjectAssetRoot(const fs::path& projectRoot, const fs::path& scenePath,
                                       const ProjectSceneHeader& header) {
    if (header.sources.empty())
        return {};
    if (header.assetRoot.empty())
        throw std::runtime_error("Сцена з ресурсами не має assetRoot");
    const auto relative = utf8Path(header.assetRoot);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        throw std::runtime_error("assetRoot сцени має бути відносним");
    const auto resolved = (scenePath.parent_path() / relative).lexically_normal();
    if (!rootMatches(resolved, projectRoot))
        throw std::runtime_error("assetRoot сцени не відповідає кореню проєкту");
    return projectAssetRoot(projectRoot, scenePath);
}

std::shared_ptr<FileTransaction> prepareMaterialTransaction(const std::shared_ptr<AssetWorkspace>& workspace,
                                                            const PreparedMaterialEdit& prepared) {
    if (!workspace)
        throw std::runtime_error("Не вибрано робочу папку матеріалу");
    FilePlan plan;
    plan.label = "Material";
    plan.edits = {{prepared.path, FileKind::File, prepared.after, {}}};
    plan.guards = {
        {prepared.path, {FileKind::File, static_cast<uint64_t>(prepared.before.size()), sha256(prepared.before)}}};
    auto transaction = FileTransaction::prepare(workspace->root(), plan);
    transaction->redo();
    return transaction;
}

void validateMaterialEdit(const MaterialValues& original, const MaterialValues& changed) {
    validateMaterial(changed);
    auto allowed = original;
    allowed.baseColor = glm::vec4(glm::vec3(changed.baseColor), original.baseColor.a);
    allowed.metallic = changed.metallic;
    allowed.roughness = changed.roughness;
    allowed.normalScale = changed.normalScale;
    if (allowed != changed)
        throw std::runtime_error("Змініть текстури, alpha та інші параметри у вихідній моделі й оновіть імпорт");
}
} // namespace
size_t SceneDocument::Command::bytes() const {
    size_t total = sizeof(Command) + label.size() + (before.size() + after.size()) * sizeof(EntityRecord);
    for (const auto& r : before)
        total += r.name.size() + (r.mesh ? r.mesh->materials.size() * sizeof(AssetId) : 0) +
                 std::accumulate(r.behaviors.begin(), r.behaviors.end(), size_t{},
                                 [](size_t bytes, const auto& behavior) { return bytes + behaviorBytes(behavior); });
    for (const auto& r : after)
        total += r.name.size() + (r.mesh ? r.mesh->materials.size() * sizeof(AssetId) : 0) +
                 std::accumulate(r.behaviors.begin(), r.behaviors.end(), size_t{},
                                 [](size_t bytes, const auto& behavior) { return bytes + behaviorBytes(behavior); });
    total +=
        ((sourcesBefore ? sourcesBefore->size() : 0) + (sourcesAfter ? sourcesAfter->size() : 0)) * sizeof(AssetId);
    if (external)
        total += external->bytes();
    if (materialTransaction)
        total += materialTransaction->memoryBytes();
    return total;
}
void SceneDocument::resetHistory(bool saved, bool preserveFileHistory) {
    std::vector<Command> oldHistory;
    size_t oldCursor = 0;
    if (preserveFileHistory) {
        oldHistory = std::move(history_);
        oldCursor = cursor_;
    } else {
        history_.clear();
    }
    history_.clear();
    cursor_ = historyBytes_ = 0;
    gesture_.reset();
    materialGesture_.reset();
    gestureChanged_ = false;
    state_ = nextState_++;
    savedState_ = saved ? state_ : 0;
    if (preserveFileHistory) {
        for (size_t i = 0; i < oldHistory.size(); ++i) {
            const bool fileHistory = oldHistory[i].kind == Kind::External ||
                                     (oldHistory[i].kind == Kind::Material && oldHistory[i].materialTransaction);
            if (!fileHistory)
                continue;
            oldHistory[i].beforeState = state_;
            oldHistory[i].afterState = state_;
            historyBytes_ += oldHistory[i].bytes();
            history_.push_back(std::move(oldHistory[i]));
            if (i < oldCursor)
                ++cursor_;
        }
    }
    selection = {};
    for (const auto h : scene.meshes())
        if (scene.mesh(h)->mesh == builtin::cube) {
            selection = scene.entity(h).id;
            break;
        }
}
void SceneDocument::newScene(bool preserveFileHistory) {
    scene = Scene::demo();
    EntityRecord sun;
    sun.id = EntityId::create();
    sun.name = "Сонце";
    sun.transform.position = {0, 4, 0};
    sun.transform.rotation = glm::quatLookAt(glm::normalize(glm::vec3(-.4f, -.7f, -1)), glm::vec3(0, 1, 0));
    sun.directionalLight = DirectionalLight{};
    scene.create(sun);
    scene.update();
    workspace_.reset();
    path_.clear();
    savedBytes_.clear();
    resetHistory(false, preserveFileHistory);
}
void SceneDocument::load(const std::filesystem::path& path) {
    auto bytes = readDocument(path);
    JsonDoc header(bytes);
    std::shared_ptr<AssetWorkspace> workspace;
    auto catalog = std::make_shared<AssetCatalog>();
    if (auto* sources = yyjson_obj_get(header.root(), "modelSources")) {
        if (!yyjson_is_arr(sources) || yyjson_arr_size(sources) > 4096)
            throw std::runtime_error("Invalid model source list");
        const auto relative = utf8Path(str(get(header.root(), "assetRoot")));
        if (relative.empty() || relative.is_absolute() || relative.has_root_name())
            throw std::runtime_error("Asset root must be relative");
        workspace = std::make_shared<AssetWorkspace>(path.parent_path() / relative);
        size_t i, n;
        Json* value;
        yyjson_arr_foreach(sources, i, n, value) {
            const auto id = AssetId::parse(str(value));
            if (catalog->models.contains(id))
                throw std::runtime_error("Duplicate model source");
            catalog->publish(workspace->load(id));
        }
    }
    auto candidate = decodeScene(bytes, catalog);
    const auto resolved = std::filesystem::absolute(path).lexically_normal();
    scene = std::move(candidate);
    workspace_ = std::move(workspace);
    path_ = resolved;
    savedBytes_ = std::move(bytes);
    resetHistory(true);
}
void SceneDocument::loadProject(const std::filesystem::path& path, std::shared_ptr<AssetWorkspace> workspace,
                                bool preserveFileHistory) {
    if (!workspace)
        throw std::runtime_error("Не можна відкрити сцену без робочої папки проєкту");
    const auto [resolved, relative] = projectScenePath(workspace->root(), path);
    (void)relative;
    if (!fs::exists(resolved) || !fs::is_regular_file(resolved))
        throw std::runtime_error("Файл сцени проєкту не знайдено");
    const auto sidecarPath = projectPath(workspace->root(), relative + ".meta");
    if (!fs::exists(sidecarPath) || !fs::is_regular_file(sidecarPath))
        throw std::runtime_error("Sidecar сцени проєкту не знайдено");

    // Read and validate both authored files before touching the project
    // workspace.  In particular, do not let a scene-controlled assetRoot
    // redirect AssetWorkspace::load outside the project boundary.
    const auto bytes = readDocument(resolved);
    const auto metadataBytes = readDocument(sidecarPath);
    const auto header = projectSceneHeader(bytes);
    if (sceneMetadataOwnerBytes(metadataBytes) != header.id)
        throw std::runtime_error("SceneId сцени не збігається із sidecar");
    const auto assetRoot = normalizedProjectAssetRoot(workspace->root(), resolved, header);
    auto catalog = loadProjectAssets(workspace, header.sources);
    auto candidate = decodeScene(bytes, catalog);
    if (candidate.id != header.id || candidate.modelSources != header.sources)
        throw std::runtime_error("Сцена змінилася під час підготовки");
    auto snapshot = candidate.snapshot();
    snapshot.assetRoot = assetRoot;
    candidate = Scene::fromSnapshot(snapshot, catalog);

    endGesture();
    scene = std::move(candidate);
    workspace_ = std::move(workspace);
    path_ = resolved;
    savedBytes_ = bytes;
    resetHistory(true, preserveFileHistory);
}
void SceneDocument::save(const std::filesystem::path& path) {
    endGesture();
    const auto resolved = std::filesystem::absolute(path).lexically_normal();
    bool same = !path_.empty() && resolved == path_;
    if (!same && !path_.empty() && std::filesystem::exists(resolved) && std::filesystem::exists(path_))
        same = std::filesystem::equivalent(resolved, path_);
    auto writtenId = scene.id;
    std::string bytes;
    auto writtenRoot = scene.assetRoot;
    if (workspace_ && !scene.modelSources.empty()) {
        auto relative = workspace_->root().lexically_relative(resolved.parent_path());
        if (relative.empty() || relative.is_absolute() || relative.has_root_name())
            throw std::runtime_error("Save scenes on the same drive as their resources");
        writtenRoot = utf8(relative.generic_wstring());
    }
    if (!path_.empty() && !same) {
        auto snapshot = scene.snapshot();
        snapshot.id = AssetId::create();
        snapshot.assetRoot = writtenRoot;
        writtenId = snapshot.id;
        const auto copy = Scene::fromSnapshot(snapshot, scene.assets);
        bytes = encodeScene(copy);
    } else {
        auto snapshot = scene.snapshot();
        snapshot.assetRoot = writtenRoot;
        auto copy = Scene::fromSnapshot(snapshot, scene.assets);
        bytes = encodeScene(copy);
    }
    atomicWrite(resolved, bytes, same ? std::optional<std::string_view>(savedBytes_) : std::nullopt);
    scene.id = writtenId;
    scene.assetRoot = writtenRoot;
    path_ = resolved;
    savedBytes_ = std::move(bytes);
    savedState_ = state_;
}
void SceneDocument::commit(Command command) {
    while (history_.size() > cursor_) {
        historyBytes_ -= history_.back().bytes();
        history_.pop_back();
    }
    command.beforeState = state_;
    command.afterState = command.kind == Kind::External ? state_ : nextState_++;
    state_ = command.afterState;
    historyBytes_ += command.bytes();
    history_.push_back(std::move(command));
    cursor_ = history_.size();
    while (history_.size() > 1 && (historyBytes_ > 64 * 1024 * 1024 || history_.size() > 512)) {
        historyBytes_ -= history_.front().bytes();
        history_.erase(history_.begin());
        --cursor_;
    }
}
EntityId SceneDocument::create(EntityRecord record) {
    endGesture();
    if (!record.id)
        record.id = EntityId::create();
    const auto handle = scene.create(record);
    try {
        scene.update();
    } catch (...) {
        scene.eraseSubtree(handle);
        scene.update();
        throw;
    }
    selection = record.id;
    commit({Kind::Create, {}, {record}, scene.activeCamera, scene.activeCamera, 0, 0, "Створення об’єкта"});
    return record.id;
}
void SceneDocument::erase(EntityId id) {
    endGesture();
    const auto h = scene.find(id);
    if (!h)
        return;
    Command command{Kind::Erase, scene.subtree(h), {}, scene.activeCamera, {}, 0, 0, "Видалення піддерева"};
    if (command.bytes() > 64 * 1024 * 1024)
        throw std::runtime_error("Команда перевищує 64 MiB історії. Видаліть піддерево частинами.");
    scene.eraseSubtree(h);
    scene.update();
    command.cameraAfter = scene.activeCamera;
    if (!scene.find(selection))
        selection = {};
    commit(std::move(command));
}
void SceneDocument::edit(EntityRecord record, std::string label) {
    endGesture();
    auto before = scene.record(scene.find(record.id));
    const auto camera = scene.activeCamera;
    scene.edit(record);
    const auto after = scene.record(scene.find(record.id));
    if (before == after && camera == scene.activeCamera)
        return;
    commit({Kind::Edit, {before}, {after}, camera, scene.activeCamera, 0, 0, std::move(label)});
}
void SceneDocument::reparent(EntityId child, EntityId parent, bool keepWorld) {
    endGesture();
    const auto h = scene.find(child);
    const auto p = scene.find(parent);
    if (!h || (parent && !p))
        throw std::runtime_error("Невідомий EntityId для переприв’язування");
    if (scene.entity(h).parent == p)
        return;
    const auto before = scene.record(h);
    scene.reparent(h, p, keepWorld);
    const auto after = scene.record(h);
    commit({Kind::Edit, {before}, {after}, scene.activeCamera, scene.activeCamera, 0, 0, "Зміна батька"});
}
void SceneDocument::setActiveCamera(EntityId id) {
    endGesture();
    if (!scene.find(id) || !scene.camera(scene.find(id)))
        throw std::runtime_error("Об’єкт не є камерою");
    const auto before = scene.activeCamera;
    if (before == id)
        return;
    scene.activeCamera = id;
    commit({Kind::Camera, {}, {}, before, id, 0, 0, "Активна камера"});
}
void SceneDocument::beginGesture(EntityId id) {
    if (gesture_ && gesture_->id == id)
        return;
    endGesture();
    gesture_ = scene.record(scene.find(id));
    gestureChanged_ = false;
}
void SceneDocument::preview(EntityRecord record) {
    if (!gesture_ || gesture_->id != record.id)
        beginGesture(record.id);
    scene.edit(record);
    gestureChanged_ = true;
}
void SceneDocument::endGesture() {
    if (materialGesture_) {
        const auto before = std::move(materialGesture_);
        const auto after = scene.assets->materials.at(before->id);
        gestureChanged_ = false;
        if (before->values != after->values) {
            std::shared_ptr<FileTransaction> transaction;
            try {
                auto workspace = this->workspace();
                const auto prepared = workspace->prepareMaterial(*after);
                transaction = prepareMaterialTransaction(workspace, prepared);
                Command command{Kind::Material, {}, {}, scene.activeCamera, scene.activeCamera, 0, 0, "Матеріал"};
                command.materialId = before->id;
                command.materialBefore = before->values;
                command.materialAfter = after->values;
                command.materialTransaction = transaction;
                commit(std::move(command));
            } catch (...) {
                if (transaction)
                    try {
                        transaction->undo();
                    } catch (...) {
                    }
                scene.assets->materials[before->id] = before;
                throw;
            }
        }
    }
    if (gesture_ && gestureChanged_) {
        const auto after = scene.record(scene.find(gesture_->id));
        if (*gesture_ != after)
            commit(
                {Kind::Edit, {*gesture_}, {after}, scene.activeCamera, scene.activeCamera, 0, 0, "Зміна властивостей"});
    }
    gesture_.reset();
    gestureChanged_ = false;
}
void SceneDocument::cancelGesture() {
    if (materialGesture_) {
        scene.assets->materials[materialGesture_->id] = materialGesture_;
        materialGesture_.reset();
    }
    if (gesture_) {
        const auto handle = scene.find(gesture_->id);
        if (!handle)
            throw std::runtime_error("Об’єкт жесту більше не існує");
        scene.edit(*gesture_);
        gesture_.reset();
    }
    gestureChanged_ = false;
}
void SceneDocument::executeExternal(std::shared_ptr<DocumentAction> action) {
    if (!action)
        throw std::runtime_error("Порожня зовнішня команда");
    endGesture();
    const auto bytes = action->bytes();
    if (bytes > maxExternalActionBytes)
        throw std::runtime_error("Зовнішня команда перевищує 64 MiB історії");
    Command command;
    command.kind = Kind::External;
    command.cameraBefore = scene.activeCamera;
    command.cameraAfter = scene.activeCamera;
    command.label = action->label();
    command.external = std::move(action);
    command.external->apply(true);
    commit(std::move(command));
}
void SceneDocument::apply(const Command& c, bool forward) {
    if (c.kind == Kind::External) {
        if (!c.external)
            throw std::runtime_error("Пошкоджена зовнішня команда");
        c.external->apply(forward);
        state_ = forward ? c.afterState : c.beforeState;
        return;
    }
    if (c.kind == Kind::Material) {
        if (c.materialTransaction) {
            if (forward)
                c.materialTransaction->redo();
            else
                c.materialTransaction->undo();
            if (scene.assets && scene.assets->materials.contains(c.materialId)) {
                auto changed = std::make_shared<MaterialAsset>(*scene.assets->materials.at(c.materialId));
                changed->values = forward ? c.materialAfter : c.materialBefore;
                ++changed->revision;
                scene.assets->materials[c.materialId] = std::move(changed);
            }
        } else {
            material(c.materialId, forward ? c.materialAfter : c.materialBefore, false);
        }
        state_ = forward ? c.afterState : c.beforeState;
        return;
    }
    const auto& sources = forward ? c.sourcesAfter : c.sourcesBefore;
    std::shared_ptr<AssetCatalog> sourceAssets;
    if (sources) {
        sourceAssets = loadProjectAssets(workspace_, *sources);
        scene.assets = sourceAssets;
    }
    const bool create = c.kind == Kind::Create ? forward : c.kind == Kind::Erase ? !forward : false;
    if (c.kind == Kind::Create || c.kind == Kind::Erase) {
        const auto& records = c.kind == Kind::Create ? c.after : c.before;
        if (create)
            scene.restore(records);
        else
            scene.eraseSubtree(scene.find(records.front().id));
    } else if (c.kind == Kind::Edit)
        scene.edit((forward ? c.after : c.before).front());
    else if (c.kind == Kind::Lighting)
        scene.lighting = forward ? c.lightingAfter : c.lightingBefore;
    scene.activeCamera = forward ? c.cameraAfter : c.cameraBefore;
    scene.update();
    if (sources)
        scene.modelSources = *sources;
    state_ = forward ? c.afterState : c.beforeState;
    if (!scene.find(selection))
        selection = {};
}
void SceneDocument::undo() {
    endGesture();
    if (canUndo()) {
        apply(history_[cursor_ - 1], false);
        --cursor_;
    }
}
void SceneDocument::redo() {
    endGesture();
    if (canRedo()) {
        apply(history_[cursor_], true);
        ++cursor_;
    }
}
void SceneDocument::lighting(const LightingSettings& settings) {
    validateLighting(settings);
    endGesture();
    const auto before = scene.lighting;
    if (before == settings)
        return;
    scene.lighting = settings;
    Command command;
    command.kind = Kind::Lighting;
    command.cameraBefore = scene.activeCamera;
    command.cameraAfter = scene.activeCamera;
    command.lightingBefore = before;
    command.lightingAfter = settings;
    command.label = "Параметри освітлення";
    commit(std::move(command));
}
std::shared_ptr<AssetCatalog> SceneDocument::loadProjectAssets(const std::shared_ptr<AssetWorkspace>& workspace,
                                                               const std::vector<AssetId>& sources) const {
    if (!workspace)
        throw std::runtime_error("Не вибрано робочу папку проєкту");
    auto catalog = std::make_shared<AssetCatalog>();
    for (const auto id : sources) {
        const auto model = workspace->load(id);
        if (!model || model->id != id)
            throw std::runtime_error("Модель проєкту має неправильний AssetId");
        catalog->publish(model);
    }
    return catalog;
}
void SceneDocument::bindProjectWorkspace(std::shared_ptr<AssetWorkspace> workspace) {
    if (!workspace)
        throw std::runtime_error("Не можна прив’язати порожню робочу папку");
    if (!scene.modelSources.empty() && !path_.empty() && scene.assetRoot.empty())
        throw std::runtime_error("Сцена з ресурсами не має assetRoot");
    if (!scene.modelSources.empty() && !path_.empty()) {
        const auto expected = fs::absolute(path_.parent_path() / utf8Path(scene.assetRoot)).lexically_normal();
        if (!rootMatches(expected, workspace->root()))
            throw std::runtime_error("Робоча папка не відповідає assetRoot сцени");
    }
    auto catalog = loadProjectAssets(workspace, scene.modelSources);
    scene.assets = std::move(catalog);
    scene.assetsChanged();
    workspace_ = std::move(workspace);
    if (!scene.modelSources.empty() && !path_.empty())
        scene.assetRoot = utf8(workspace_->root().lexically_relative(path_.parent_path()).generic_wstring());
}
SceneDocument::ProjectView SceneDocument::prepareProjectRelocation(const std::filesystem::path& newScenePath,
                                                                   std::shared_ptr<AssetWorkspace> workspace) const {
    if (!workspace)
        throw std::runtime_error("Не можна перемістити сцену без робочої папки");
    if (newScenePath.empty()) {
        auto catalog = loadProjectAssets(workspace, scene.modelSources);
        auto snapshot = scene.snapshot();
        auto candidate = Scene::fromSnapshot(snapshot, catalog);
        return {std::move(candidate), selection, {}, {}, std::move(workspace)};
    }
    const auto [resolved, relative] = projectScenePath(workspace->root(), newScenePath);
    (void)relative;
    if (!fs::exists(resolved) || !fs::is_regular_file(resolved))
        throw std::runtime_error("Переміщений файл сцени не знайдено");
    const auto sidecarPath = projectPath(workspace->root(), relative + ".meta");
    if (!fs::exists(sidecarPath) || !fs::is_regular_file(sidecarPath))
        throw std::runtime_error("Sidecar переміщеної сцени не знайдено");
    const auto diskBytes = readDocument(resolved);
    const auto metadataBytes = readDocument(sidecarPath);
    const auto header = projectSceneHeader(diskBytes);
    if (sceneMetadataOwnerBytes(metadataBytes) != header.id)
        throw std::runtime_error("SceneId сцени не збігається із sidecar");
    if (header.id != scene.id)
        throw std::runtime_error("Переміщений файл має інший SceneId");
    (void)normalizedProjectAssetRoot(workspace->root(), resolved, header);
    auto sourceIds = scene.modelSources;
    for (const auto id : header.sources)
        if (std::find(sourceIds.begin(), sourceIds.end(), id) == sourceIds.end())
            sourceIds.push_back(id);
    auto catalog = loadProjectAssets(workspace, sourceIds);
    auto disk = decodeScene(diskBytes, catalog);
    if (disk.id != scene.id)
        throw std::runtime_error("Переміщений файл не відповідає поточній сцені");
    auto snapshot = scene.snapshot();
    snapshot.assetRoot = scene.modelSources.empty() ? std::string{} : projectAssetRoot(workspace->root(), resolved);
    auto candidate = Scene::fromSnapshot(snapshot, catalog);
    if (selection && !candidate.find(selection))
        throw std::runtime_error("Переміщений файл втрачає поточний selection");
    return {std::move(candidate), selection, resolved, diskBytes, std::move(workspace)};
}
void SceneDocument::publishProjectView(ProjectView&& view) noexcept {
    scene = std::move(view.scene);
    selection = view.selection;
    path_ = std::move(view.path);
    savedBytes_ = std::move(view.savedBytes);
    workspace_ = std::move(view.workspace);
}
void SceneDocument::relocateProject(const std::filesystem::path& newScenePath,
                                    std::shared_ptr<AssetWorkspace> workspace) {
    auto view = prepareProjectRelocation(newScenePath, std::move(workspace));
    publishProjectView(std::move(view));
}
void SceneDocument::refreshProjectAssets() {
    if (gestureActive())
        return;
    if (!workspace_)
        throw std::runtime_error("Не вибрано робочу папку проєкту");
    auto catalog = loadProjectAssets(workspace_, scene.modelSources);
    auto snapshot = scene.snapshot();
    auto candidate = Scene::fromSnapshot(snapshot, catalog);
    scene.assets = std::move(candidate.assets);
    scene.assetsChanged();
}
void SceneDocument::validateSavedFile() const {
    if (path_.empty())
        return;
    if (!fs::exists(path_) || !fs::is_regular_file(path_))
        throw std::runtime_error("Поточний файл сцени відсутній або замінений");
    if (readDocument(path_) != savedBytes_)
        throw std::runtime_error("Поточний файл сцени змінився зовні");
}
void SceneDocument::saveProject(const std::filesystem::path& path, const std::filesystem::path& projectRoot) {
    endGesture();
    const auto root = fs::absolute(projectRoot).lexically_normal();
    if (!fs::is_directory(root))
        throw std::runtime_error("Не знайдено корінь проєкту");
    const auto [resolved, relative] = projectScenePath(root, path);
    const auto metadataRelative = relative + ".meta";
    const auto metadataPath = projectPath(root, metadataRelative);
    const bool same = !path_.empty() && samePath(resolved, path_);
    if (same) {
        if (!fs::exists(resolved) || readDocument(resolved) != savedBytes_)
            throw std::runtime_error("Поточний файл сцени змінився зовні");
    } else if (fs::exists(resolved) || fs::exists(metadataPath)) {
        throw std::runtime_error("Цільова сцена або її sidecar вже існує");
    }
    const auto metadataOwner = fs::exists(metadataPath) ? sceneMetadataOwner(metadataPath) : AssetId{};
    if (same && metadataOwner && metadataOwner != scene.id)
        throw std::runtime_error("Sidecar належить іншій сцені");

    auto workspace = workspace_;
    if (!workspace || !rootMatches(workspace->root(), root))
        workspace = std::make_shared<AssetWorkspace>(root);
    auto catalog = loadProjectAssets(workspace, scene.modelSources);
    auto snapshot = scene.snapshot();
    snapshot.id = same ? scene.id : AssetId::create();
    snapshot.assetRoot = projectAssetRoot(root, resolved);
    auto candidate = Scene::fromSnapshot(snapshot, catalog);
    const auto bytes = encodeScene(candidate);
    const auto metadata = sceneMetadata(snapshot.id);

    FilePlan plan;
    plan.label = same ? "Save project scene" : "Save project scene as";
    plan.edits = {{relative, FileKind::File, bytes, {}}, {metadataRelative, FileKind::File, metadata, {}}};
    plan.guards = {{relative, projectStamp(root, relative)}, {metadataRelative, projectStamp(root, metadataRelative)}};
    auto transaction = FileTransaction::prepare(root, plan);
    transaction->redo();

    scene = std::move(candidate);
    scene.assetRoot = snapshot.assetRoot;
    workspace_ = std::move(workspace);
    path_ = resolved;
    savedBytes_ = bytes;
    savedState_ = state_;
}
std::shared_ptr<AssetWorkspace> SceneDocument::workspace() {
    if (!workspace_) {
        if (path_.empty())
            throw std::runtime_error("Збережіть сцену, щоб обрати папку ресурсів");
        workspace_ = std::make_shared<AssetWorkspace>(path_.parent_path());
        scene.assetRoot = ".";
    }
    return workspace_;
}
EntityId SceneDocument::instantiate(std::shared_ptr<const ModelBundle> model) {
    endGesture();
    workspace();
    if (!scene.assets->models.contains(model->id) || scene.assets->models.at(model->id)->revision != model->revision)
        scene.assets->publish(model);
    scene.assetsChanged();
    EntityRecord root;
    root.id = EntityId::create();
    root.name = model->name;
    std::vector<EntityRecord> records{root};
    for (const auto& node : model->nodes) {
        EntityRecord record;
        record.id = EntityId::create();
        record.name = node.name;
        record.parent = node.parent < 0 ? root.id : records.at(size_t(node.parent) + 1).id;
        record.transform = decomposeTrs(node.local);
        if (node.mesh)
            record.mesh = MeshRenderer{node.mesh, glm::vec4(1)};
        records.push_back(record);
    }
    Command command{Kind::Create, {}, records, scene.activeCamera, scene.activeCamera, 0, 0, "Додавання моделі"};
    command.sourcesBefore = scene.modelSources;
    scene.restore(records);
    if (std::find(scene.modelSources.begin(), scene.modelSources.end(), model->id) == scene.modelSources.end())
        scene.modelSources.push_back(model->id);
    command.sourcesAfter = scene.modelSources;
    selection = root.id;
    commit(std::move(command));
    return root.id;
}
void SceneDocument::material(AssetId id, const MaterialValues& values, bool history) {
    if (history)
        endGesture();
    auto before = scene.assets->materials.at(id);
    validateMaterialEdit(before->values, values);
    if (before->values == values)
        return;
    auto changed = std::make_shared<MaterialAsset>(*before);
    changed->values = values;
    validateMaterial(values);
    ++changed->revision;
    if (!history) {
        scene.assets->materials[id] = std::move(changed);
        return;
    }
    auto workspace = this->workspace();
    std::shared_ptr<FileTransaction> transaction;
    try {
        const auto prepared = workspace->prepareMaterial(*changed);
        transaction = prepareMaterialTransaction(workspace, prepared);
        scene.assets->materials[id] = changed;
        Command c{Kind::Material, {}, {}, scene.activeCamera, scene.activeCamera, 0, 0, "Матеріал"};
        c.materialId = id;
        c.materialBefore = before->values;
        c.materialAfter = values;
        c.materialTransaction = transaction;
        commit(std::move(c));
    } catch (...) {
        if (transaction)
            try {
                transaction->undo();
            } catch (...) {
            }
        scene.assets->materials[id] = std::move(before);
        throw;
    }
}
void SceneDocument::beginMaterialGesture(AssetId id) {
    if (materialGesture_ && materialGesture_->id == id)
        return;
    endGesture();
    materialGesture_ = scene.assets->materials.at(id);
}
void SceneDocument::previewMaterial(AssetId id, const MaterialValues& values) {
    beginMaterialGesture(id);
    validateMaterialEdit(materialGesture_->values, values);
    auto next = std::make_shared<MaterialAsset>(*scene.assets->materials.at(id));
    next->values = values;
    ++next->revision;
    scene.assets->materials[id] = next;
    gestureChanged_ = true;
}
} // namespace proto
