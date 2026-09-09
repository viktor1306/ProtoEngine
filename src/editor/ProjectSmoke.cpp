#include "editor/Editor.hpp"
#include "editor/ProjectUiState.hpp"
#include "project/ProjectSession.hpp"
#include "assets/AssetIO.hpp"
#include <fstream>
#include <algorithm>

namespace proto {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
void Editor::startProjectSmoke(const std::filesystem::path& directory, const std::filesystem::path& model) {
    projectSmoke_ = std::make_shared<ProjectSmokeState>();
    auto& smoke = *projectSmoke_;
    smoke.root = directory / utf8Path("m4-Майстерня-" + Uuid::create().string());
    smoke.model = model;
    createProject(smoke.root, "Майстерня Proto");
    smoke.projectId = project_->info().id;
    smoke.sceneId = document_.scene.id;
    smoke.scenePath = projectRelative(smoke.root, document_.path());
    importScene_ = document_.scene.id;
    importInstance_ = true;
    importJob_.start(project_->assetWorkspace(), model);
}
bool Editor::projectSmokePassed() const {
    return projectSmoke_ && projectSmoke_->passed;
}
void Editor::projectSmokeStep(uint64_t frame) {
    if (!projectSmoke_ || projectSmoke_->passed)
        return;
    auto& smoke = *projectSmoke_;
    if (showError_ || !error_.empty())
        throw std::runtime_error("Project UI smoke: " + error_);
    const auto mouse = [&](ImVec2 point, int button) {
        testInput_ = [point, button] {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            if (button >= 0)
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, button != 0);
        };
    };
    const auto key = [&](ImGuiKey value, bool down, bool ctrl = false) {
        testInput_ = [value, down, ctrl] {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, ctrl && down);
            io.AddKeyEvent(value, down);
        };
    };
    if (smoke.stage == 0 && frame >= 8 && !importJob_.busy()) {
        require(document_.scene.modelSources.size() == 1, "Project import did not instantiate its model");
        smoke.modelId = document_.scene.modelSources.front();
        smoke.source = document_.scene.assets->models.at(smoke.modelId)->source;
        require(smoke.source.starts_with("Assets/"), "Project import escaped Assets");
        require(document_.workspace()->root() == smoke.root, "Import workspace points into Scenes");
        document_.saveProject(document_.path(), smoke.root);
        bool dependencyIds{};
        for (const auto& asset : inspectProjectAssets(smoke.root))
            dependencyIds |= asset.kind == "RawDependency";
        require(dependencyIds, "Normal project import did not enroll dependency metadata");
        executeFilePlan({"Smoke folder", {{"Assets/Копії", FileKind::Directory}}});
        navigateFiles(utf8(utf8Path(smoke.source).parent_path().generic_wstring()));
        const auto filename = utf8(utf8Path(smoke.source).filename().wstring());
        std::copy_n(filename.data(), std::min(filename.size(), projectUi_->filter.size() - 1),
                    projectUi_->filter.data());
        smoke.history = document_.undoCount();
        smoke.stage = 1;
    } else if (smoke.stage == 1) {
        require(projectUi_->rowCenters.contains(smoke.source), "File source row is not visible");
        mouse(projectUi_->rowCenters.at(smoke.source), 1);
        smoke.stage = 2;
    } else if (smoke.stage == 2) {
        mouse(projectUi_->rowCenters.at(smoke.source), 0);
        smoke.stage = 3;
    } else if (smoke.stage == 3) {
        key(ImGuiKey_C, true, true);
        smoke.stage = 4;
    } else if (smoke.stage == 4) {
        key(ImGuiKey_C, false);
        smoke.stage = 5;
    } else if (smoke.stage == 5) {
        require(projectUi_->clipboard == std::vector<std::string>{smoke.source}, "Ctrl+C did not copy selected file");
        navigateFiles("Assets/Копії");
        smoke.stage = 6;
    } else if (smoke.stage == 6) {
        mouse(projectUi_->pasteCenter, 1);
        smoke.stage = 7;
    } else if (smoke.stage == 7) {
        mouse(projectUi_->pasteCenter, 0);
        smoke.stage = 8;
    } else if (smoke.stage == 8 && !projectUi_->pending) {
        smoke.copied = "Assets/Копії/" + utf8(utf8Path(smoke.source).filename().wstring());
        require(std::filesystem::exists(projectPath(smoke.root, smoke.copied)),
                "Paste button did not publish model copy");
        for (const auto& asset : inspectProjectAssets(smoke.root))
            if (asset.path == smoke.copied && asset.kind == "ModelSource")
                smoke.copiedId = AssetId::parse(asset.id);
        require(smoke.copiedId && smoke.copiedId != smoke.modelId, "Copy kept original model UUID");
        require(document_.undoCount() == smoke.history + 1, "File copy did not enter shared Undo history");
        document_.undo();
        require(!std::filesystem::exists(projectPath(smoke.root, smoke.copied)), "Copy Undo left destination");
        document_.redo();
        bool stable{};
        for (const auto& asset : inspectProjectAssets(smoke.root))
            stable |= asset.path == smoke.copied && asset.id == smoke.copiedId.string();
        require(stable, "Copy Redo changed resource UUID");
        require(document_.scene.id == smoke.sceneId, "File copy changed scene UUID");
        smoke.stage = 9;
    } else if (smoke.stage == 9) {
        mouse(projectUi_->rowCenters.at(smoke.copied), 1);
        smoke.stage = 10;
    } else if (smoke.stage == 10) {
        mouse(projectUi_->rowCenters.at(smoke.copied), 0);
        smoke.stage = 11;
    } else if (smoke.stage == 11) {
        key(ImGuiKey_F2, true);
        smoke.stage = 12;
    } else if (smoke.stage == 12) {
        key(ImGuiKey_F2, false);
        smoke.stage = 13;
    } else if (smoke.stage == 13) {
        testInput_ = [] {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
        };
        smoke.stage = 14;
    } else if (smoke.stage == 14) {
        testInput_ = [] {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddInputCharactersUTF8("Копія моделі.gltf");
        };
        smoke.stage = 15;
    } else if (smoke.stage == 15) {
        key(ImGuiKey_Enter, true);
        smoke.stage = 16;
    } else if (smoke.stage == 16) {
        key(ImGuiKey_Enter, false);
        smoke.stage = 17;
    } else if (smoke.stage == 17 && !projectUi_->pending) {
        smoke.renamed = "Assets/Копії/Копія моделі.gltf";
        require(std::filesystem::exists(projectPath(smoke.root, smoke.renamed)), "F2 rename UI did not rename file");
        require(!std::filesystem::exists(projectPath(smoke.root, smoke.copied)), "Rename UI left old source");
        require(project_->assetWorkspace()->load(smoke.copiedId)->source == smoke.renamed,
                "Renamed copy cannot reload by UUID");
        // Insert a scene edit between two file commands, then prove the same
        // history restores the scene and files in reverse chronological order.
        smoke.entity = document_.selection;
        auto record = document_.scene.record(document_.scene.find(smoke.entity));
        record.transform.position.x += .4f;
        smoke.editedPosition = record.transform.position;
        document_.edit(record);
        operateFiles(int(FileOperation::Move), {smoke.scenePath}, "Scenes", std::string("Майстерня.scene.json"));
        require(document_.dirty() && document_.scene.record(document_.scene.find(smoke.entity)).transform.position ==
                                         smoke.editedPosition,
                "Moving active scene lost its unsaved edit");
        require(document_.scene.id == smoke.sceneId && document_.path().filename() == utf8Path("Майстерня.scene.json"),
                "Active scene move became SaveAs");
        document_.undo();
        require(projectRelative(smoke.root, document_.path()) == smoke.scenePath && document_.dirty(),
                "Undo scene move lost dirty state/path");
        document_.undo();
        require(!document_.dirty(), "Mixed history Undo did not restore saved scene state");
        document_.redo();
        document_.redo();
        document_.saveProject(document_.path(), smoke.root);
        const auto savedScene = document_.path();
        const auto id = project_->info().id;
        // Clear history before releasing the single-writer project session.
        document_.newScene();
        project_.reset();
        projectUi_.reset();
        openProject(smoke.root);
        require(project_->info().id == id && document_.scene.id == smoke.sceneId && document_.path() == savedScene,
                "Project reopen lost UUID startup mapping");
        require(document_.scene.record(document_.scene.find(smoke.entity)).transform.position == smoke.editedPosition,
                "Project reopen lost saved scene edit");
        const auto savedBytes = readDocument(savedScene);
        const auto manifestPath = smoke.root / "project.proto.json";
        const auto manifestBytes = readDocument(manifestPath);
        const auto previousUi = projectUi_;
        const auto previousHistory = document_.undoCount();
        bool rejected{};
        try {
            executeFilePlan(
                {"Invalid manifest candidate", {{"project.proto.json", FileKind::File, std::string("{broken"), {}}}});
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && readDocument(manifestPath) == manifestBytes && document_.path() == savedScene &&
                    projectUi_ == previousUi && document_.undoCount() == previousHistory &&
                    project_->startupScenePath() == savedScene,
                "Failed project reconciliation partially published editor state");
        executeFilePlan({"Baseline guard", {{"Code/Baseline", FileKind::Directory}}});
        {
            std::ofstream edited(savedScene, std::ios::binary);
            edited << savedBytes << "\n ";
        }
        rejected = false;
        try {
            document_.undo();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && std::filesystem::exists(smoke.root / "Code/Baseline") &&
                    readDocument(savedScene) == savedBytes + "\n ",
                "Undo adopted or overwrote an external scene version");
        {
            std::ofstream restored(savedScene, std::ios::binary);
            restored << savedBytes;
        }
        document_.undo();
        // File history belongs to the open project even while the active
        // scene changes. An active copied scene cannot disappear under Undo.
        const auto currentSceneRelative = projectRelative(smoke.root, savedScene);
        operateFiles(int(FileOperation::Copy), {currentSceneRelative}, "Scenes", std::string("ІНША.SCENE.JSON"));
        const auto otherScene = projectPath(smoke.root, "Scenes/ІНША.SCENE.JSON");
        openProjectScene(otherScene);
        const auto copiedSceneId = document_.scene.id;
        const auto beforeUndo = document_.undoCount();
        rejected = false;
        try {
            document_.undo();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && document_.path() == otherScene && document_.scene.id == copiedSceneId &&
                    document_.undoCount() == beforeUndo && std::filesystem::exists(otherScene),
                "Undo deleted the active copied scene or corrupted its history");
        openProjectScene(savedScene);
        document_.undo();
        require(!std::filesystem::exists(otherScene), "Scene switch discarded project file Undo history");
        const auto originalRaw = utf8(utf8Path(smoke.source).parent_path().generic_wstring()) + "/mesh data.bin";
        const auto copiedRaw = "Assets/Копії/mesh data.bin";
        operateFiles(int(FileOperation::Copy), {originalRaw}, "Assets/Копії");
        operateFiles(int(FileOperation::Remove), {copiedRaw}, "");
        require(!std::filesystem::exists(projectPath(smoke.root, copiedRaw)) &&
                    std::filesystem::exists(projectPath(smoke.root, originalRaw)),
                "An unused dependency copy could not be deleted independently of its provenance owner");
        document_.undo();
        require(std::filesystem::exists(projectPath(smoke.root, copiedRaw)),
                "Deleted raw copy was not restored by Undo");
        document_.redo();
        navigateFiles("Assets/Копії");
        smoke.frame = frame;
        smoke.stage = 18;
    } else if (smoke.stage == 18 && frame > smoke.frame + 18 && !projectUi_->refreshPending) {
        smoke.refreshBaseline = projectUi_->refreshes;
        smoke.frame = frame;
        smoke.stage = 19;
    } else if (smoke.stage == 19 && frame > smoke.frame + 18) {
        require(projectUi_->refreshes == smoke.refreshBaseline,
                "Idle file browser rescanned without a change notification");
        std::ofstream external(projectPath(smoke.root, "Assets/Копії/Зовнішній.txt"), std::ios::binary);
        external << "watcher external change";
        smoke.frame = frame;
        smoke.stage = 20;
    } else if (smoke.stage == 20) {
        bool visible{};
        for (const auto& entry : projectUi_->entries)
            visible |= entry.name == "Зовнішній.txt";
        if (visible) {
            require(projectUi_->refreshes > smoke.refreshBaseline, "External event did not refresh file cache");
            smoke.stage = 21;
        }
    } else if (smoke.stage == 21) {
        require(projectUi_->rowCenters.contains(smoke.renamed), "Copied model row is unavailable for drag/drop");
        mouse(projectUi_->rowCenters.at(smoke.renamed), 1);
        smoke.stage = 22;
    } else if (smoke.stage == 22) {
        auto point = projectUi_->rowCenters.at(smoke.renamed);
        point.x += 20;
        mouse(point, -1);
        smoke.stage = 23;
    } else if (smoke.stage == 23) {
        mouse({viewportPosition_.x + viewportSize_.x * .5f, viewportPosition_.y + viewportSize_.y * .5f}, -1);
        smoke.stage = 24;
    } else if (smoke.stage == 24) {
        mouse({viewportPosition_.x + viewportSize_.x * .5f, viewportPosition_.y + viewportSize_.y * .5f}, 0);
        smoke.stage = 25;
    } else if (smoke.stage == 25 && !importJob_.busy() && !projectUi_->pending) {
        require(std::find(document_.scene.modelSources.begin(), document_.scene.modelSources.end(), smoke.copiedId) !=
                    document_.scene.modelSources.end(),
                "Dragging a project model into viewport did not instantiate it");
        require(document_.scene.id == smoke.sceneId, "Model drag/drop changed SceneId");
        smoke.passed = true;
    }
}
} // namespace proto
