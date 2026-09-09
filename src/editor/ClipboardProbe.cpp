#include "editor/ClipboardProbe.hpp"
#include <windows.h>
#include <imgui.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <cstring>
#include <vector>

namespace proto {
namespace {
struct ClipboardBackup {
    struct Item { UINT format; HGLOBAL memory; };
    std::vector<Item> items;
    ~ClipboardBackup() { for (const auto& item : items) if (item.memory) GlobalFree(item.memory); }
};
}
std::string probeClipboard(GLFWwindow* window) {
    ClipboardBackup backup;
    const auto owner = glfwGetWin32Window(window);
    if (!OpenClipboard(owner)) return "skipped_busy";
    bool preserved = true;
    UINT format{};
    while ((format = EnumClipboardFormats(format)) != 0) {
        // These formats use GDI handles or owner callbacks, not plain HGLOBAL.
        if (format == CF_BITMAP || format == CF_PALETTE || format == CF_METAFILEPICT ||
            format == CF_ENHMETAFILE || format == CF_OWNERDISPLAY || (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST)) {
            preserved = false; break;
        }
        const auto original = GetClipboardData(format);
        const auto size = original ? GlobalSize(original) : 0;
        if (!size || size > 64 * 1024 * 1024) { preserved = false; break; }
        const auto copy = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!copy) { preserved = false; break; }
        const auto source = GlobalLock(original); const auto destination = GlobalLock(copy);
        if (!source || !destination) {
            if (source) GlobalUnlock(original);
            if (destination) GlobalUnlock(copy);
            GlobalFree(copy); preserved = false; break;
        }
        std::memcpy(destination, source, size);
        GlobalUnlock(copy); GlobalUnlock(original);
        backup.items.push_back({format, copy});
    }
    const auto originalSequence = GetClipboardSequenceNumber();
    CloseClipboard();
    if (!preserved) return "skipped_noncopyable_format";
    if (GetClipboardSequenceNumber() != originalSequence) return "skipped_changed";
    constexpr auto sample = "Proto Engine: Україна Ґґ Єє Іі Її";
    ImGui::SetClipboardText(sample);
    const auto testSequence = GetClipboardSequenceNumber();
    const auto* received = ImGui::GetClipboardText();
    const bool matches = received && std::strcmp(received, sample) == 0;
    bool opened{};
    for (int i = 0; i < 200 && !opened; ++i) {
        opened = OpenClipboard(owner) != FALSE;
        if (!opened) Sleep(5);
    }
    if (!opened) return "failed_restore_busy";
    // If another application copied something meanwhile, preserve that new data.
    if (GetClipboardSequenceNumber() != testSequence) { CloseClipboard(); return "interrupted_by_new_copy"; }
    bool restored = EmptyClipboard() != FALSE;
    for (auto& item : backup.items) {
        if (SetClipboardData(item.format, item.memory)) item.memory = nullptr; // OS owns it now.
        else restored = false;
    }
    CloseClipboard();
    return matches && restored ? "passed_and_restored" : "failed_roundtrip_or_restore";
}
}
