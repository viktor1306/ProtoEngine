#include "InputManager.hpp"
#include <cstring>

namespace core {

void InputManager::update() {
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
}

bool InputManager::isKeyPressed(int key) const {
    if (key < 0 || key >= 256) return false;
    return m_keys[key];
}

bool InputManager::isMouseButtonPressed(int button) const {
    if (button < 0 || button >= 3) return false;
    return m_mouseButtons[button];
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

} // namespace core
