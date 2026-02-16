#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace core {

class Window {
public:
    struct Extent {
        uint32_t width;
        uint32_t height;
    };

    Window(const std::string& title, uint32_t width, uint32_t height);
    ~Window();

    bool shouldClose() const { return m_shouldClose; }
    void pollEvents();
    
    HWND getHandle() const { return m_hwnd; }
    HINSTANCE getInstance() const { return m_hinstance; }
    Extent getExtent() const { return {m_width, m_height}; }
    bool isResized() const { return m_resized; }
    void resetResizedFlag() { m_resized = false; }

    // Input handling is now delegated to InputManager
    // Window still captures events but forwards them.
    
private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    HINSTANCE m_hinstance;
    HWND m_hwnd;
    uint32_t m_width;
    uint32_t m_height;
    std::string m_title;
    bool m_shouldClose = false;
    bool m_resized = false;
};

} // namespace core
