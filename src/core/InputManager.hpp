#pragma once
#include <array>
#include <cstdint>
#include <windows.h> // For VK_ codes if needed, or just use int

namespace core {

class InputManager {
public:
    static InputManager& get() {
        static InputManager instance;
        return instance;
    }

    // Delete copy constructors
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void update(); // Called every frame to reset per-frame states (deltas)

    // Key State
    bool isKeyPressed(int key) const;
    bool isKeyJustPressed(int key) const; // Optional: true only on the frame it was pressed
    
    // Mouse State
    bool isMouseButtonPressed(int button) const; // 0: Left, 1: Right, 2: Middle
    void getMousePosition(int& x, int& y) const;
    void getMouseDelta(int& dx, int& dy) const;

    // Platform specific callbacks (called by Window)
    void processKey(int key, bool pressed);
    void processMouseButton(int button, bool pressed);
    void processMouseMove(int x, int y);
    void processMouseRaw(int dx, int dy);

private:
    InputManager() = default;

    std::array<bool, 256> m_keys{};
    std::array<bool, 3> m_mouseButtons{}; // 0: Left, 1: Right, 2: Middle
    
    int m_mouseX = 0;
    int m_mouseY = 0;
    int m_mouseDeltaX = 0;
    int m_mouseDeltaY = 0;
};

} // namespace core
