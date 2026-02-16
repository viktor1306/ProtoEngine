#include "Camera.hpp"
#include "../core/InputManager.hpp"
#include <algorithm>

namespace scene {

Camera::Camera(core::math::Vec3 position, float fov, float aspect)
    : m_position(position), m_fov(fov), m_aspect(aspect) {
    
    m_worldUp = {0.0f, 1.0f, 0.0f};
    m_yaw = -90.0f;
    m_pitch = 0.0f;
    updateVectors();
}

void Camera::update(core::Window& window, float dt) {
    // Keyboard Input
    float velocity = m_speed * dt;
    
    if (core::InputManager::get().isKeyPressed('W')) m_position += m_front * velocity;
    if (core::InputManager::get().isKeyPressed('S')) m_position = m_position - m_front * velocity;
    if (core::InputManager::get().isKeyPressed('A')) m_position = m_position - m_right * velocity;
    if (core::InputManager::get().isKeyPressed('D')) m_position += m_right * velocity;
    if (core::InputManager::get().isKeyPressed('E')) m_position += m_worldUp * velocity;
    if (core::InputManager::get().isKeyPressed('Q')) m_position = m_position - m_worldUp * velocity;

    // Mouse Input (Only if RMB is pressed)
    int mouseX, mouseY;
    core::InputManager::get().getMousePosition(mouseX, mouseY);

    if (core::InputManager::get().isMouseButtonPressed(1)) { // RMB
        if (m_firstMouse) {
            m_lastMouseX = mouseX;
            m_lastMouseY = mouseY;
            m_firstMouse = false;
        }

        // Use raw delta if available from InputManager for better precision
        int dx, dy;
        core::InputManager::get().getMouseDelta(dx, dy);
        
        float xoffset = static_cast<float>(dx);
        float yoffset = static_cast<float>(-dy); // Reversed since Y-coordinates range from bottom to top

        xoffset *= m_sensitivity;
        yoffset *= m_sensitivity;

        m_yaw += xoffset;
        m_pitch += yoffset;

        if (m_pitch > 89.0f) m_pitch = 89.0f;
        if (m_pitch < -89.0f) m_pitch = -89.0f;

        updateVectors();
        
        // Update last mouse position for next frame check (though delta usage makes this redundant for calculation, it keeps state consistent if we switch back)
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
    } else {
        m_firstMouse = true;
    }
}

void Camera::updateVectors() {
    core::math::Vec3 front;
    front.x = cos(core::math::toRadians(m_yaw)) * cos(core::math::toRadians(m_pitch));
    front.y = sin(core::math::toRadians(m_pitch));
    front.z = sin(core::math::toRadians(m_yaw)) * cos(core::math::toRadians(m_pitch));
    m_front = core::math::Vec3::normalize(front);
    m_right = core::math::Vec3::normalize(core::math::Vec3::cross(m_front, m_worldUp));
    m_up = core::math::Vec3::normalize(core::math::Vec3::cross(m_right, m_front));
}

core::math::Mat4 Camera::getViewMatrix() const {
    return core::math::Mat4::lookAt(m_position, m_position + m_front, m_up);
}

core::math::Mat4 Camera::getProjectionMatrix() const {
    return core::math::Mat4::perspective(core::math::toRadians(m_fov), m_aspect, m_zNear, m_zFar);
}

} // namespace scene
