#define GLFW_EXPOSE_NATIVE_WIN32
#include "runtime/PlayerGraphicsUi.hpp"
#include "assets/AssetIO.hpp"
#include "scene/SceneIO.hpp"
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace proto {
namespace {
constexpr wchar_t className[] = L"ProtoPlayerGraphicsSettings";
constexpr int firstControl = 100;
constexpr int applyButton = 900;
constexpr int cancelButton = 901;
int nextControl = firstControl;

HWND label(HWND parent, const wchar_t* text, int x, int y, int width = 170) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, 22, parent, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}
HWND edit(HWND parent, int x, int y, int width = 145) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, width, 24,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextControl++)), GetModuleHandleW(nullptr),
                           nullptr);
}
HWND check(HWND parent, const wchar_t* text, int x, int y) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, 220, 24, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextControl++)), GetModuleHandleW(nullptr), nullptr);
}
HWND combo(HWND parent, int x, int y, int width = 145) {
    return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, x, y, width, 180, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextControl++)), GetModuleHandleW(nullptr), nullptr);
}
std::wstring wide(std::string_view value) { return utf8Path(std::string(value)).wstring(); }
std::string narrow(std::wstring_view value) { return utf8(value); }
void setCheck(HWND control, bool checked) { SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }
bool getCheck(HWND control) { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void addCombo(HWND control, const wchar_t* text) { SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text)); }
void setCombo(HWND control, std::wstring_view text) {
    const auto index = SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.data()));
    SendMessageW(control, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
}
std::wstring comboText(HWND control) {
    const auto index = SendMessageW(control, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR)
        return {};
    wchar_t value[64]{};
    SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value));
    return value;
}
} // namespace

PlayerGraphicsUi::PlayerGraphicsUi(GLFWwindow* owner, std::filesystem::path userPath, const PlayerGraphics& current,
                                   Diagnostics& log)
    : owner_(owner), userPath_(std::move(userPath)), current_(current), log_(log) {
    draft_ = current_;
    create();
}

PlayerGraphicsUi::~PlayerGraphicsUi() {
    if (window_)
        DestroyWindow(window_);
}

bool PlayerGraphicsUi::visible() const { return window_ && IsWindowVisible(window_) != FALSE; }

void PlayerGraphicsUi::toggle() {
    if (!window_)
        return;
    if (visible())
        ShowWindow(window_, SW_HIDE);
    else {
        draft_ = current_;
        refreshControls();
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
    }
}

void PlayerGraphicsUi::setCurrent(const PlayerGraphics& settings) {
    current_ = settings;
    draft_ = settings;
    if (visible())
        refreshControls();
}

bool PlayerGraphicsUi::commitApplied(const PlayerGraphics& settings) {
    current_ = settings;
    draft_ = settings;
    try {
        const auto bytes = encodePlayerGraphics(settings);
        atomicWrite(userPath_, bytes);
        return true;
    } catch (const std::exception& errorValue) {
        error(errorValue.what());
        return false;
    }
}

bool PlayerGraphicsUi::consumeApply(PlayerGraphics& settings) {
    if (!applied_)
        return false;
    settings = *applied_;
    applied_.reset();
    return true;
}

bool PlayerGraphicsUi::smokePresetCancel() {
    const auto before = encodePlayerGraphics(current_);
    ShowWindow(window_, SW_SHOWNORMAL);
    SendMessageW(profile_, CB_SETCURSEL, 0, 0);
    SendMessageW(window_, WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(profile_)), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(profile_));
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(cancelButton, BN_CLICKED), 0);
    return encodePlayerGraphics(current_) == before;
}

void PlayerGraphicsUi::smokeApply(const PlayerGraphics& settings) {
    draft_ = settings;
    refreshControls();
    ShowWindow(window_, SW_SHOWNORMAL);
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(applyButton, BN_CLICKED), 0);
}

std::wstring PlayerGraphicsUi::editText(HWND control) const {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length)
        GetWindowTextW(control, result.data(), length + 1);
    return result;
}

void PlayerGraphicsUi::setEdit(HWND control, std::wstring_view value) {
    SetWindowTextW(control, std::wstring(value).c_str());
}

void PlayerGraphicsUi::error(std::string_view message) {
    log_.write("ERROR", message);
    MessageBoxW(window_, utf8Path(std::string(message)).c_str(), L"Proto Player", MB_OK | MB_ICONERROR);
}

void PlayerGraphicsUi::refreshControls() {
    setCombo(profile_, wide(draft_.profile));
    setEdit(renderScale_, wide(std::to_string(draft_.renderScale)));
    setCheck(vSync_, draft_.vSync);
    setEdit(viewDistance_, wide(std::to_string(draft_.viewDistance)));
    setCheck(sunEnabled_, draft_.sunShadows.enabled);
    setEdit(sunCascades_, wide(std::to_string(draft_.sunShadows.cascades)));
    setEdit(sunResolution_, wide(std::to_string(draft_.sunShadows.resolution)));
    setEdit(sunDistance_, wide(std::to_string(draft_.sunShadows.distance)));
    setCombo(sunFilter_, wide(draft_.sunShadows.filter));
    setCheck(pointEnabled_, draft_.pointShadows.enabled);
    setEdit(pointResolution_, wide(std::to_string(draft_.pointShadows.resolution)));
    setEdit(pointTaps_, wide(std::to_string(draft_.pointShadows.filterTaps)));
    setEdit(pointBudget_, wide(std::to_string(draft_.pointShadows.poolBudgetMiB)));
    setEdit(textureMipDrop_, wide(std::to_string(draft_.textureTopMipDrop)));
}

void PlayerGraphicsUi::apply() {
    try {
        PlayerGraphics next = draft_;
        next.profile = narrow(comboText(profile_));
        next.renderScale = std::stof(narrow(editText(renderScale_)));
        next.vSync = getCheck(vSync_);
        next.viewDistance = std::stof(narrow(editText(viewDistance_)));
        next.sunShadows.enabled = getCheck(sunEnabled_);
        next.sunShadows.cascades = static_cast<uint32_t>(std::stoul(narrow(editText(sunCascades_))));
        next.sunShadows.resolution = static_cast<uint32_t>(std::stoul(narrow(editText(sunResolution_))));
        next.sunShadows.distance = std::stof(narrow(editText(sunDistance_)));
        next.sunShadows.filter = narrow(comboText(sunFilter_));
        next.pointShadows.enabled = getCheck(pointEnabled_);
        next.pointShadows.resolution = static_cast<uint32_t>(std::stoul(narrow(editText(pointResolution_))));
        next.pointShadows.filterTaps = static_cast<uint32_t>(std::stoul(narrow(editText(pointTaps_))));
        next.pointShadows.poolBudgetMiB = static_cast<uint32_t>(std::stoul(narrow(editText(pointBudget_))));
        next.textureTopMipDrop = static_cast<uint32_t>(std::stoul(narrow(editText(textureMipDrop_))));
        next.profile = "Custom";
        validatePlayerGraphics(next);
        draft_ = next;
        applied_ = std::move(next);
        ShowWindow(window_, SW_HIDE);
    } catch (const std::exception& exception) {
        error(exception.what());
    }
}

void PlayerGraphicsUi::cancel() {
    draft_ = current_;
    refreshControls();
    ShowWindow(window_, SW_HIDE);
}

void PlayerGraphicsUi::create() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = &PlayerGraphicsUi::windowProc;
        klass.lpszClassName = className;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&klass);
    });
    const auto owner = owner_ ? glfwGetWin32Window(owner_) : nullptr;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"Proto Player · Graphics", WS_OVERLAPPED | WS_CAPTION |
                                                                          WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 440, 590, owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!window_)
        throw std::runtime_error("Cannot create Player graphics settings window");
    int y = 12;
    label(window_, L"Профіль", 16, y);
    profile_ = combo(window_, 210, y - 2);
    addCombo(profile_, L"Low");
    addCombo(profile_, L"Balanced");
    addCombo(profile_, L"High");
    addCombo(profile_, L"Custom");
    y += 32;
    label(window_, L"Render scale", 16, y);
    renderScale_ = edit(window_, 210, y - 2);
    y += 30;
    vSync_ = check(window_, L"VSync", 16, y);
    y += 34;
    label(window_, L"View distance", 16, y);
    viewDistance_ = edit(window_, 210, y - 2);
    y += 36;
    label(window_, L"Sun shadows", 16, y);
    sunEnabled_ = check(window_, L"Enabled", 210, y);
    y += 28;
    label(window_, L"Sun cascades", 16, y);
    sunCascades_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Sun resolution", 16, y);
    sunResolution_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Sun distance", 16, y);
    sunDistance_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Sun filter", 16, y);
    sunFilter_ = combo(window_, 210, y - 2);
    addCombo(sunFilter_, L"point");
    addCombo(sunFilter_, L"pcf3x3");
    addCombo(sunFilter_, L"pcf5x5");
    y += 36;
    label(window_, L"Point shadows", 16, y);
    pointEnabled_ = check(window_, L"Enabled", 210, y);
    y += 28;
    label(window_, L"Point resolution", 16, y);
    pointResolution_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Point filter taps", 16, y);
    pointTaps_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Point pool MiB", 16, y);
    pointBudget_ = edit(window_, 210, y - 2);
    y += 28;
    label(window_, L"Texture top mip drop", 16, y);
    textureMipDrop_ = edit(window_, 210, y - 2);
    CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 210, 520, 90, 28, window_,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(applyButton)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 310, 520, 90, 28, window_,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(cancelButton)), GetModuleHandleW(nullptr), nullptr);
    refreshControls();
}

LRESULT CALLBACK PlayerGraphicsUi::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PlayerGraphicsUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PlayerGraphicsUi*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    if (!self)
        return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE &&
        LOWORD(wParam) == GetDlgCtrlID(self->profile_)) {
        const auto selected = comboText(self->profile_);
        try {
            if (selected == L"Custom") {
                self->draft_ = self->current_;
                self->draft_.profile = "Custom";
            } else {
                self->draft_ = playerGraphicsProfile(narrow(selected));
            }
            self->refreshControls();
        } catch (const std::exception& errorValue) {
            self->error(errorValue.what());
        }
        return 0;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        if (LOWORD(wParam) == applyButton)
            self->apply();
        else if (LOWORD(wParam) == cancelButton)
            self->cancel();
        return 0;
    }
    if (message == WM_CLOSE) {
        self->cancel();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace proto
