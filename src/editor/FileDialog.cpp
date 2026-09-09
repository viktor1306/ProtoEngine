#include "editor/FileDialog.hpp"
#include <windows.h>
#include <shobjidl.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <stdexcept>

namespace proto {
namespace {
std::optional<std::filesystem::path> fileDialog(GLFWwindow* window, bool save, bool model,
                                                const std::filesystem::path& initialDirectory = {}) {
    IFileDialog* dialog{};
    const auto result = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                         CLSCTX_INPROC_SERVER, IID_IFileDialog, reinterpret_cast<void**>(&dialog));
    if (FAILED(result))
        throw std::runtime_error("Не вдалося відкрити системний файловий діалог");
    struct Release {
        IFileDialog* p;
        ~Release() { p->Release(); }
    } release{dialog};
    const COMDLG_FILTERSPEC filter[]{{L"Сцена Proto Engine", L"*.scene.json"}, {L"JSON", L"*.json"}};
    const COMDLG_FILTERSPEC modelFilter[]{{L"Модель glTF 2.0", L"*.gltf;*.glb"}};
    dialog->SetFileTypes(model ? 1 : 2, model ? modelFilter : filter);
    if (!model)
        dialog->SetDefaultExtension(L"scene.json");
    dialog->SetTitle(model  ? L"Імпортувати модель"
                     : save ? L"Зберегти сцену Proto Engine"
                            : L"Відкрити сцену Proto Engine");
    if (!initialDirectory.empty()) {
        IShellItem* folder{};
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDirectory.c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void**>(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (save)
        dialog->SetFileName(L"Нова сцена.scene.json");
    const auto shown = dialog->Show(glfwGetWin32Window(window));
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    if (FAILED(shown))
        throw std::runtime_error("Помилка файлового діалогу");
    IShellItem* item{};
    if (FAILED(dialog->GetResult(&item)))
        throw std::runtime_error("Не вдалося отримати шлях сцени");
    PWSTR path{};
    const auto hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(hr))
        throw std::runtime_error("Не вдалося прочитати шлях сцени");
    std::filesystem::path selected(path);
    CoTaskMemFree(path);
    return selected;
}
} // namespace
std::optional<std::filesystem::path> sceneFileDialog(GLFWwindow* window, bool save,
                                                     const std::filesystem::path& initialDirectory) {
    return fileDialog(window, save, false, initialDirectory);
}
std::optional<std::filesystem::path> modelFileDialog(GLFWwindow* window) {
    return fileDialog(window, false, true);
}
std::optional<std::filesystem::path> projectFolderDialog(GLFWwindow* window, bool create) {
    IFileOpenDialog* dialog{};
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dialog))))
        throw std::runtime_error("Не вдалося відкрити вибір папки");
    struct Release {
        IFileOpenDialog* p;
        ~Release() { p->Release(); }
    } release{dialog};
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(create ? L"Оберіть папку, в якій створити новий проєкт" : L"Оберіть папку проєкту Proto Engine");
    const auto shown = dialog->Show(glfwGetWin32Window(window));
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    if (FAILED(shown))
        throw std::runtime_error("Помилка вибору папки");
    IShellItem* item{};
    if (FAILED(dialog->GetResult(&item)))
        throw std::runtime_error("Не вдалося отримати папку");
    PWSTR path{};
    const auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(result))
        throw std::runtime_error("Не вдалося прочитати шлях папки");
    std::filesystem::path selected(path);
    CoTaskMemFree(path);
    return selected;
}
} // namespace proto
