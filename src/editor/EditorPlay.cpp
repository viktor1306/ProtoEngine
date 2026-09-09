#include "editor/Editor.hpp"
#include "editor/EditorPlayState.hpp"
#include "project/ProjectSession.hpp"
#include "project/PlayAssets.hpp"
#include "assets/AssetIO.hpp"
#include <Proto/Build.hpp>
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <algorithm>
#include <imgui_internal.h>

namespace proto {
PlayUiState::~PlayUiState() {
    if (ready)
        CloseHandle(ready);
}
void PlayUiState::append(std::string_view text) {
    std::scoped_lock lock(logMutex);
    log.append(text);
    if (log.size() > 256 * 1024)
        log.erase(0, log.size() - 256 * 1024);
}
namespace {
std::filesystem::path sdkDirectory() {
    const auto bin = executableDirectory();
    const auto local = bin.parent_path() / "sdk";
    if (!std::filesystem::exists(local / "sdk.json"))
        throw std::runtime_error("Пакет SDK відсутній. Зберіть ціль ProtoSDK для цієї конфігурації.");
    return local;
}
ProcessStats selfStats() {
    ProcessStats result;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        result.cpuSeconds = double(ticks(kernel) + ticks(user)) / 10000000.0;
    }
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        result.workingSetBytes = memory.WorkingSetSize;
    return result;
}
const char* phaseName(PlayPhase phase) {
    switch (phase) {
    case PlayPhase::Preparing:
        return "Компіляція C++ / підготовка";
    case PlayPhase::Starting:
        return "Запуск Player";
    case PlayPhase::Running:
        return "Player працює";
    case PlayPhase::Stopping:
        return "Зупинення";
    default:
        return "Редагування";
    }
}
} // namespace
bool Editor::playBusy() const {
    return playUi_ && playUi_->phase != PlayPhase::Editing;
}
bool Editor::playerActive() const {
    return playUi_ && playUi_->sessionLaunched;
}
const BehaviorSchema* Editor::behaviorSchema() const {
    return playUi_ && playUi_->schema ? &*playUi_->schema : nullptr;
}
void Editor::requestBuild(bool play) {
    if (playBusy())
        return;
    if (!project_) {
        showError(std::runtime_error("Створіть або відкрийте проєкт перед компіляцією C++"));
        return;
    }
    if (importJob_.busy() || gizmoActive() || codeDirty()) {
        showError(std::runtime_error("Завершіть імпорт/перетягування та збережіть код C++ перед збіркою"));
        return;
    }
    try {
        if (queuedEdit_) {
            auto edit = std::move(queuedEdit_);
            queuedEdit_ = {};
            edit();
        }
        document_.endGesture();
        project_->refresh();
        const auto additionalRoots = project_->buildSettings().additionalAssets;
        if (!playUi_)
            playUi_ = std::make_shared<PlayUiState>();
        auto& ui = *playUi_;
        ui.cancel->store(false);
        ui.launch = play;
        ui.forced = false;
        ui.lastSucceeded = false;
        ui.showLog = true;
        ui.diagnostics.clear();
        {
            std::scoped_lock lock(ui.logMutex);
            ui.log.clear();
        }
        ProjectBuildRequest request{project_->root(), sdkDirectory(), sdk::configuration};
        request.expectedSdkBuildId = sdk::buildId;
        auto scene = Scene::fromSnapshot(document_.scene.snapshot(), document_.scene.assets);
        const auto weak = std::weak_ptr<PlayUiState>(playUi_);
        ui.preparation = std::async(
            std::launch::async, [request, play, cancel = ui.cancel, weak, additionalRoots,
                                scene = std::move(scene)]() mutable {
                auto build = buildProject(request, *cancel, [weak](const BuildProgress& progress) {
                    if (const auto state = weak.lock())
                        state->append("[" + progress.stage + "] " + progress.message + "\n");
                });
                if (cancel->load())
                    throw std::runtime_error("Збірку скасовано");
                std::optional<PlaySnapshot> snapshot;
                std::string sceneError;
                try {
                    (void)preparePlayAssets(scene, request.projectRoot, build.schema, additionalRoots);
                    validateSceneBehaviors(scene, build.schema);
                    if (play)
                        snapshot = stagePlaySnapshot(request.projectRoot, scene, build.schema.sdkBuildId);
                } catch (const std::exception& error) {
                    sceneError = error.what();
                }
                if (cancel->load())
                    throw std::runtime_error("Підготовку скасовано");
                return PlayPreparation{std::move(build), std::move(snapshot), std::move(sceneError)};
            });
        ui.phase = PlayPhase::Preparing;
        ui.phaseStarted = Clock::now();
        renderer_.setScenePaused(true);
    } catch (const std::exception& e) {
        showError(e);
    }
}
void Editor::stopPlay() {
    if (!playBusy())
        return;
    auto& ui = *playUi_;
    if (ui.phase == PlayPhase::Stopping)
        return;
    ui.cancel->store(true);
    if (ui.player.running())
        ui.player.signalStop();
    ui.phase = PlayPhase::Stopping;
    ui.phaseStarted = Clock::now();
}
void Editor::requestExport() {
    if (playBusy())
        return;
    if (!project_) {
        showError(std::runtime_error("Створіть або відкрийте проєкт перед експортом"));
        return;
    }
    if (importJob_.busy() || gizmoActive() || codeDirty()) {
        showError(std::runtime_error("Завершіть імпорт/перетягування та збережіть код C++ перед експортом"));
        return;
    }
    try {
        if (queuedEdit_) {
            auto edit = std::move(queuedEdit_);
            queuedEdit_ = {};
            edit();
        }
        document_.endGesture();
        if (document_.dirty())
            throw std::runtime_error("Експорт використовує збережену стартову сцену. Спочатку збережіть поточні правки (Ctrl+S).");
        project_->refresh();
        if (!playUi_)
            playUi_ = std::make_shared<PlayUiState>();
        auto& ui = *playUi_;
        ui.cancel->store(false);
        ui.launch = false;
        ui.forced = false;
        ui.lastSucceeded = false;
        ui.showLog = true;
        ui.diagnostics.clear();
        {
            std::scoped_lock lock(ui.logMutex);
            ui.log.clear();
        }
        ProjectPackageRequest request;
        request.session = project_;
        request.sdkRoot = sdkDirectory();
        request.outputDirectory = project_->root() / "Build" / utf8Path(project_->buildSettings().name);
        request.expectedSdkBuildId = sdk::buildId;
        request.configuration = sdk::configuration;
        const auto weak = std::weak_ptr<PlayUiState>(playUi_);
        ui.packaging = std::async(std::launch::async, [request, weak, cancel = ui.cancel] {
            return buildProjectPackage(request, *cancel, [weak](const BuildProgress& progress) {
                if (const auto state = weak.lock())
                    state->append("[" + progress.stage + "] " + progress.message + "\n");
            });
        });
        ui.phase = PlayPhase::Preparing;
        ui.phaseStarted = Clock::now();
        renderer_.setScenePaused(true);
        status_ = "Експорт збереженого проєкту…";
    } catch (const std::exception& error) {
        showError(error);
    }
}
void Editor::shutdownPlay() {
    if (!playUi_)
        return;
    auto& ui = *playUi_;
    ui.cancel->store(true);
    try {
        ui.player.forceStop();
    } catch (...) {
    }
    if (ui.preparation.valid()) {
        ui.preparation.wait();
        try {
            (void)ui.preparation.get();
        } catch (...) {
        }
    }
    if (ui.packaging.valid()) {
        ui.packaging.wait();
        try {
            (void)ui.packaging.get();
        } catch (...) {
        }
    }
    ui.snapshot.reset();
    ui.sessionLaunched = false;
    ui.phase = PlayPhase::Editing;
    renderer_.setScenePaused(false);
}
void Editor::pollPlay() {
    if (!playUi_)
        return;
    auto& ui = *playUi_;
    const auto finish = [&](bool success, std::string message) {
        ui.lastSucceeded = success;
        ui.sessionLaunched = false;
        ui.append(message + "\n");
        ui.snapshot.reset();
        if (ui.ready) {
            CloseHandle(ui.ready);
            ui.ready = nullptr;
        }
        ui.phase = PlayPhase::Editing;
        renderer_.setScenePaused(false);
        status_ = std::move(message);
    };
    if (ui.packaging.valid() && ui.packaging.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            ui.lastPackage = ui.packaging.get();
            ui.schema = ui.lastPackage->schema;
            ui.append("Папка: " + utf8(ui.lastPackage->packageRoot.wstring()) + "\n");
            ui.append("Файлів: " + std::to_string(ui.lastPackage->runtimeStats.files) + " · " +
                      std::to_string(ui.lastPackage->runtimeStats.bytes / 1048576) + " MiB\n");
            finish(true, "Експорт готовий: " + utf8(ui.lastPackage->executable.filename().wstring()));
        } catch (const ProjectPackageError& error) {
            ui.diagnostics = error.diagnostics();
            finish(false, error.what());
        } catch (const std::exception& error) {
            finish(false, error.what());
        }
    }
    if (ui.preparation.valid() && ui.preparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto prepared = ui.preparation.get();
            ui.schema = prepared.build.schema;
            ui.diagnostics = std::move(prepared.build.diagnostics);
            // Publish discovery even when scene validation fails, so the
            // inspector has the schema needed to repair the stored values.
            if (!prepared.sceneError.empty()) {
                finish(false, prepared.sceneError);
                return;
            }
            if (ui.cancel->load()) {
                finish(false, "Підготовку скасовано");
                return;
            }
            if (!ui.launch) {
                finish(true, "C++ зібрано. Схеми поведінок оновлено.");
                return;
            }
            ui.snapshot = std::move(prepared.snapshot);
            if (ui.ready)
                CloseHandle(ui.ready);
            ui.readyName = L"Local\\ProtoPlayer.Ready." + std::wstring(utf8Path(Uuid::create().string()).native());
            ui.ready = CreateEventW(nullptr, TRUE, FALSE, ui.readyName.c_str());
            if (!ui.ready)
                throw std::runtime_error("Cannot create Player ready event");
            const auto stop = ui.player.prepareStopEvent();
            ProcessSpec process{prepared.build.executable,
                                {"--snapshot", utf8(ui.snapshot->manifest.wstring()), "--stop-event", utf8(stop),
                                 "--ready-event", utf8(ui.readyName)},
                                prepared.build.executable.parent_path()};
            process.arguments.insert(process.arguments.end(), ui.extraPlayerArguments.begin(),
                                     ui.extraPlayerArguments.end());
            ui.player.start(process);
            ui.sessionLaunched = true;
            ui.phase = PlayPhase::Starting;
            ui.phaseStarted = Clock::now();
            ui.pausedSceneFrames = renderer_.sceneRenderFrames();
            ui.metricsAt = Clock::now();
            ui.editorStats = selfStats();
            ui.playerStats = ui.player.stats();
            ui.append("Player PID " + std::to_string(ui.player.processId()) + "\n");
        } catch (const ProjectBuildError& e) {
            ui.diagnostics = e.diagnostics();
            finish(false, e.what());
        } catch (const std::exception& e) {
            finish(false, e.what());
        }
    }
    if (ui.sessionLaunched) {
        try {
            ui.player.poll();
            ui.append(ui.player.takeStdout());
            ui.append(ui.player.takeStderr());
            if (ui.phase == PlayPhase::Starting && ui.ready && WaitForSingleObject(ui.ready, 0) == WAIT_OBJECT_0) {
                ui.phase = PlayPhase::Running;
                ui.phaseStarted = Clock::now();
                ui.append("Player готовий. Рендеринг сцени редактора призупинено.\n");
            }
            if (ui.phase == PlayPhase::Starting && milliseconds(ui.phaseStarted) > 15000) {
                ui.append("Player не відповів за 15 с. Зупинення…\n");
                stopPlay();
            }
            if (ui.phase == PlayPhase::Stopping && milliseconds(ui.phaseStarted) > 1500) {
                ui.player.forceStop();
                ui.forced = true;
            }
            if (!ui.player.running()) {
                const auto exit = ui.player.exitCode().value_or(1);
                finish(exit == 0 && !ui.forced,
                       ui.forced   ? "Player примусово зупинено. Сцена редактора збережена в пам’яті."
                       : exit == 0 ? "Player завершено. Можна продовжувати редагування."
                                   : "Player завершився з помилкою, код " + std::to_string(exit));
            }
        } catch (const std::exception& e) {
            try {
                ui.player.forceStop();
            } catch (...) {
            }
            finish(false, e.what());
        }
    } else if (ui.phase == PlayPhase::Stopping && !ui.preparation.valid() && !ui.packaging.valid())
        finish(false, "Зупинено");
    if (milliseconds(ui.metricsAt) >= 500) {
        const auto editor = selfStats(), player = ui.player.stats();
        const double seconds = milliseconds(ui.metricsAt) / 1000;
        ui.editorCpuPercent = std::max(0.0, (editor.cpuSeconds - ui.editorStats.cpuSeconds) / seconds * 100);
        ui.playerCpuPercent = std::max(0.0, (player.cpuSeconds - ui.playerStats.cpuSeconds) / seconds * 100);
        ui.editorStats = editor;
        ui.playerStats = player;
        ui.metricsAt = Clock::now();
    }
}
void Editor::playToolbar() {
    ImGui::BeginDisabled(!project_ || playBusy() || codeDirty() || importJob_.busy() || gizmoActive());
    if (ImGui::Button("Зібрати C++"))
        requestBuild(false);
    ImGui::SameLine();
    if (ImGui::Button("Play"))
        requestBuild(true);
    ImGui::SameLine();
    if (ImGui::Button("Експорт .exe"))
        requestExport();
    if (exportSmoke_ || acceptanceSmoke_) {
        exportButtonMin_ = ImGui::GetItemRectMin();
        exportButtonMax_ = ImGui::GetItemRectMax();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Зібрати збережену стартову сцену до папки Build проєкту");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!playBusy());
    if (ImGui::Button("Stop"))
        stopPlay();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Журнал")) {
        if (!playUi_)
            playUi_ = std::make_shared<PlayUiState>();
        playUi_->showLog = true;
    }
    ImGui::SameLine();
    if (playUi_)
        ImGui::TextDisabled("%s", phaseName(playUi_->phase));
    ImGui::Separator();
}
void Editor::buildLogPanel() {
    if (!playUi_ || !playUi_->showLog)
        return;
    auto& ui = *playUi_;
    ImGui::SetNextWindowSize({780 * appliedScale_, 330 * appliedScale_}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("C++ / Player — журнал###BuildLog", &ui.showLog)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(phaseName(ui.phase));
    ImGui::SameLine();
    ImGui::BeginDisabled(!playBusy());
    if (ImGui::Button("Stop##Log"))
        stopPlay();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Редактор: CPU %.1f%% ядра · RAM %.1f MiB | Player: CPU %.1f%% ядра · RAM %.1f MiB",
                        ui.editorCpuPercent, double(ui.editorStats.workingSetBytes) / 1048576, ui.playerCpuPercent,
                        double(ui.playerStats.workingSetBytes) / 1048576);
    if (ui.lastPackage) {
        ImGui::BeginDisabled(playBusy());
        if (ImGui::Button("Папка останньої збірки")) {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                ui.lastPackage->packageRoot.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32)
                showError(std::runtime_error("Не вдалося відкрити папку збірки"));
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    for (size_t i = 0; i < ui.diagnostics.size(); ++i) {
        const auto& diagnostic = ui.diagnostics[i];
        ImGui::PushID(static_cast<int>(i));
        if (diagnostic.sourcePath) {
            const auto label =
                *diagnostic.sourcePath + ":" + std::to_string(diagnostic.line.value_or(1)) + "  " + diagnostic.message;
            if (ImGui::Selectable(label.c_str()))
                try {
                    openCode(utf8Path(*diagnostic.sourcePath), diagnostic.line.value_or(1));
                } catch (const std::exception& e) {
                    showError(e);
                }
        } else
            ImGui::TextWrapped("%s", diagnostic.message.c_str());
        ImGui::PopID();
    }
    ImGui::BeginChild("Output", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    {
        std::scoped_lock lock(ui.logMutex);
        ImGui::TextUnformatted(ui.log.c_str());
    }
    if (follow)
        ImGui::SetScrollHereY(1);
    ImGui::EndChild();
    ImGui::End();
}
} // namespace proto
