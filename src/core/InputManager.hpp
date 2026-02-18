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
    bool isMouseButtonPressed(int button) const;     // 0: Left, 1: Right, 2: Middle
    bool isMouseButtonJustPressed(int button) const; // true only on the frame it was first pressed
    void getMousePosition(int& x, int& y) const;
    void getMouseDelta(int& dx, int& dy) const;
    float getMouseWheelDelta() const { return m_mouseWheelDelta; } // ticks this frame (+up/-down)

    // Platform specific callbacks (called by Window)
    void processKey(int key, bool pressed);
    void processMouseButton(int button, bool pressed);
    void processMouseMove(int x, int y);
    void processMouseRaw(int dx, int dy);
    void processMouseWheel(float delta); // delta in wheel ticks (120 units = 1 tick on Windows)

private:
    InputManager() = default;

    std::array<bool, 256> m_keys{};
    std::array<bool, 3>   m_mouseButtons{};     // current frame state
    std::array<bool, 3>   m_mouseButtonsPrev{}; // previous frame state (for JustPressed)

    int   m_mouseX = 0;
    int   m_mouseY = 0;
    int   m_mouseDeltaX    = 0;
    int   m_mouseDeltaY    = 0;
    float m_mouseWheelDelta = 0.0f; // accumulated wheel ticks this frame
};

} // namespace core
