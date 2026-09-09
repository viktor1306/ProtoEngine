#include "editor/SceneDocument.hpp"
#include "WinJunction.hpp"
#include "AssetFixtures.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "project/FileTransactions.hpp"
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace proto;
namespace {
namespace fs = std::filesystem;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void rejects(Function&& function) {
    bool failed{};
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "Expected rejection");
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    check(bool(stream), "Fixture write failed");
}

std::string readBytes(const fs::path& path) {
    const auto bytes = assetBytes(path);
    return {bytes.begin(), bytes.end()};
}

EntityRecord object(std::string name = "Об’єкт") {
    EntityRecord result;
    result.id = EntityId::create();
    result.name = std::move(name);
    result.mesh = MeshRenderer{};
    return result;
}

struct ExternalValue final : DocumentAction {
    int& value;
    bool fail{};
    std::string name;

    ExternalValue(int& target, bool shouldFail, std::string actionName)
        : value(target), fail(shouldFail), name(std::move(actionName)) {}

    void apply(bool forward) override {
        if (fail)
            throw std::runtime_error("Injected external action failure");
        value = forward ? 1 : 0;
    }
    size_t bytes() const override { return 64; }
    std::string label() const override { return name; }
};
} // namespace

int main() {
    const auto base =
        fs::absolute(fs::temp_directory_path()) / utf8Path("proto-m4-document-" + Uuid::create().string());
    fs::create_directories(base);
    unsigned passed{}, failed{};
    const auto test = [&](const char* name, const auto& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("cancel gesture restores entity without history or dirty change", [&] {
        SceneDocument document;
        const auto id = document.create(object("Cancel"));
        const auto before = document.scene.record(document.scene.find(id));
        const auto history = document.undoCount();
        const auto dirty = document.dirty();
        document.beginGesture(id);
        auto changed = before;
        changed.transform.position.x += 4.0f;
        document.preview(changed);
        check(document.scene.record(document.scene.find(id)) == changed, "Preview did not apply");
        document.cancelGesture();
        check(document.scene.record(document.scene.find(id)) == before, "Cancel did not restore entity");
        check(document.undoCount() == history && document.dirty() == dirty, "Cancel changed history or dirty state");
    });

    test("project workspace binds on an empty scene and refresh keeps document history", [&] {
        const auto root = base / "workspace";
        fs::create_directories(root / "Assets");
        SceneDocument document;
        document.newScene();
        const auto history = document.undoCount();
        const auto dirty = document.dirty();
        auto workspace = std::make_shared<AssetWorkspace>(root);
        document.bindProjectWorkspace(workspace);
        check(document.workspace()->root() == fs::absolute(root).lexically_normal(), "Workspace root was not bound");
        document.refreshProjectAssets();
        check(document.undoCount() == history && document.dirty() == dirty, "Asset refresh changed document history");
    });

    test("redo instantiate lazily reloads command target sources after refresh", [&] {
        const auto fixtureRoot = base / "instantiate-fixtures";
        const auto projectRoot = base / "instantiate-project";
        fs::create_directories(projectRoot);
        fixtures::create(fixtureRoot);
        auto workspace = std::make_shared<AssetWorkspace>(projectRoot);
        const auto model = workspace->importFile(fixtureRoot / "probes.gltf");
        SceneDocument document;
        document.newScene();
        document.bindProjectWorkspace(workspace);
        const auto rootId = document.instantiate(model);
        check(document.scene.assets->models.contains(model->id), "Instantiate did not publish model");
        document.undo();
        check(document.scene.modelSources.empty() && document.scene.assets->models.empty(),
              "Undo instantiate retained the removed model catalog");
        document.refreshProjectAssets();
        check(document.scene.assets->models.empty(), "Refresh unexpectedly retained historical model");
        document.redo();
        check(document.scene.modelSources.size() == 1 && document.scene.modelSources.front() == model->id &&
                  bool(document.scene.find(rootId)),
              "Redo did not restore model source and root");
        bool hasMesh{};
        for (const auto handle : document.scene.meshes())
            hasMesh |= bool(document.scene.mesh(handle));
        check(hasMesh, "Redo restored model nodes without mesh assets");
    });

    test("external action preserves dirty token and orders with scene Undo", [&] {
        const auto root = base / "external";
        fs::create_directories(root);
        SceneDocument document;
        document.newScene();
        document.save(root / "base.scene.json");
        check(!document.dirty(), "Baseline save is dirty");
        int value{};
        auto action = std::make_shared<ExternalValue>(value, false, "file move");
        document.executeExternal(action);
        check(value == 1 && !document.dirty() && document.undoCount() == 1, "External action changed document token");
        auto changed = document.scene.record(document.scene.find(document.selection));
        changed.name = "Scene edit";
        document.edit(changed);
        check(document.dirty() && document.undoCount() == 2, "Scene edit did not follow external action");
        document.undo();
        check(value == 1 && !document.dirty(), "Undo scene edit did not restore saved token");
        document.undo();
        check(value == 0 && !document.dirty() && document.undoCount() == 0, "Undo external action ordering failed");
        document.redo();
        document.redo();
        check(value == 1 && document.dirty(), "Redo ordering or dirty token failed");
    });

    test("failed external action does not move cursor", [&] {
        SceneDocument document;
        const auto history = document.undoCount();
        int value{};
        auto action = std::make_shared<ExternalValue>(value, true, "failed");
        rejects([&] { document.executeExternal(action); });
        check(document.undoCount() == history && value == 0, "Failed external action moved document history");
    });

    test("external file history survives switching to a new project scene", [&] {
        const auto root = base / "external-switch";
        fs::create_directories(root / "Scenes");
        SceneDocument document;
        document.newScene();
        document.saveProject(root / "Scenes" / "Main.scene.json", root);
        auto record = document.scene.record(document.scene.find(document.selection));
        record.name = "old scene edit";
        document.edit(record, "old scene edit");
        int value{};
        document.executeExternal(std::make_shared<ExternalValue>(value, false, "file move"));
        check(document.undoCount() == 2 && value == 1, "External action was not recorded after scene edit");
        document.newScene(true);
        check(document.undoCount() == 1 && !document.canRedo(), "Old scene command was retained across switch");
        document.undo();
        check(value == 0, "External Undo failed after scene switch");
        document.redo();
        check(value == 1, "External Redo failed after scene switch");
    });

    test("project save writes canonical sidecar and Save As keeps entity IDs", [&] {
        const auto root = base / "save";
        fs::create_directories(root / "Scenes");
        SceneDocument document;
        const auto first = root / "Scenes" / "Main.scene.json";
        document.saveProject(first, root);
        const auto originalSceneId = document.scene.id;
        const auto originalEntity = document.scene.entities().front();
        const auto originalEntityId = document.scene.entity(originalEntity).id;
        const auto firstBytes = readBytes(first);
        const auto metadata = readBytes(fs::path(first.wstring() + L".meta"));
        check(metadata == "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"assetId\":" +
                              jsonString(originalSceneId.string()) + ",\"kind\":\"Scene\"}\n",
              "Scene sidecar is not canonical");
        const auto second = root / "Scenes" / "Copy.scene.json";
        document.saveProject(second, root);
        check(document.scene.id != originalSceneId, "Save As retained SceneId");
        check(bool(document.scene.find(originalEntityId)), "Save As changed an EntityId");
        check(readBytes(first) == firstBytes, "Save As changed original scene");
        check(readBytes(fs::path(second.wstring() + L".meta")).find(document.scene.id.string()) != std::string::npos,
              "Save As sidecar has wrong SceneId");
    });

    test("project save rejects external same-path scene changes", [&] {
        const auto root = base / "conflict";
        fs::create_directories(root / "Scenes");
        SceneDocument document;
        const auto path = root / "Scenes" / "Main.scene.json";
        document.saveProject(path, root);
        document.create(object("Unsaved"));
        writeBytes(path, "external change");
        rejects([&] { document.saveProject(path, root); });
        check(readBytes(path) == "external change" && document.dirty(), "External scene conflict was overwritten");
    });

    test("material Undo survives project scene switch with exact metadata bytes", [&] {
        const auto fixtureRoot = base / "material-fixtures";
        const auto root = base / "material-project";
        fs::create_directories(root / "Assets");
        fs::create_directories(root / "Scenes");
        fixtures::create(fixtureRoot);
        auto workspace = std::make_shared<AssetWorkspace>(root);
        const auto model = workspace->importFile(fixtureRoot / "probes.gltf");
        SceneDocument document;
        document.newScene();
        document.bindProjectWorkspace(workspace);
        document.instantiate(model);
        const auto scenePath = root / "Scenes" / "Main.scene.json";
        document.saveProject(scenePath, root);
        check(!model->materials.empty(), "Material fixture is empty");
        const auto materialId = model->materials.front()->id;
        auto material = document.scene.assets->materials.at(materialId);
        auto expectedMaterial = *material;
        expectedMaterial.values.baseColor.r = .73f;
        auto prepared = workspace->prepareMaterial(expectedMaterial);
        const auto materialPath = root / utf8Path(prepared.path);
        const auto before = readBytes(materialPath);
        auto changed = material->values;
        changed.baseColor.r = .73f;
        document.material(materialId, changed);
        const auto after = readBytes(materialPath);
        check(after == prepared.after && after != before, "Material edit did not publish exact bytes");
        document.loadProject(scenePath, workspace);
        check(document.undoCount() == 1 && !document.dirty(), "Scene switch did not preserve material history");
        document.undo();
        check(readBytes(materialPath) == before, "Material Undo did not restore exact bytes");
        document.redo();
        check(readBytes(materialPath) == after, "Material Redo did not restore exact bytes");
        document.undo();
        document.newScene(true);
        document.bindProjectWorkspace(workspace);
        document.redo();
        check(readBytes(materialPath) == after, "Material Redo failed after new project scene");
        document.loadProject(scenePath, workspace);
        const auto history = document.undoCount();
        auto preview = document.scene.assets->materials.at(materialId)->values;
        preview.baseColor.g = .37f;
        document.beginMaterialGesture(materialId);
        document.previewMaterial(materialId, preview);
        document.refreshProjectAssets();
        check(document.scene.assets->materials.at(materialId)->values == preview,
              "Asset refresh discarded an active material preview");
        document.endGesture();
        check(document.undoCount() == history + 1 && workspace->load(model->id)->materials.front()->values == preview,
              "Material preview did not survive refresh as one persisted command");
    });

    test("project loader rejects escaped assets and redirected metadata before outside I/O", [&] {
        const auto root = base / "boundary";
        const auto outside = base / "outside";
        fs::create_directories(root / "Scenes");
        fs::create_directories(outside);
        auto workspace = std::make_shared<AssetWorkspace>(root);
        SceneDocument document;
        document.newScene();
        const auto main = root / "Scenes/Main.scene.json";
        document.saveProject(main, root);
        const auto previousId = document.scene.id;
        const auto previousHistory = document.undoCount();
        const auto previous = readBytes(main);
        auto escaped = previous;
        check(escaped.find("\"assetRoot\"") == std::string::npos, "Boundary fixture unexpectedly has assets");
        escaped.insert(escaped.rfind('}'), ",\"assetRoot\":\"../../outside\",\"modelSources\":[" +
                                               jsonString(AssetId::create().string()) + "]");
        const auto other = root / "Scenes/Other.scene.json";
        writeBytes(other, escaped);
        const auto meta = fs::path(other.wstring() + L".meta");
        fs::copy_file(fs::path(main.wstring() + L".meta"), meta);
        rejects([&] { document.loadProject(other, workspace); });
        check(!fs::exists(outside / ".proto") && document.path() == main && document.scene.id == previousId &&
                  document.undoCount() == previousHistory,
              "Escaped load touched outside or replaced document");
        writeBytes(other, previous);
        const auto externalMeta = outside / "external.meta";
        fs::rename(meta, externalMeta);
        if (CreateSymbolicLinkW(meta.c_str(), externalMeta.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            rejects([&] { document.loadProject(other, workspace); });
            rejects([&] { (void)document.prepareProjectRelocation(other, workspace); });
            fs::remove(meta);
        } else {
            check(GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "Unexpected file-symlink fixture error");
            std::cout
                << "NOTE file symlink fixture unavailable (Windows privilege); testing directory junction boundary\n";
        }
        const auto redirect = root / "Scenes/Redirect";
        writeBytes(outside / "External.scene.json", previous);
        fs::copy_file(externalMeta, outside / "External.scene.json.meta");
        testHelpers::makeJunction(redirect, outside);
        rejects([&] { document.loadProject(redirect / "External.scene.json", workspace); });
        rejects([&] { (void)document.prepareProjectRelocation(redirect / "External.scene.json", workspace); });
        check(document.path() == main && document.scene.id == previousId && !fs::exists(outside / ".proto"),
              "Redirected metadata replaced live state");
        RemoveDirectoryW(redirect.c_str());
    });

    test("relocate keeps unsaved scene state and next save publishes there", [&] {
        const auto root = base / "relocate";
        fs::create_directories(root / "Scenes");
        SceneDocument document;
        const auto first = root / "Scenes" / "Main.scene.json";
        const auto moved = root / "Scenes" / "Moved.scene.json";
        document.saveProject(first, root);
        const auto id = document.create(object("Unsaved child"));
        const auto selection = document.selection;
        const auto history = document.undoCount();
        check(document.dirty(), "Unsaved scene edit was not dirty");
        fs::rename(first, moved);
        fs::rename(fs::path(first.wstring() + L".meta"), fs::path(moved.wstring() + L".meta"));
        auto workspace = std::make_shared<AssetWorkspace>(root);
        document.relocateProject(moved, workspace);
        check(document.path() == fs::absolute(moved).lexically_normal() && document.selection == selection &&
                  document.undoCount() == history && document.dirty(),
              "Relocate changed unsaved document state");
        check(bool(document.scene.find(id)), "Relocate lost unsaved entity");
        document.saveProject(moved, root);
        check(!document.dirty() && readBytes(moved).find(id.string()) != std::string::npos,
              "Next save did not publish relocated unsaved state");
    });

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
