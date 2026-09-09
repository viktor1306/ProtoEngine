#include "editor/Editor.hpp"
#include "editor/EditorPlayState.hpp"
#include "project/ProjectSession.hpp"
#include "assets/AssetIO.hpp"
#include "runtime/RuntimePackage.hpp"
#include <Proto/Build.hpp>
#include <imgui.h>

namespace proto {
struct ExportSmokeState {
    std::filesystem::path root, output, manifest;
    std::string originalScene, manifestHash;
    uint64_t sceneFrames{};
    int stage{};
    bool passed{};
};
namespace {
void requireExport(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
}
void Editor::startExportSmoke(const std::filesystem::path& output) {
    automatedInput_ = true;
    auto test = std::make_shared<ExportSmokeState>();
    test->output = std::filesystem::absolute(output);
    test->root = test->output / ("m6-editor-" + Uuid::create().string());
    std::filesystem::create_directories(test->root);
    const std::filesystem::path sample(PROTO_M5_SAMPLE_DIR);
    for (const auto* folder : {"Assets", "Scenes", "Code", "Config"})
        std::filesystem::copy(sample / folder, test->root / folder, std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(sample / "project.proto.json", test->root / "project.proto.json");
    openProject(test->root);
    test->originalScene = readDocument(document_.path());
    exportSmoke_ = std::move(test);
    useSceneCamera_ = true;
}
bool Editor::exportSmokePassed() const { return exportSmoke_ && exportSmoke_->passed; }
void Editor::exportSmokeStep(uint64_t frame) {
    if (!exportSmoke_ || exportSmoke_->passed || frame < 6)
        return;
    auto& test = *exportSmoke_;
    const auto click = [this](bool down) {
        const auto center = ImVec2((exportButtonMin_.x + exportButtonMax_.x) * .5f,
                                   (exportButtonMin_.y + exportButtonMax_.y) * .5f);
        testInput_ = [center, down] {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(center.x, center.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        };
    };
    if (test.stage == 0) {
        const auto entity = EntityId::parse("50000000-0000-4000-8000-000000000010");
        auto record = document_.scene.record(document_.scene.find(entity));
        record.transform.position.x += .25f;
        document_.edit(record);
        requestExport();
        requireExport(!playBusy() && showError_, "Export accepted an unsaved document");
        showError_ = false;
        error_.clear();
        document_.undo();
        requireExport(!document_.dirty(), "Export guard changed saved document state");
        test.stage = 1;
    } else if (test.stage == 1 && exportButtonMax_.x > exportButtonMin_.x) {
        click(true);
        test.stage = 2;
    } else if (test.stage == 2) {
        click(false);
        test.stage = 3;
    } else if (test.stage == 3 && playBusy()) {
        test.sceneFrames = renderer_.sceneRenderFrames();
        test.stage = 4;
    } else if (test.stage == 4) {
        if (playBusy()) {
            requireExport(renderer_.sceneRenderFrames() == test.sceneFrames,
                          "Editor kept rendering the scene during export");
            return;
        }
        requireExport(playUi_ && playUi_->lastSucceeded && playUi_->lastPackage,
                      "Native export button did not produce a package");
        test.manifest = playUi_->lastPackage->manifest;
        test.manifestHash = sha256(assetBytes(test.manifest));
        requireExport(readDocument(document_.path()) == test.originalScene,
                      "Export changed the authored scene");
        requestExport();
        requireExport(playBusy(), "Repeat export did not start");
        stopPlay();
        test.stage = 5;
    } else if (test.stage == 5 && !playBusy()) {
        requireExport(!playUi_->lastSucceeded, "Cancelled export was reported successful");
        requireExport(sha256(assetBytes(test.manifest)) == test.manifestHash,
                      "Cancelled export replaced the previous package");
        const auto package = loadRuntimePackage(test.manifest, sdk::buildId);
        requireExport(package.projectId == project_->info().id, "Export package identity mismatch");
        atomicWrite(test.output / "m6-export-details.json",
                    "{\"passed\":true,\"native_button\":true,\"unsaved_guard\":true,"
                    "\"cancel_preserves_output\":true,\"scene_paused\":true,\"authored_preserved\":true,"
                    "\"package\":" + jsonString(utf8(package.root.wstring())) + "}\n");
        status_ = "M6: експорт, скасування та збереження проєкту перевірено";
        playUi_->showLog = false;
        test.passed = true;
    }
}
}
