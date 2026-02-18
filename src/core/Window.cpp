#include "Window.hpp"
#include "InputManager.hpp"
#include <iostream>

// Forward declaration of ImGui Win32 message handler.
// Defined in imgui_impl_win32.cpp — linked when ImGui is included in the build.
// We avoid including imgui headers here to keep Window.cpp independent.
extern "C++" LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace core {

Window::Window(const std::string& title, uint32_t width, uint32_t height)
    : m_width(width), m_height(height), m_title(title) {
    
    m_hinstance = GetModuleHandle(nullptr);

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hinstance;
    wc.lpszClassName = "VulkanEngineClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClass(&wc);

    // Adjust window size to include borders/title bar
    RECT rect = { 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowEx(
        0,
        "VulkanEngineClass",
        m_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        m_hinstance,
        this
    );

    if (m_hwnd == nullptr) {
        throw std::runtime_error("Failed to create window");
    }

    ShowWindow(m_hwnd, SW_SHOW);
    
    // Register for Raw Input (Mouse)
    RAWINPUTDEVICE rid[1];
    rid[0].usUsagePage = 0x01; // Generic Desktop Controls
    rid[0].usUsage = 0x02;     // Mouse
    rid[0].dwFlags = 0;
    rid[0].hwndTarget = m_hwnd;
    RegisterRawInputDevices(rid, 1, sizeof(rid[0]));
}

Window::~Window() {
    DestroyWindow(m_hwnd);
    UnregisterClass("VulkanEngineClass", m_hinstance);
}

void Window::pollEvents() {
    MSG msg = {};
    // In main loop, call InputManager::get().update() at start of frame
    
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_shouldClose = true;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Let ImGui process input first (when ImGui is active)
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
        return true;

    Window* window = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* createStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
        window = reinterpret_cast<Window*>(createStruct->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }

    if (window) {
        window->handleMessage(uMsg, wParam, lParam);
    }

    if (uMsg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void Window::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto& input = InputManager::get();

    switch (uMsg) {
        case WM_KEYDOWN:
            if (wParam < 256) input.processKey((int)wParam, true);
            break;
        case WM_KEYUP:
            if (wParam < 256) input.processKey((int)wParam, false);
            break;
        case WM_LBUTTONDOWN: input.processMouseButton(0, true); break;
        case WM_LBUTTONUP:   input.processMouseButton(0, false); break;
        case WM_RBUTTONDOWN: input.processMouseButton(1, true); break;
        case WM_RBUTTONUP:   input.processMouseButton(1, false); break;
        case WM_MBUTTONDOWN: input.processMouseButton(2, true); break;
        case WM_MBUTTONUP:   input.processMouseButton(2, false); break;
        case WM_MOUSEWHEEL: {
            // GET_WHEEL_DELTA_WPARAM returns signed short (positive = scroll up)
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
            input.processMouseWheel(delta);
            break;
        }
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            input.processMouseMove(x, y);
            break;
        }
        case WM_SIZE:
            m_width = LOWORD(lParam);
            m_height = HIWORD(lParam);
            m_resized = true;
            break;
        case WM_INPUT: {
            UINT dwSize = 0;
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
            if (dwSize == 0) return;

            LPBYTE lpb = new BYTE[dwSize];
            if (lpb == NULL) return;
            
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
                 std::cout << "GetRawInputData does not return correct size !" << std::endl;
            
            RAWINPUT* raw = (RAWINPUT*)lpb;
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                input.processMouseRaw(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
            }
            delete[] lpb;
            break;
        }
    }
}

} // namespace core
