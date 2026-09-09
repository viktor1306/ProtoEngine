#include "editor/Editor.hpp"
#include "editor/CodeUiState.hpp"
#include "assets/AssetIO.hpp"
#include "project/AssetFilePlanner.hpp"
#include "project/FileTransactions.hpp"
#include "project/ProjectSession.hpp"
#include "scene/SceneIO.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <imgui_internal.h>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

namespace proto {

namespace {
namespace fs = std::filesystem;
constexpr size_t maxCodeBytes = 1024u * 1024u;
uint64_t nextCodeWidgetRevision = 1;

bool allowedCodeExtension(const fs::path& path) {
    auto extension = utf8(path.extension().wstring());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    static constexpr std::array<std::string_view, 10> extensions{".CC",  ".CPP", ".CXX", ".H",   ".HH",
                                                                 ".HPP", ".HXX", ".INL", ".IPP", ".TCC"};
    return std::find(extensions.begin(), extensions.end(), std::string_view(extension)) != extensions.end();
}

size_t boundedLength(const char* text, size_t limit) {
    size_t length{};
    while (length < limit && text[length])
        ++length;
    return length;
}

bool sameCodePath(const CodeUiState& state, const fs::path& path) {
    return fs::absolute(state.path).lexically_normal() == fs::absolute(path).lexically_normal();
}

bool codeUnder(const std::string& path, const std::string& parent) {
    const auto childKey = projectPathKey(path), parentKey = projectPathKey(parent);
    return childKey == parentKey ||
           (childKey.size() > parentKey.size() && childKey.starts_with(parentKey) && childKey[parentKey.size()] == '/');
}

std::string followCodePath(std::string path, const std::vector<PathMove>& moves, bool forward) {
    const PathMove* selected{};
    for (const auto& move : moves) {
        const auto& source = forward ? move.before : move.after;
        if (codeUnder(path, source) &&
            (!selected || source.size() > (forward ? selected->before : selected->after).size()))
            selected = &move;
    }
    if (selected) {
        const auto& source = forward ? selected->before : selected->after;
        const auto& destination = forward ? selected->after : selected->before;
        path = destination + path.substr(source.size());
    }
    return path;
}

void fillCodeBuffer(CodeUiState& state) {
    state.buffer.assign(maxCodeBytes + 1, '\0');
    const auto count = std::min(state.text.size(), maxCodeBytes);
    std::memcpy(state.buffer.data(), state.text.data(), count);
    state.buffer[count] = '\0';
}

std::pair<int, int> lineSelection(const char* text, int length, size_t requested) {
    if (length <= 0)
        return {0, 0};
    const size_t target = std::max<size_t>(requested, 1);
    size_t line = 1;
    int start = 0;
    for (int index = 0; index < length && line < target; ++index) {
        if (text[index] == '\n') {
            ++line;
            start = index + 1;
        }
    }
    int end = start;
    while (end < length && text[end] != '\n')
        ++end;
    if (end > start && text[end - 1] == '\r')
        --end;
    return {start, end};
}

int codeLineCallback(ImGuiInputTextCallbackData* data) {
    auto* state = static_cast<CodeUiState*>(data->UserData);
    if (!state)
        return 0;
    // InputTextMultiline keeps its internal editing buffer until validation,
    // while the editor needs codeDirty() to gate Build/Play immediately.  The
    // callback receives that internal buffer, so mirror it into the retained
    // CodeUiState on every callback before any Save button can be pressed.
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        state->text.assign(data->Buf, static_cast<size_t>(data->BufTextLen));
        state->dirty = state->text != state->savedText;
    }
    if (!state->linePending)
        return 0;
    const auto [start, end] = lineSelection(data->Buf, data->BufTextLen, state->line);
    data->CursorPos = end;
    data->SelectionStart = start;
    data->SelectionEnd = end;
    state->linePending = false;
    return 0;
}

fs::path resolveCodePath(const std::shared_ptr<ProjectSession>& project, const fs::path& input, std::string& relative) {
    if (!project)
        throw std::runtime_error("Спочатку відкрийте проєкт");
    auto absolute = input;
    if (!absolute.is_absolute())
        absolute = project->root() / absolute;
    absolute = fs::absolute(absolute).lexically_normal();
    relative = projectRelative(project->root(), absolute);
    const auto key = projectPathKey(relative);
    if (key == "CODE" || !key.starts_with("CODE/"))
        throw std::runtime_error("Редактор коду відкриває лише файли з папки Code");
    if (!allowedCodeExtension(absolute))
        throw std::runtime_error("Непідтримуване розширення файла C++");
    return projectPath(project->root(), relative);
}

} // namespace

bool Editor::codeDirty() const {
    return codeUi_ && codeUi_->dirty;
}

bool Editor::codeInputFocused() const {
    return codeUi_ && codeUi_->focused;
}

std::shared_ptr<CodeUiState> Editor::prepareCodeRefresh(const std::vector<PathMove>& moves, bool forward) const {
    if (!codeUi_ || !project_)
        return {};
    // An external watcher must never replace an unsaved editor buffer or its
    // original stamp. A save sets codeSaving_ so its transaction can publish a
    // clean candidate after the guarded write succeeds.
    if (codeUi_->dirty && !codeSaving_)
        return codeUi_;
    try {
        const auto relative = followCodePath(codeUi_->relative, moves, forward);
        const auto key = projectPathKey(relative);
        if (key == "CODE" || !key.starts_with("CODE/") || !allowedCodeExtension(utf8Path(relative)))
            return {};
        const auto path = projectPath(project_->root(), relative);
        const auto stamp = projectStamp(project_->root(), relative);
        if (stamp.kind != FileKind::File || stamp.size > maxCodeBytes)
            return {};
        const auto bytes = readDocument(path);
        if (bytes.size() > maxCodeBytes || bytes.find('\0') != std::string::npos || !validUtf8(bytes))
            return {};
        if (relative == codeUi_->relative && stamp == codeUi_->stamp && bytes == codeUi_->text)
            return codeUi_;
        auto candidate = std::make_shared<CodeUiState>();
        candidate->path = path;
        candidate->relative = relative;
        candidate->text = bytes;
        candidate->savedText = bytes;
        candidate->stamp = stamp;
        candidate->line = codeUi_->line;
        candidate->linePending = codeUi_->linePending;
        candidate->dirty = false;
        candidate->focused = codeUi_->focused;
        candidate->focusRequested = codeUi_->focusRequested;
        candidate->widgetRevision = nextCodeWidgetRevision++;
        fillCodeBuffer(*candidate);
        return candidate;
    } catch (...) {
        // Deletion, an external replacement, or a path that is no longer a
        // regular Code source closes a clean panel without mutating the live
        // state until the caller publishes this candidate.
        return {};
    }
}

void Editor::openCode(const std::filesystem::path& input, size_t line) {
    std::string relative;
    const auto path = resolveCodePath(project_, input, relative);
    if (codeUi_ && codeUi_->dirty && !sameCodePath(*codeUi_, path))
        throw std::runtime_error("Спочатку збережіть відкритий файл C++ перед перемиканням");
    if (codeUi_ && codeUi_->dirty && sameCodePath(*codeUi_, path)) {
        codeUi_->line = line;
        codeUi_->linePending = line != 0;
        codeUi_->focusRequested = true;
        return;
    }

    const auto stamp = projectStamp(project_->root(), relative);
    if (stamp.kind != FileKind::File)
        throw std::runtime_error("Файл коду не знайдено");
    if (stamp.size > maxCodeBytes)
        throw std::runtime_error("Файл коду перевищує ліміт 1 MiB");
    const auto bytes = readDocument(path);
    if (bytes.size() > maxCodeBytes || bytes.find('\0') != std::string::npos || !validUtf8(bytes))
        throw std::runtime_error("Файл коду має бути UTF-8 без NUL та не більший за 1 MiB");

    auto state = std::make_shared<CodeUiState>();
    state->path = path;
    state->relative = relative;
    state->text = bytes;
    state->savedText = bytes;
    state->stamp = stamp;
    state->line = line;
    state->linePending = line != 0;
    state->dirty = false;
    state->focusRequested = true;
    state->widgetRevision = nextCodeWidgetRevision++;
    fillCodeBuffer(*state);
    codeUi_ = std::move(state);
}

bool Editor::saveCode() {
    if (!codeUi_ || !codeUi_->dirty)
        return true;
    if (!project_) {
        showError(std::runtime_error("Спочатку відкрийте проєкт"));
        return false;
    }
    try {
        const auto state = codeUi_;
        struct SaveGuard {
            bool& active;
            explicit SaveGuard(bool& value) : active(value) { active = true; }
            ~SaveGuard() { active = false; }
        } saving(codeSaving_);
        if (state->text.size() > maxCodeBytes || state->text.find('\0') != std::string::npos || !validUtf8(state->text))
            throw std::runtime_error("Код має бути UTF-8 без NUL та не більший за 1 MiB");
        FilePlan plan;
        plan.label = "Зберегти код C++";
        plan.edits = {{state->relative, FileKind::File, state->text, {}}};
        plan.guards = {{state->relative, state->stamp}};
        executeFilePlan(plan);
        status_ = "Код збережено: " + state->relative;
        return true;
    } catch (const std::exception& errorValue) {
        showError(errorValue);
        return false;
    }
}

void Editor::codePanel() {
    if (!codeUi_)
        return;
    if (codeUi_->focusRequested || (codePanelProbe_.enabled && stringProbe_.passed))
        ImGui::SetNextWindowFocus();
    ImGui::Begin("Код C++###Code");
    if (!project_) {
        ImGui::TextDisabled("Відкрийте проєкт, щоб редагувати Code.");
        ImGui::End();
        return;
    }
    if (!codeUi_) {
        ImGui::TextDisabled("Оберіть файл .cpp або .hpp у журналі збірки.");
        ImGui::End();
        return;
    }

    auto& state = *codeUi_;
    // Keep the code tab in front only after the preceding String-input probe
    // has completed; stealing focus here would clear its active InputText
    // between the injected mouse-down and mouse-up frames.
    if (codePanelProbe_.enabled && stringProbe_.passed)
        ImGui::SetWindowFocus();
    state.focusRequested = false;
    state.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::TextUnformatted(state.relative.c_str());
    ImGui::SameLine();
    ImGui::TextColored(state.dirty ? ImVec4(1, .70f, .25f, 1) : ImVec4(.33f, .80f, .70f, 1), "%s",
                       state.dirty ? "● змінено" : "● збережено");
    ImGui::BeginDisabled(playBusy() || !state.dirty);
    const bool saveClicked = ImGui::Button("Зберегти");
    if (codePanelProbe_.enabled) {
        codePanelProbe_.saveMin = ImGui::GetItemRectMin();
        codePanelProbe_.saveMax = ImGui::GetItemRectMax();
    }
    if (saveClicked) {
        saveCode();
        // saveCode() may publish a new CodeUiState during reconciliation.
        // Do not touch the old reference after that swap.
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    ImGui::EndDisabled();
    if (state.line)
        ImGui::SameLine(), ImGui::TextDisabled("Рядок %zu", state.line);
    ImGui::Separator();

    ImGui::BeginDisabled(playBusy());
    if (state.buffer.size() != maxCodeBytes + 1)
        fillCodeBuffer(state);
    if (state.linePending)
        ImGui::SetKeyboardFocusHere();
    const auto flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways;
    const std::string inputId = "##CodeText" + std::to_string(state.widgetRevision);
    if (codePanelProbe_.enabled && codePanelProbe_.stage == 1)
        ImGui::ActivateItemByID(ImGui::GetID(inputId.c_str()));
    // Keep the retained source state live while typing so Save becomes
    // available before the next native button frame.  Without this flag
    // ImGui 1.92 defers the backing-buffer write until validation, which
    // leaves the Save button disabled while the editor is visibly edited.
    ImGui::PushItemFlag(ImGuiItemFlags_LiveEditOnInputText, true);
    const bool inputChanged = ImGui::InputTextMultiline(inputId.c_str(), state.buffer.data(), state.buffer.size(),
                                                        ImVec2(-1, -1), flags, codeLineCallback, &state);
    ImGui::PopItemFlag();
    if (inputChanged) {
        const auto length = boundedLength(state.buffer.data(), maxCodeBytes);
        state.text.assign(state.buffer.data(), length);
        state.dirty = state.text != state.savedText;
    }
    if (codePanelProbe_.enabled) {
        codePanelProbe_.inputMin = ImGui::GetItemRectMin();
        codePanelProbe_.inputMax = ImGui::GetItemRectMax();
        codePanelProbe_.inputActive = ImGui::IsItemActive();
        codePanelProbe_.inputFocused = ImGui::IsItemFocused();
        codePanelProbe_.inputEdited = ImGui::IsItemEdited();
        codePanelProbe_.inputId = ImGui::GetItemID();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::codeSmokeContract() {
    if (!project_)
        throw std::runtime_error("Code smoke requires an open project");
    const auto root = project_->root();
    const auto originalPath = root / "Code/Behaviors.cpp";
    const std::string original = readDocument(originalPath);
    const std::string originalRelative = "Code/Behaviors.cpp";
    const auto restoreOriginal = [&] {
        codeUi_.reset();
        openCode(originalPath);
        if (!codeUi_ || codeUi_->text != original)
            throw std::runtime_error("Code smoke could not restore the registration source");
    };
    const auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    const auto setDirty = [&](const std::string& text) {
        if (!codeUi_)
            throw std::runtime_error("Code smoke source panel unexpectedly closed");
        codeUi_->text = text;
        codeUi_->dirty = true;
        fillCodeBuffer(*codeUi_);
    };
    std::string temporaryRelative, renamedRelative;

    try {
        openCode(originalPath);
        require(codeUi_ && codeUi_->text == original, "Code smoke could not open registration source");

        const std::string saved = original + "\n// M5 code save contract\n";
        setDirty(saved);
        require(saveCode(), "Code smoke save failed");
        require(readDocument(originalPath) == saved, "Code smoke save did not publish the edited source");
        document_.undo();
        require(codeUi_ && codeUi_->relative == originalRelative && codeUi_->text == original,
                "Code smoke Undo did not restore visible source text");
        document_.redo();
        require(codeUi_ && codeUi_->text == saved, "Code smoke Redo did not restore visible source text");
        document_.undo();
        require(codeUi_ && codeUi_->text == original, "Code smoke final Undo did not restore source");

        const auto suffix = Uuid::create().string();
        temporaryRelative = "Code/M5CodeSmoke-" + suffix + ".cpp";
        renamedRelative = "Code/M5CodeSmoke-" + suffix + "-renamed.cpp";
        const std::string temporary = "#pragma once\n// M5 file transaction contract\n";
        FilePlan create;
        create.label = "Створення файла code smoke";
        create.edits = {{temporaryRelative, FileKind::File, temporary, {}}};
        executeFilePlan(create);
        openCode(root / utf8Path(temporaryRelative));
        require(codeUi_ && codeUi_->text == temporary, "Code smoke could not open a newly created source");
        operateFiles(int(FileOperation::Move), {temporaryRelative}, "Code",
                     utf8(utf8Path(renamedRelative).filename().wstring()));
        require(codeUi_ && codeUi_->relative == renamedRelative && codeUi_->text == temporary,
                "Code smoke rename did not follow the visible source");
        document_.undo();
        require(codeUi_ && codeUi_->relative == temporaryRelative && codeUi_->text == temporary,
                "Code smoke rename Undo did not restore the source panel");
        document_.redo();
        require(codeUi_ && codeUi_->relative == renamedRelative && codeUi_->text == temporary,
                "Code smoke rename Redo did not follow the source panel");
        document_.undo();
        operateFiles(int(FileOperation::Remove), {temporaryRelative}, "");
        require(!codeUi_, "Code smoke did not close the panel after deleting its clean source");
        restoreOriginal();

        const std::string dirty = original + "\n// M5 dirty delete guard\n";
        setDirty(dirty);
        bool rejectedDelete = false;
        try {
            operateFiles(int(FileOperation::Remove), {originalRelative}, "");
        } catch (const std::exception&) {
            rejectedDelete = true;
        }
        require(rejectedDelete && std::filesystem::exists(originalPath), "Code smoke allowed deleting a dirty source");
        restoreOriginal();

        const std::string local = original + "\n// M5 external conflict guard\n";
        setDirty(local);
        atomicWrite(originalPath, original + "\n// external edit\n");
        FilePlan conflict;
        conflict.label = "Перевірка конфлікту code smoke";
        conflict.edits = {{originalRelative, FileKind::File, local, {}}};
        conflict.guards = {{originalRelative, codeUi_->stamp}};
        bool rejectedConflict = false;
        const bool previousSaving = codeSaving_;
        codeSaving_ = true;
        try {
            executeFilePlan(conflict);
        } catch (const std::exception&) {
            rejectedConflict = true;
        }
        codeSaving_ = previousSaving;
        require(rejectedConflict && codeUi_ && codeUi_->dirty && codeUi_->text == local,
                "Code smoke did not preserve the dirty buffer on external conflict");
        atomicWrite(originalPath, original);
        restoreOriginal();
    } catch (...) {
        // The smoke fixture is disposable. Restore the authored source and
        // remove any temporary source before propagating the diagnostic.
        try {
            atomicWrite(originalPath, original);
        } catch (...) {
        }
        std::error_code ignored;
        if (!temporaryRelative.empty())
            std::filesystem::remove(root / utf8Path(temporaryRelative), ignored);
        if (!renamedRelative.empty())
            std::filesystem::remove(root / utf8Path(renamedRelative), ignored);
        codeUi_.reset();
        try {
            openCode(originalPath);
        } catch (...) {
        }
        throw;
    }
}
} // namespace proto
