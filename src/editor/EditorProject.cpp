#include "editor/Editor.hpp"
#include "editor/FileDialog.hpp"
#include "editor/ProjectUiState.hpp"
#include "project/ProjectSession.hpp"
#include "assets/AssetIO.hpp"
#include "renderer/AssetRenderer.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>

namespace proto {
namespace {
namespace fs = std::filesystem;
ImVec2 itemCenter() {
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    return {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
}
bool under(const std::string& path, const std::string& parent) {
    const auto p = projectPathKey(path), base = projectPathKey(parent);
    return p == base || (p.size() > base.size() && p.starts_with(base) && p[base.size()] == '/');
}
std::string follow(std::string path, const std::vector<PathMove>& moves, bool forward) {
    const PathMove* chosen{};
    for (const auto& move : moves) {
        const auto& source = forward ? move.before : move.after;
        if (under(path, source) && (!chosen || source.size() > (forward ? chosen->before : chosen->after).size()))
            chosen = &move;
    }
    if (chosen) {
        const auto& from = forward ? chosen->before : chosen->after;
        path = (forward ? chosen->after : chosen->before) + path.substr(from.size());
    }
    return path;
}
std::string join(const std::string& parent, const std::string& child) {
    return parent.empty() ? child : parent + "/" + child;
}
std::string parent(const std::string& path) {
    return utf8(utf8Path(path).parent_path().generic_wstring());
}
std::vector<std::string> selected(const ProjectUiState& ui) {
    return {ui.selection.begin(), ui.selection.end()};
}
void nameBuffer(ProjectUiState& ui, const std::string& value) {
    ui.name.fill(0);
    std::memcpy(ui.name.data(), value.data(), std::min(value.size(), ui.name.size() - 1));
}
struct ProjectFileAction final : DocumentAction {
    std::shared_ptr<FileTransaction> transaction;
    std::function<void()> preflight;
    std::function<void(bool)> reconcile;
    void apply(bool forward) override {
        preflight();
        if (forward)
            transaction->redo();
        else
            transaction->undo();
        try {
            reconcile(forward);
        } catch (...) {
            const auto original = std::current_exception();
            if (forward)
                transaction->undo();
            else
                transaction->redo();
            // Reconciliation builds a candidate and publishes only on success.
            std::rethrow_exception(original);
        }
    }
    size_t bytes() const override { return sizeof(*this) + transaction->memoryBytes(); }
    std::string label() const override { return transaction->label(); }
};
} // namespace

bool Editor::fileInputFocused() const {
    return projectUi_ && projectUi_->focused;
}
void Editor::acceptProject(std::shared_ptr<ProjectSession> candidate) {
    if (playBusy() || codeDirty())
        throw std::runtime_error("Завершіть Play та збережіть зміни C++ перед відкриттям іншого проєкту");
    if (importJob_.busy())
        throw std::runtime_error("Дочекайтеся завершення імпорту");
    SceneDocument loaded;
    loaded.loadProject(candidate->startupScenePath(), candidate->assetWorkspace(), false);
    auto ui = std::make_shared<ProjectUiState>();
    refreshProjectFiles(candidate, ui.get());
    // The previous document/history releases its journal handles before its
    // project lock is released. Failed candidate loading leaves it untouched.
    document_ = std::move(loaded);
    shutdownPlay();
    playUi_.reset();
    codeUi_.reset();
    project_ = std::move(candidate);
    projectUi_ = std::move(ui);
    camera_ = {};
    useSceneCamera_ = false;
    status_ = "Проєкт відкрито: " + project_->info().name;
}
void Editor::openProject(const fs::path& path) {
    if (project_ &&
        fs::equivalent(path.has_filename() && path.filename() == "project.proto.json" ? path.parent_path() : path,
                       project_->root())) {
        status_ = "Цей проєкт уже відкрито";
        return;
    }
    acceptProject(ProjectSession::open(path));
}
void Editor::createProject(const fs::path& path, const std::string& name) {
    acceptProject(ProjectSession::create(path, name));
}
void Editor::openProjectScene(const fs::path& path) {
    if (!project_) {
        document_.load(path);
        return;
    }
    const auto relative = projectRelative(project_->root(), path);
    projectPath(project_->root(), relative);
    if (!under(relative, "Scenes") || !projectPathKey(relative).ends_with(".SCENE.JSON"))
        throw std::runtime_error("Оберіть сцену всередині папки Scenes цього проєкту");
    document_.loadProject(path, project_->assetWorkspace());
    camera_ = {};
    useSceneCamera_ = false;
}
void Editor::performProjectAction(Action action) {
    if (importJob_.busy())
        throw std::runtime_error("Дочекайтеся завершення імпорту");
    if (!projectUi_)
        projectUi_ = std::make_shared<ProjectUiState>();
    if (action == Action::OpenFileScene) {
        openProjectScene(pendingScenePath_);
        pendingScenePath_.clear();
    } else if (action == Action::OpenProject) {
        if (const auto path = projectFolderDialog(window_, false))
            openProject(*path);
    } else if (action == Action::NewProject) {
        if (const auto path = projectFolderDialog(window_, true)) {
            projectUi_->createParent = *path;
            nameBuffer(*projectUi_, "Мій проєкт");
            projectUi_->createPopup = true;
        }
    }
}
void Editor::refreshProjectFiles(std::shared_ptr<ProjectSession> project, ProjectUiState* target) {
    if (!project)
        project = project_;
    if (!target)
        target = projectUi_.get();
    if (!project || !target)
        return;
    auto& ui = *target;
    auto directory = ui.directory;
    while (!fs::is_directory(projectPath(project->root(), directory)) && !directory.empty())
        directory = parent(directory);
    std::vector<ProjectUiState::Entry> entries;
    auto assets = inspectProjectAssets(project->root());
    std::map<std::string, const ProjectAsset*> byPath;
    for (const auto& asset : assets)
        if (asset.owner.empty())
            byPath[projectPathKey(asset.path)] = &asset;
    for (const auto& file : fs::directory_iterator(projectPath(project->root(), directory))) {
        const auto path = utf8(file.path().lexically_relative(project->root()).generic_wstring());
        if (!projectVisible(path))
            continue;
        projectPath(project->root(), path); // rejects links before following them
        const bool isDirectory = file.is_directory();
        if (!isDirectory && !file.is_regular_file())
            continue;
        ProjectUiState::Entry entry;
        entry.path = path;
        entry.name = utf8(file.path().filename().wstring());
        entry.directory = isDirectory;
        const auto key = projectPathKey(path);
        entry.protectedPath =
            key == "ASSETS" || key == "SCENES" || key == "CODE" || key == "CONFIG" || key == "PROJECT.PROTO.JSON";
        entry.kind = isDirectory ? "Папка" : "Файл";
        entry.size = isDirectory ? 0 : file.file_size();
        if (const auto found = byPath.find(key); found != byPath.end()) {
            entry.assetId = found->second->id;
            entry.kind = found->second->kind == "ModelSource" ? "3D-модель"
                         : found->second->kind == "Scene"     ? "Сцена"
                                                              : "Ресурс";
        }
        if (key == "PROJECT.PROTO.JSON")
            entry.kind = "Проєкт";
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.directory != b.directory ? a.directory > b.directory : projectPathKey(a.name) < projectPathKey(b.name);
    });
    ui.directory = directory;
    ui.entries = std::move(entries);
    ui.assets = std::move(assets);
    ++ui.refreshes;
}
void Editor::navigateFiles(const std::string& directory) {
    if (!project_)
        return;
    if (!fs::is_directory(projectPath(project_->root(), directory)))
        throw std::runtime_error("Папка вже не існує");
    projectUi_->directory = directory;
    projectUi_->selection.clear();
    projectUi_->filter.fill(0);
    refreshProjectFiles();
}
void Editor::pollProject() {
    if (!projectUi_)
        return;
    const auto lifetime = projectUi_;
    auto& ui = *lifetime;
    if (ui.pending) {
        auto action = std::move(ui.pending);
        ui.pending = {};
        try {
            action();
        } catch (const std::exception& error) {
            showError(error);
        }
        if (projectUi_ != lifetime)
            return;
    }
    if (!project_)
        return;
    const auto now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    if (project_->consumeChanged()) {
        ui.refreshPending = true;
        ui.changedAt = now;
    }
    if (ui.refreshPending && now - ui.changedAt >= .20 && !importJob_.busy() && !document_.gestureActive()) {
        ui.refreshPending = false;
        try {
            project_->refresh();
            auto code = prepareCodeRefresh();
            document_.refreshProjectAssets();
            refreshProjectFiles();
            codeUi_.swap(code);
        } catch (const std::exception& error) {
            showError(error);
        }
    }
}
void Editor::executeFilePlan(const FilePlan& plan) {
    if (!project_)
        throw std::runtime_error("Спочатку відкрийте проєкт");
    if (codeDirty() && !codeSaving_)
        throw std::runtime_error("Збережіть зміни у панелі C++ перед файловою операцією");
    if (importJob_.busy())
        throw std::runtime_error("Дочекайтеся завершення імпорту");
    document_.endGesture();
    document_.validateSavedFile();
    if (plan.edits.empty())
        return;
    auto action = std::make_shared<ProjectFileAction>();
    action->transaction = FileTransaction::prepare(project_->root(), plan);
    const auto transaction = action->transaction;
    const auto project = project_;
    action->preflight = [this, project] {
        if (project_ != project)
            throw std::runtime_error("Історія належить іншому проєкту");
        if (codeDirty() && !codeSaving_)
            throw std::runtime_error("Збережіть зміни у панелі C++ перед Undo/Redo файлової операції");
        if (importJob_.busy())
            throw std::runtime_error("Дочекайтеся завершення імпорту");
        document_.validateSavedFile();
    };
    action->reconcile = [this, transaction, project](bool forward) {
        if (project_ != project)
            throw std::runtime_error("Історія належить іншому проєкту");
        auto path = document_.path();
        if (!path.empty()) {
            const auto relative = follow(projectRelative(project->root(), path), transaction->moves(), forward);
            path = projectPath(project->root(), relative);
        }
        auto session = project->prepareRefresh();
        auto ui = std::make_shared<ProjectUiState>(*projectUi_);
        ui->directory = follow(ui->directory, transaction->moves(), forward);
        ui->selection.clear();
        ui->pending = {};
        refreshProjectFiles(project, ui.get());
        auto document = document_.prepareProjectRelocation(path, project->assetWorkspace());
        auto code = prepareCodeRefresh(transaction->moves(), forward);
        auto status = (forward ? "Готово: " : "Скасовано: ") + transaction->label();
        // No throwing work follows publication. A failed preparation leaves
        // all live editor state untouched while the transaction compensates.
        codeUi_.swap(code);
        document_.publishProjectView(std::move(document));
        project->publishRefresh(std::move(session));
        projectUi_.swap(ui);
        status_.swap(status);
    };
    document_.executeExternal(action);
}
void Editor::operateFiles(int operation, const std::vector<std::string>& sources, const std::string& destination,
                          const std::optional<std::string>& newName) {
    if (!project_ || sources.empty())
        return;
    const auto op = static_cast<FileOperation>(operation);
    if (op == FileOperation::Move && sources.size() == 1 &&
        sources.front() ==
            join(destination, newName ? *newName : utf8(utf8Path(sources.front()).filename().wstring()))) {
        status_ = "Розташування та назву не змінено";
        return;
    }
    if (op == FileOperation::Remove) {
        const auto startup = projectRelative(project_->root(), project_->startupScenePath());
        const auto active = document_.path().empty() ? "" : projectRelative(project_->root(), document_.path());
        for (const auto& source : sources) {
            if (under(startup, source) || (!active.empty() && under(active, source)))
                throw std::runtime_error("Початкову та відкриту сцени не можна видаляти");
            for (const auto& asset : projectUi_->assets)
                if (asset.kind == "ModelSource" && under(asset.path, source) &&
                    std::any_of(document_.scene.modelSources.begin(), document_.scene.modelSources.end(),
                                [&](AssetId id) { return id.string() == asset.id; }))
                    throw std::runtime_error("Ресурс використовується відкритою сценою");
        }
    }
    executeFilePlan(planProjectFiles(project_->root(), op, sources, destination, newName));
}
void Editor::dropProjectAsset() {
    if (!project_ || importJob_.busy())
        return;
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("PROTO_PROJECT_FILES")) {
            std::string source(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
            if (source.find('\n') == std::string::npos) {
                const auto found =
                    std::find_if(projectUi_->assets.begin(), projectUi_->assets.end(), [&](const auto& a) {
                        return a.kind == "ModelSource" && projectPathKey(a.path) == projectPathKey(source);
                    });
                if (found != projectUi_->assets.end()) {
                    const auto id = AssetId::parse(found->id);
                    projectUi_->pending = [this, id] {
                        document_.endGesture();
                        importScene_ = document_.scene.id;
                        importInstance_ = true;
                        importJob_.reload(project_->assetWorkspace(), id);
                    };
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}
void Editor::projectFiles() {
    if (!projectUi_)
        projectUi_ = std::make_shared<ProjectUiState>();
    if (!project_) {
        ImGui::Begin("Файли проєкту###Files");
        if (ImGui::Button("Новий проєкт…"))
            request(Action::NewProject);
        ImGui::SameLine();
        if (ImGui::Button("Відкрити проєкт…"))
            request(Action::OpenProject);
        ImGui::End();
        sceneFiles();
        return;
    }
    auto& ui = *projectUi_;
    ImGui::Begin("Файли проєкту###Files");
    ui.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ui.rowCenters.clear();
    const auto defer = [&](std::function<void()> work) { ui.pending = std::move(work); };
    const auto paste = [&] {
        const auto paths = ui.clipboard;
        const auto destination = ui.directory;
        const bool cut = ui.cut;
        defer([this, paths, destination, cut] {
            operateFiles(int(cut ? FileOperation::Move : FileOperation::Copy), paths, destination);
            if (cut) {
                projectUi_->clipboard.clear();
                projectUi_->cut = false;
            }
        });
    };
    const auto rename = [&](bool copy) {
        if (ui.selection.size() != 1)
            return;
        ui.renamePath = *ui.selection.begin();
        nameBuffer(ui, utf8(utf8Path(ui.renamePath).filename().wstring()));
        ui.copyAs = copy;
        ui.renamePopup = true;
    };
    const auto dragTarget = [&](const std::string& directory) {
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("PROTO_PROJECT_FILES")) {
                const std::string text(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
                std::vector<std::string> paths;
                size_t at{};
                while (at < text.size()) {
                    const auto end = text.find('\n', at);
                    paths.push_back(text.substr(at, end - at));
                    if (end == std::string::npos)
                        break;
                    at = end + 1;
                }
                const bool copy = ImGui::GetIO().KeyCtrl;
                defer([this, paths, directory, copy] {
                    operateFiles(int(copy ? FileOperation::Copy : FileOperation::Move), paths, directory);
                });
            }
            ImGui::EndDragDropTarget();
        }
    };
    if (ImGui::SmallButton(project_->info().name.c_str()))
        defer([this] { navigateFiles(""); });
    dragTarget("");
    std::string breadcrumb;
    for (const auto& part : utf8Path(ui.directory)) {
        const auto name = utf8(part.wstring());
        breadcrumb = join(breadcrumb, name);
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::PushID(breadcrumb.c_str());
        if (ImGui::SmallButton(name.c_str()))
            defer([this, breadcrumb] { navigateFiles(breadcrumb); });
        dragTarget(breadcrumb);
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  |  %zu елементів  ·  Undo: %zu", ui.entries.size(), document_.undoCount());
    ImGui::BeginDisabled(importJob_.busy());
    ImGui::BeginDisabled(ui.directory.empty());
    if (ImGui::SmallButton("Вгору")) {
        const auto path = parent(ui.directory);
        defer([this, path] { navigateFiles(path); });
    }
    ui.backCenter = itemCenter();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Нова папка")) {
        nameBuffer(ui, "Нова папка");
        ui.folderPopup = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Імпорт glTF / GLB…"))
        importModel();
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.clipboard.empty());
    if (ImGui::SmallButton("Вставити"))
        paste();
    ui.pasteCenter = itemCenter();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Оновити"))
        defer([this] {
            project_->refresh();
            document_.refreshProjectAssets();
            refreshProjectFiles();
        });
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * appliedScale_);
    ImGui::InputTextWithHint("##FileFilter", "Пошук у папці", ui.filter.data(), ui.filter.size());
    auto& io = ImGui::GetIO();
    if (!playBusy() && ui.focused && !io.WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            ui.selection.clear();
            for (const auto& entry : ui.entries)
                if (!entry.protectedPath)
                    ui.selection.insert(entry.path);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            ui.clipboard = selected(ui);
            ui.cut = false;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            ui.clipboard = selected(ui);
            ui.cut = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && !ui.clipboard.empty())
            paste();
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            rename(false);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ui.selection.empty())
            ui.deletePopup = true;
    }
    if (ImGui::BeginTable(
            "ProjectFiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, std::max(65.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupColumn("Назва", ImGuiTableColumnFlags_WidthStretch, .65f);
        ImGui::TableSetupColumn("Тип", ImGuiTableColumnFlags_WidthStretch, .2f);
        ImGui::TableSetupColumn("Розмір", ImGuiTableColumnFlags_WidthStretch, .15f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        std::vector<size_t> filtered;
        const auto filter = projectPathKey(ui.filter.data());
        for (size_t i = 0; i < ui.entries.size(); ++i)
            if (filter.empty() || projectPathKey(ui.entries[i].name).find(filter) != std::string::npos)
                filtered.push_back(i);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filtered.size()));
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& entry = ui.entries[filtered[static_cast<size_t>(row)]];
                ImGui::PushID(entry.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string label = (entry.directory ? "+  " : "   ") + entry.name;
                if (ImGui::Selectable(label.c_str(), ui.selection.contains(entry.path),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (!io.KeyCtrl)
                        ui.selection.clear();
                    if (io.KeyCtrl && ui.selection.contains(entry.path))
                        ui.selection.erase(entry.path);
                    else
                        ui.selection.insert(entry.path);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        const auto path = entry.path;
                        if (entry.directory)
                            defer([this, path] { navigateFiles(path); });
                        else if (entry.kind == "Сцена")
                            defer([this, path] {
                                pendingScenePath_ = projectPath(project_->root(), path);
                                request(Action::OpenFileScene);
                            });
                        else if (under(entry.path, "Code"))
                            defer([this, path] { openCode(projectPath(project_->root(), path)); });
                        else if (entry.kind == "3D-модель") {
                            const auto id = AssetId::parse(entry.assetId);
                            defer([this, id] {
                                importScene_ = document_.scene.id;
                                importInstance_ = true;
                                importJob_.reload(project_->assetWorkspace(), id);
                            });
                        }
                    }
                }
                ui.rowCenters[entry.path] = itemCenter();
                if (ImGui::IsItemHovered() && !entry.assetId.empty())
                    ImGui::SetTooltip("%s", entry.assetId.c_str());
                if (!entry.protectedPath && ImGui::BeginDragDropSource()) {
                    if (!ui.selection.contains(entry.path)) {
                        ui.selection.clear();
                        ui.selection.insert(entry.path);
                    }
                    std::string payload;
                    for (const auto& path : ui.selection) {
                        if (!payload.empty())
                            payload += '\n';
                        payload += path;
                    }
                    ImGui::SetDragDropPayload("PROTO_PROJECT_FILES", payload.data(), payload.size());
                    ImGui::Text("%zu елементів · Ctrl: копія", ui.selection.size());
                    ImGui::EndDragDropSource();
                }
                if (entry.directory)
                    dragTarget(entry.path);
                if (ImGui::BeginPopupContextItem()) {
                    if (!ui.selection.contains(entry.path)) {
                        ui.selection.clear();
                        ui.selection.insert(entry.path);
                    }
                    ImGui::BeginDisabled(entry.protectedPath);
                    if (ImGui::MenuItem("Копіювати", "Ctrl+C")) {
                        ui.clipboard = selected(ui);
                        ui.cut = false;
                    }
                    if (ImGui::MenuItem("Вирізати", "Ctrl+X")) {
                        ui.clipboard = selected(ui);
                        ui.cut = true;
                    }
                    if (ImGui::MenuItem("Перейменувати…", "F2", false, ui.selection.size() == 1))
                        rename(false);
                    if (ImGui::MenuItem("Копіювати як…", nullptr, false, ui.selection.size() == 1))
                        rename(true);
                    if (ImGui::MenuItem("Видалити…", "Delete"))
                        ui.deletePopup = true;
                    ImGui::EndDisabled();
                    if (entry.directory && ImGui::MenuItem("Вставити сюди", nullptr, false, !ui.clipboard.empty())) {
                        const auto paths = ui.clipboard;
                        const auto destination = entry.path;
                        const bool cut = ui.cut;
                        defer([this, paths, destination, cut] {
                            operateFiles(int(cut ? FileOperation::Move : FileOperation::Copy), paths, destination);
                            if (cut)
                                projectUi_->clipboard.clear();
                        });
                    }
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", entry.kind.c_str());
                ImGui::TableNextColumn();
                if (!entry.directory)
                    ImGui::TextDisabled("%.1f KiB", double(entry.size) / 1024);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    const auto& gpu = *renderer_.assetRenderer();
    const auto& lighting = gpu.lightingStats();
    ImGui::TextDisabled("GPU %.2f ms · Ресурси %.1f MiB · Тіні %u · Кеш %u  |", renderer_.gpuMilliseconds(),
                        double(allocationBytes_) / (1024 * 1024), lighting.shadowFaces, lighting.shadowCacheHits);
    ImGui::SameLine();
    if (!gpu.error().empty())
        ImGui::TextColored(ImVec4(1, .5f, .35f, 1), "%s", gpu.error().c_str());
    else if (gpu.pending())
        ImGui::TextDisabled("Завантаження на GPU: %zu ресурсів", gpu.pending());
    else if (importJob_.busy())
        ImGui::TextDisabled("Підготовка моделі…");
    else
        ImGui::TextDisabled("%s",
                            status_.empty() ? "Ctrl+C / X / V · F2 · Перетягніть 3D-модель у сцену" : status_.c_str());
    ImGui::End();
}
void Editor::projectPopups() {
    if (!projectUi_)
        return;
    auto& ui = *projectUi_;
    const auto popup = [](bool& flag, const char* title) {
        if (flag) {
            ImGui::OpenPopup(title);
            flag = false;
        }
    };
    popup(ui.createPopup, "Новий проєкт");
    popup(ui.renamePopup, "Назва файлу");
    popup(ui.folderPopup, "Створити папку");
    popup(ui.deletePopup, "Видалити файли");
    for (int kind = 0; kind < 3; ++kind) {
        const char* title = kind == 0 ? "Новий проєкт" : kind == 1 ? "Назва файлу" : "Створити папку";
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            continue;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(370 * appliedScale_);
        const bool enter =
            ImGui::InputText("##Name", ui.name.data(), ui.name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (kind == 0)
            ImGui::TextWrapped("Папка: %s", utf8(ui.createParent.wstring()).c_str());
        if (ImGui::Button(kind == 1 && ui.copyAs ? "Копіювати" : "Готово") || enter) {
            const std::string name(ui.name.data());
            try {
                if (name.empty() || name.find_first_of("/\\") != std::string::npos)
                    throw std::runtime_error("Вкажіть одну непорожню назву");
                if (kind == 0) {
                    const auto path = projectPath(ui.createParent, name);
                    // Queue publication after EndPopup so replacing UI state
                    // cannot invalidate references in this frame.
                    ui.pending = [this, path, name] { createProject(path, name); };
                } else if (kind == 1) {
                    const auto source = ui.renamePath;
                    const bool copy = ui.copyAs;
                    ui.pending = [this, source, name, copy] {
                        operateFiles(int(copy ? FileOperation::Copy : FileOperation::Move), {source}, parent(source),
                                     name);
                    };
                } else {
                    const auto path = join(ui.directory, name);
                    ui.pending = [this, path] {
                        if (projectStamp(project_->root(), path).kind != FileKind::Missing)
                            throw std::runtime_error("Така назва вже існує");
                        FilePlan plan{"Створення папки", {{path, FileKind::Directory}}};
                        executeFilePlan(plan);
                    };
                }
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                showError(error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Видалити файли", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Видалити %zu елементів?", ui.selection.size());
        ImGui::TextUnformatted("Їх можна відновити через Ctrl+Z у цій сесії.");
        if (ImGui::Button("Видалити")) {
            const auto paths = selected(ui);
            ui.pending = [this, paths] { operateFiles(int(FileOperation::Remove), paths, ""); };
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
} // namespace proto
