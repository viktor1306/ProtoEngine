#include "Camera.hpp"
#include "core/InputManager.hpp"
#include <algorithm>
#include <cmath>

namespace scene {

Camera::Camera(core::math::Vec3 position, float fov, float aspect)
    : m_position(position), m_fov(fov), m_aspect(aspect)
{
    m_worldUp = {0.0f, 1.0f, 0.0f};
    m_yaw     = -90.0f;
    m_pitch   =   0.0f;
    updateVectors();
}

void Camera::update(float dt) {
    auto& input = core::InputManager::get();

    // --- Keyboard movement ---
    float velocity = m_speed * dt;

    if (input.isKeyPressed('W')) m_position += m_front    * velocity;
    if (input.isKeyPressed('S')) m_position  = m_position - m_front    * velocity;
    if (input.isKeyPressed('A')) m_position  = m_position - m_right    * velocity;
    if (input.isKeyPressed('D')) m_position += m_right    * velocity;
    if (input.isKeyPressed('E')) m_position += m_worldUp  * velocity;
    if (input.isKeyPressed('Q')) m_position  = m_position - m_worldUp  * velocity;

    // --- Mouse look (only while RMB is held) ---
    if (input.isMouseButtonPressed(1)) {
        if (m_firstMouse) {
            // Discard the first delta after RMB press to avoid a jump
            int dx, dy;
            input.getMouseDelta(dx, dy); // consume & discard
            m_firstMouse = false;
            return;
        }

        int dx, dy;
        input.getMouseDelta(dx, dy);

        m_yaw   += static_cast<float>( dx) * m_sensitivity;
        m_pitch += static_cast<float>(-dy) * m_sensitivity; // Y inverted

        m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);

        updateVectors();
    } else {
        m_firstMouse = true;
    }
}

void Camera::updateVectors() {
    core::math::Vec3 front;
    front.x = std::cos(core::math::toRadians(m_yaw))   * std::cos(core::math::toRadians(m_pitch));
    front.y = std::sin(core::math::toRadians(m_pitch));
    front.z = std::sin(core::math::toRadians(m_yaw))   * std::cos(core::math::toRadians(m_pitch));

    m_front = core::math::Vec3::normalize(front);
    m_right = core::math::Vec3::normalize(core::math::Vec3::cross(m_front, m_worldUp));
    m_up    = core::math::Vec3::normalize(core::math::Vec3::cross(m_right, m_front));
}

core::math::Mat4 Camera::getViewMatrix() const {
    return core::math::Mat4::lookAt(m_position, m_position + m_front, m_up);
}

core::math::Mat4 Camera::getProjectionMatrix() const {
    return core::math::Mat4::perspective(core::math::toRadians(m_fov), m_aspect, m_zNear, m_zFar);
}

} // namespace scene
