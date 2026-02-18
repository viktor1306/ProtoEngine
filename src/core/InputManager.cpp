#include "InputManager.hpp"
#include <cstring>

namespace core {

void InputManager::update() {
    // Save current state as previous (for JustPressed detection)
    m_mouseButtonsPrev = m_mouseButtons;
    // Reset per-frame deltas
    m_mouseDeltaX    = 0;
    m_mouseDeltaY    = 0;
    m_mouseWheelDelta = 0.0f;
}

bool InputManager::isKeyPressed(int key) const {
    if (key < 0 || key >= 256) return false;
    return m_keys[key];
}

bool InputManager::isKeyJustPressed(int key) const {
    // Not tracked per-frame for keys yet; placeholder
    if (key < 0 || key >= 256) return false;
    return m_keys[key];
}

bool InputManager::isMouseButtonPressed(int button) const {
    if (button < 0 || button >= 3) return false;
    return m_mouseButtons[button];
}

bool InputManager::isMouseButtonJustPressed(int button) const {
    if (button < 0 || button >= 3) return false;
    // True only on the frame the button transitioned from released → pressed
    return m_mouseButtons[button] && !m_mouseButtonsPrev[button];
}

void InputManager::getMousePosition(int& x, int& y) const {
    x = m_mouseX;
    y = m_mouseY;
}

void InputManager::getMouseDelta(int& dx, int& dy) const {
    dx = m_mouseDeltaX;
    dy = m_mouseDeltaY;
}

void InputManager::processKey(int key, bool pressed) {
    if (key >= 0 && key < 256) {
        m_keys[key] = pressed;
    }
}

void InputManager::processMouseButton(int button, bool pressed) {
    if (button >= 0 && button < 3) {
        m_mouseButtons[button] = pressed;
    }
}

void InputManager::processMouseMove(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
}

void InputManager::processMouseRaw(int dx, int dy) {
    m_mouseDeltaX += dx;
    m_mouseDeltaY += dy;
}

void InputManager::processMouseWheel(float delta) {
    // Windows sends 120 units per notch; normalize to ticks
    m_mouseWheelDelta += delta / 120.0f;
}

} // namespace core
