#pragma once

#include "core/Diagnostics.hpp"
#include "runtime/PlayerGraphics.hpp"
#include <windows.h>
#include <filesystem>
#include <memory>
#include <optional>

struct GLFWwindow;

namespace proto {

// Small native modeless settings window. It intentionally has no dependency
// on Editor or the asset importer; values are committed only by Apply.
class PlayerGraphicsUi {
  public:
    PlayerGraphicsUi(GLFWwindow* owner, std::filesystem::path userPath, const PlayerGraphics& current,
                     Diagnostics& log);
    ~PlayerGraphicsUi();
    PlayerGraphicsUi(const PlayerGraphicsUi&) = delete;
    PlayerGraphicsUi& operator=(const PlayerGraphicsUi&) = delete;

    void toggle();
    bool visible() const;
    bool consumeApply(PlayerGraphics& settings);
    bool commitApplied(const PlayerGraphics& settings);
    void setCurrent(const PlayerGraphics& settings);
    bool smokePresetCancel();
    void smokeApply(const PlayerGraphics& settings);

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void create();
    void refreshControls();
    void apply();
    void cancel();
    std::wstring editText(HWND control) const;
    void setEdit(HWND control, std::wstring_view value);
    void error(std::string_view message);

    GLFWwindow* owner_{};
    HWND window_{};
    std::filesystem::path userPath_;
    PlayerGraphics current_;
    PlayerGraphics draft_;
    std::optional<PlayerGraphics> applied_;
    Diagnostics& log_;
    HWND profile_{}, renderScale_{}, vSync_{}, viewDistance_{};
    HWND sunEnabled_{}, sunCascades_{}, sunResolution_{}, sunDistance_{}, sunFilter_{};
    HWND pointEnabled_{}, pointResolution_{}, pointTaps_{}, pointBudget_{}, textureMipDrop_{};
};

} // namespace proto
