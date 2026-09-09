#include "editor/Editor.hpp"
#include "editor/CodeUiState.hpp"
#include "editor/EditorPlayState.hpp"
#include "editor/ProjectUiState.hpp"
#include "project/ProjectSession.hpp"
#include "assets/AssetIO.hpp"
#include "scene/SceneIO.hpp"
#include <algorithm>
#include <imgui_internal.h>
#include <string_view>

namespace proto {
struct PlaySmokeState {
    std::filesystem::path root, output;
    std::string source, documentBefore;
    EntityId selection;
    size_t undo{};
    uint64_t pausedFrames{};
    int stage{};
    Clock::time_point started;
    bool passed{}, sawRunning{}, compileFailure{}, crash{}, forced{}, restart{};
    bool schemaRecoveryStarted{}, schemaRecoveryChecked{};
    EntityId missingTarget;
};
namespace {
void requirePlay(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
void Editor::startPlaySmoke(const std::filesystem::path& output) {
    automatedInput_ = true;
    stringProbe_ = {};
    stringProbe_.enabled = true;
    auto state = std::make_shared<PlaySmokeState>();
    state->output = std::filesystem::absolute(output);
    state->root = state->output / ("m5-editor-" + Uuid::create().string());
    std::filesystem::create_directories(state->root);
    const std::filesystem::path sample(PROTO_M5_SAMPLE_DIR);
    for (const auto* folder : {"Assets", "Scenes", "Code", "Config"})
        std::filesystem::copy(sample / folder, state->root / folder, std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(sample / "project.proto.json", state->root / "project.proto.json");
    state->source = readDocument(state->root / "Code/Behaviors.cpp");
    const std::string signature = "void OnUpdate(BehaviorContext& context, double dt) override {";
    const auto at = state->source.find(signature);
    requirePlay(at != std::string::npos, "M5 sample Spin insertion point missing");
    state->source.insert(at + signature.size(),
                         "\n        if (context.Property<double>(\"speedDegreesPerSecond\") == -359) std::abort();"
                         "\n        if (context.Property<double>(\"speedDegreesPerSecond\") == -358) for (;;) "
                         "std::this_thread::sleep_for(std::chrono::milliseconds(10));\n");
    state->source = "#include <cstdlib>\n#include <thread>\n#include <chrono>\n" + state->source;
    const std::string speedProperty = "{\"speedDegreesPerSecond\", PropertyType::Float";
    const auto speedAt = state->source.find(speedProperty);
    requirePlay(speedAt != std::string::npos, "M5 sample Spin property insertion point missing");
    state->source.insert(speedAt, "{\"target\",PropertyType::EntityRef,EntityRef{}, {}, {}, \"Ціль\"},\n        ");
    atomicWrite(state->root / "Code/Behaviors.cpp", state->source);
    openProject(state->root);
    codeSmokeContract();
    codePanelProbe_ = {};
    codePanelProbe_.enabled = true;
    codePanelProbe_.original = readDocument(state->root / "Code/Behaviors.cpp");
    document_.selection = EntityId::parse("50000000-0000-4000-8000-000000000010");
    auto record = document_.scene.record(document_.scene.find(document_.selection));
    record.transform.position.x += .25f;
    document_.edit(record);
    state->selection = document_.selection;
    state->documentBefore = encodeScene(document_.scene);
    state->undo = document_.undoCount();
    playSmoke_ = std::move(state);
    focusSelection();
}
bool Editor::playSmokePassed() const {
    return playSmoke_ && playSmoke_->passed;
}
void Editor::playSmokeStep(uint64_t frame) {
    if (!playSmoke_ || playSmoke_->passed || frame < 6)
        return;
    auto& test = *playSmoke_;
    constexpr std::string_view asciiProbe = "Probe";
    constexpr std::string_view ukrainianProbe = "Український рядок для M5";
    if (!stringProbe_.passed) {
        ++stringProbe_.frames;
        if (stringProbe_.frames >= 180) {
            const auto* imgui = ImGui::GetCurrentContext();
            throw std::runtime_error("String property probe timed out stage=" + std::to_string(stringProbe_.stage) +
                                     " textQueued=" + std::to_string(stringProbe_.textQueued) +
                                     " value=" + stringProbe_.value + " input=" +
                                     std::to_string(stringProbe_.inputMin.x) + "," +
                                     std::to_string(stringProbe_.inputMax.x) + " active=" +
                                     std::to_string(imgui ? imgui->ActiveId : 0) + " prev=" +
                                     std::to_string(imgui ? imgui->ActiveIdPreviousFrame : 0) + " nav=" +
                                     std::to_string(imgui ? imgui->NavId : 0) + " hovered=" +
                                     std::to_string(imgui ? imgui->HoveredId : 0) + " mouseDown=" +
                                     std::to_string(imgui && imgui->IO.MouseDown[0]) + " clicked=" +
                                     std::to_string(imgui && imgui->IO.MouseClicked[0]));
        }
        const auto center = ImVec2((stringProbe_.inputMin.x + stringProbe_.inputMax.x) * .5f,
                                   (stringProbe_.inputMin.y + stringProbe_.inputMax.y) * .5f);
        const auto queueMouse = [this](ImVec2 position, bool down) {
            testInput_ = [position, down] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(position.x, position.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            };
        };
        if (stringProbe_.stage == 0 && stringProbe_.inputMax.x > stringProbe_.inputMin.x) {
            queueMouse(center, true);
            stringProbe_.stage = 1;
        } else if (stringProbe_.stage == 1) {
            testInput_ = [center] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(center.x, center.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            };
            stringProbe_.stage = 2;
        } else if (stringProbe_.stage == 2 && !stringProbe_.textQueued) {
            testInput_ = [] { ImGui::GetIO().AddInputCharactersUTF8("Probe"); };
            stringProbe_.textQueued = true;
        } else if (stringProbe_.stage == 2 && stringProbe_.value == asciiProbe) {
            queueMouse(center, true);
            stringProbe_.stage = 3;
        } else if (stringProbe_.stage == 3) {
            testInput_ = [center] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(center.x, center.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            };
            stringProbe_.stage = 4;
            stringProbe_.textQueued = false;
        } else if (stringProbe_.stage == 4 && !stringProbe_.textQueued) {
            testInput_ = [] {
                auto& io = ImGui::GetIO();
                io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
                io.AddKeyEvent(ImGuiKey_A, true);
                io.AddKeyEvent(ImGuiKey_A, false);
                io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
                io.AddInputCharactersUTF8("Український рядок для M5");
            };
            stringProbe_.textQueued = true;
            stringProbe_.stage = 5;
        } else if (stringProbe_.stage == 5 && stringProbe_.value == ukrainianProbe) {
            stringProbe_.passed = true;
            stringProbe_.enabled = false;
        }
    }
    if (stringProbe_.passed && !codePanelProbe_.passed) {
        ++codePanelProbe_.frames;
        if (codePanelProbe_.frames >= 240) {
            const auto* imgui = ImGui::GetCurrentContext();
            throw std::runtime_error("Code panel native probe timed out stage=" +
                                     std::to_string(codePanelProbe_.stage) +
                                     " dirty=" + std::to_string(codeDirty()) +
                                     " focused=" + std::to_string(codeUi_ ? codeUi_->focused : false) +
                                     " input=" + std::to_string(codePanelProbe_.inputMin.x) + "," +
                                     std::to_string(codePanelProbe_.inputMax.x) + " y=" +
                                     std::to_string(codePanelProbe_.inputMin.y) + "," +
                                     std::to_string(codePanelProbe_.inputMax.y) +
                                     " save=" + std::to_string(codePanelProbe_.saveMin.x) + "," +
                                     std::to_string(codePanelProbe_.saveMax.x) + " value=" +
                                     (codeUi_ ? codeUi_->text.substr(0, 48) : std::string("<none>")) +
                                     " active=" + std::to_string(imgui ? imgui->ActiveId : 0) +
                                     " prev=" + std::to_string(imgui ? imgui->ActiveIdPreviousFrame : 0) +
                                     " nav=" + std::to_string(imgui ? imgui->NavId : 0) +
                                     " hovered=" + std::to_string(imgui ? imgui->HoveredId : 0) +
                                     " mouseDown=" + std::to_string(imgui && imgui->IO.MouseDown[0]) +
                                     " clicked=" + std::to_string(imgui && imgui->IO.MouseClicked[0]) +
                                     " ctrl=" + std::to_string(imgui && imgui->IO.KeyCtrl) +
                                     " inputActive=" + std::to_string(codePanelProbe_.inputActive) +
                                     " inputFocused=" + std::to_string(codePanelProbe_.inputFocused) +
                                     " inputEdited=" + std::to_string(codePanelProbe_.inputEdited) +
                                     " inputId=" + std::to_string(codePanelProbe_.inputId) +
                                     " widget=" + std::to_string(codeUi_ ? codeUi_->widgetRevision : 0));
        }
        const auto inputCenter = ImVec2((codePanelProbe_.inputMin.x + codePanelProbe_.inputMax.x) * .5f,
                                        (codePanelProbe_.inputMin.y + codePanelProbe_.inputMax.y) * .5f);
        const auto saveCenter = ImVec2((codePanelProbe_.saveMin.x + codePanelProbe_.saveMax.x) * .5f,
                                       (codePanelProbe_.saveMin.y + codePanelProbe_.saveMax.y) * .5f);
        const auto queueMouse = [this](ImVec2 position, bool down) {
            testInput_ = [position, down] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(position.x, position.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            };
        };
        if (codePanelProbe_.stage == 0 && codePanelProbe_.inputMax.x > codePanelProbe_.inputMin.x &&
            codePanelProbe_.saveMax.x > codePanelProbe_.saveMin.x) {
            queueMouse(inputCenter, true);
            codePanelProbe_.stage = 1;
        } else if (codePanelProbe_.stage == 1) {
            testInput_ = [inputCenter] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(inputCenter.x, inputCenter.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            };
            codePanelProbe_.stage = 2;
        } else if (codePanelProbe_.stage == 2 && !codeDirty() && !codePanelProbe_.textQueued) {
            testInput_ = [] {
                ImGui::GetIO().AddInputCharactersUTF8("\n// native UI save probe\n");
            };
            codePanelProbe_.textQueued = true;
        } else if (codePanelProbe_.stage == 2 && codeDirty()) {
            codePanelProbe_.edited = codeUi_ ? codeUi_->text : std::string{};
            queueMouse(saveCenter, true);
            codePanelProbe_.stage = 3;
        } else if (codePanelProbe_.stage == 3) {
            queueMouse(saveCenter, false);
            codePanelProbe_.stage = 4;
        } else if (codePanelProbe_.stage == 4 && !codeDirty()) {
            requirePlay(!codePanelProbe_.edited.empty() &&
                            readDocument(project_->root() / "Code/Behaviors.cpp") == codePanelProbe_.edited,
                        "Code panel Save button did not publish the edited source");
            document_.undo();
            codePanelProbe_.stage = 5;
        } else if (codePanelProbe_.stage == 5 && !codeDirty()) {
            requirePlay(readDocument(project_->root() / "Code/Behaviors.cpp") == codePanelProbe_.original && codeUi_ &&
                            codeUi_->text == codePanelProbe_.original,
                        "Code panel Undo did not restore the original source");
            codePanelProbe_.external = codePanelProbe_.original + "\n// external watcher refresh\n";
            atomicWrite(project_->root() / "Code/Behaviors.cpp", codePanelProbe_.external);
            if (projectUi_) {
                projectUi_->refreshPending = true;
                projectUi_->changedAt = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() - 1.0;
            }
            codePanelProbe_.stage = 6;
        } else if (codePanelProbe_.stage == 6 && !codeDirty() && codeUi_ && codeUi_->text == codePanelProbe_.external &&
                   std::string(codeUi_->buffer.data()) == codePanelProbe_.external) {
            atomicWrite(project_->root() / "Code/Behaviors.cpp", codePanelProbe_.original);
            if (projectUi_) {
                projectUi_->refreshPending = true;
                projectUi_->changedAt = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() - 1.0;
            }
            codePanelProbe_.stage = 7;
        } else if (codePanelProbe_.stage == 7 && !codeDirty() && codeUi_ && codeUi_->text == codePanelProbe_.original &&
                   std::string(codeUi_->buffer.data()) == codePanelProbe_.original) {
            codePanelProbe_.passed = true;
            codePanelProbe_.enabled = false;
        }
    }
    const auto unchanged = [&] {
        requirePlay(document_.dirty(), "Play lost dirty state");
        requirePlay(document_.selection == test.selection, "Play lost selection");
        requirePlay(document_.undoCount() == test.undo, "Play changed Undo history");
        requirePlay(encodeScene(document_.scene) == test.documentBefore, "Player replaced editor scene");
    };
    const auto parameter = [&](double speed) {
        auto record = document_.scene.record(document_.scene.find(test.selection));
        record.behaviors[0].properties["speedDegreesPerSecond"] = speed;
        document_.edit(record);
        test.documentBefore = encodeScene(document_.scene);
        test.undo = document_.undoCount();
    };
    if (test.stage == 0) {
        if (!stringProbe_.passed || !codePanelProbe_.passed)
            return;
        if (!test.schemaRecoveryStarted) {
            test.missingTarget = EntityId::create();
            auto record = document_.scene.record(document_.scene.find(test.selection));
            record.behaviors.at(0).properties["target"] = sdk::EntityRef{test.missingTarget};
            document_.edit(record, "Перевірка відновлення схеми");
            test.schemaRecoveryStarted = true;
            requestBuild(false);
            test.stage = 1;
            return;
        }
        requestBuild(false);
        test.stage = 1;
        return;
    }
    if (test.stage == 1 && !playBusy()) {
        if (!test.schemaRecoveryChecked) {
            requirePlay(playUi_ && !playUi_->lastSucceeded && behaviorSchema() && behaviorSchema()->types.size() == 3,
                        "Schema recovery build did not publish the discovered schema");
            const auto* spin = behaviorSchema()->find(BehaviorTypeId::parse("50000000-0000-4000-8000-000000000001"));
            requirePlay(spin != nullptr && std::any_of(spin->properties.begin(), spin->properties.end(),
                                                       [](const auto& property) {
                                                           return property.name == "target" &&
                                                                  property.type == sdk::PropertyType::EntityRef;
                                                       }),
                        "Schema recovery did not publish the EntityRef descriptor");
            const auto current = document_.scene.record(document_.scene.find(test.selection));
            const auto found = current.behaviors.at(0).properties.find("target");
            requirePlay(found != current.behaviors.at(0).properties.end() &&
                            std::get<sdk::EntityRef>(found->second).id == test.missingTarget,
                        "Schema recovery lost the stored missing EntityRef");
            document_.undo();
            test.schemaRecoveryChecked = true;
            requestBuild(false);
            return;
        }
        requirePlay(playUi_ && playUi_->lastSucceeded && behaviorSchema() && behaviorSchema()->types.size() == 3,
                    "Project C++ build/schema discovery failed");
        unchanged();
        playUi_->extraPlayerArguments = {"--frames",
                                         "90",
                                         "--fixed-dt",
                                         "0.0166666666666667",
                                         "--test-input",
                                         "--capture",
                                         utf8((test.output / "m5-player.png").wstring()),
                                         "--report",
                                         utf8((test.output / "m5-player.json").wstring())};
        requestBuild(true);
        test.stage = 2;
        return;
    }
    if (test.stage == 2) {
        if (playerActive()) {
            if (!test.sawRunning) {
                test.sawRunning = true;
                test.pausedFrames = renderer_.sceneRenderFrames();
            }
            requirePlay(renderer_.sceneRenderFrames() == test.pausedFrames,
                        "Editor rendered 3D while Player was active");
        }
        if (playBusy())
            return;
        requirePlay(playUi_->lastSucceeded && test.sawRunning, "Normal Player failed");
        unchanged();
        JsonDoc report(readDocument(test.output / "m5-player.json"));
        requirePlay(yyjson_is_true(get(report.root(), "passed")), "Player report failed");
        requirePlay(num(get(report.root(), "starts")) == 3 && num(get(report.root(), "stops")) == 3,
                    "Player lifecycle counts");
        auto* finalScene = get(report.root(), "finalScene");
        char* bytes = yyjson_val_write(finalScene, 0, nullptr);
        requirePlay(bytes != nullptr, "Final Player scene report missing");
        const std::string finalText(bytes);
        free(bytes);
        auto runtimeScene = decodeScene(finalText, document_.scene.assets);
        const auto final = runtimeScene.record(runtimeScene.find(test.selection));
        const auto original = document_.scene.record(document_.scene.find(test.selection));
        requirePlay(final.transform.position.x > original.transform.position.x + .9f &&
                        final.transform.position.y > original.transform.position.y + .49f,
                    "Runtime keyboard action failed");
        requirePlay(final.transform.rotation != original.transform.rotation, "C++ Spin did not rotate");
        const auto lamp = EntityId::parse("004de170-579e-4f2c-8d1f-35903540a74a");
        requirePlay(runtimeScene.transform(runtimeScene.find(lamp)).local.position !=
                        document_.scene.transform(document_.scene.find(lamp)).local.position,
                    "C++ MoveLight did not move");
        requirePlay(num(get(report.root(), "shadow_faces_total")) > 90 * 6,
                    "Moving light did not refresh dynamic shadow faces");
        atomicWrite(test.root / "Code/Behaviors.cpp", test.source + "\nthis_is_an_intentional_compile_error;\n");
        requestBuild(true);
        test.stage = 3;
        return;
    }
    if (test.stage == 3 && !playBusy()) {
        requirePlay(!playUi_->lastSucceeded && !playerActive(), "Compile failure launched stale Player");
        requirePlay(!playUi_->diagnostics.empty(), "Compiler diagnostics missing");
        bool fileLine{};
        for (const auto& diagnostic : playUi_->diagnostics)
            fileLine |= diagnostic.sourcePath.has_value() && diagnostic.line.value_or(0) > 0;
        requirePlay(fileLine, "Compiler file/line diagnostic missing");
        unchanged();
        test.compileFailure = true;
        atomicWrite(test.root / "Code/Behaviors.cpp", test.source);
        parameter(-359);
        playUi_->extraPlayerArguments.clear();
        requestBuild(true);
        test.stage = 4;
        return;
    }
    if (test.stage == 4 && !playBusy()) {
        requirePlay(!playUi_->lastSucceeded && !playerActive(), "C++ runtime crash was not isolated");
        unchanged();
        test.crash = true;
        parameter(-358);
        requestBuild(true);
        test.stage = 5;
        test.sawRunning = false;
        return;
    }
    if (test.stage == 5 && playerActive()) {
        if (!test.sawRunning) {
            test.started = Clock::now();
            test.sawRunning = true;
        }
        if (milliseconds(test.started) > 600) {
            stopPlay();
            test.started = Clock::now();
            test.stage = 6;
        }
        return;
    }
    if (test.stage == 6 && !playBusy()) {
        requirePlay(playUi_->forced && milliseconds(test.started) < 6000, "Hung Player did not stop within bound");
        unchanged();
        test.forced = true;
        parameter(30);
        playUi_->extraPlayerArguments = {"--frames",   "24",
                                         "--fixed-dt", "0.0166666666666667",
                                         "--report",   utf8((test.output / "m5-restart.json").wstring())};
        requestBuild(true);
        test.stage = 7;
        return;
    }
    if (test.stage == 7 && !playBusy()) {
        requirePlay(playUi_->lastSucceeded, "Player did not restart after failures");
        unchanged();
        test.restart = true;
        test.passed = true;
        playUi_->showLog = false;
        atomicWrite(test.output / "m5-editor-contract.json",
                    "{\"passed\":true,\"schema_discovery\":true,\"keyboard_spin_moving_light\":true,\"paused_editor_"
                    "renderer\":true,"
                    "\"compile_error_file_line\":true,\"no_stale_launch\":true,\"runtime_crash_isolated\":true,"
                    "\"hung_player_stopped\":true,\"restart\":true,\"unsaved_scene_and_history_preserved\":true,"
                    "\"string_input_probe\":true,\"code_panel_native_save_undo\":true,"
                    "\"code_panel_external_refresh\":true,\"schema_recovery\":true}");
    }
}
} // namespace proto
