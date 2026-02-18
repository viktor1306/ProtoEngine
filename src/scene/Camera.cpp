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

// ---------------------------------------------------------------------------
// getRayFromMouse — screen-to-world ray (unproject)
//
// Steps:
//   1. Convert pixel (mouseX, mouseY) → NDC [-1,+1]
//      NDC.x = (2 * mx / w) - 1
//      NDC.y = 1 - (2 * my / h)   ← Y flipped (screen top = NDC +1)
//   2. Clip-space ray: vec4(ndcX, ndcY, -1, 1)  (pointing into screen)
//   3. Unproject: multiply by inverse(proj * view)
//   4. Perspective divide + normalize → world-space direction
// ---------------------------------------------------------------------------
core::math::Vec3 Camera::getRayFromMouse(int mouseX, int mouseY,
                                          int screenW, int screenH) const
{
    if (screenW <= 0 || screenH <= 0) return m_front; // fallback

    // Step 1: pixel → NDC
    // Vulkan NDC: X in [-1,+1] left-to-right, Y in [-1,+1] top-to-bottom.
    // Screen origin is top-left, so Y maps directly (no flip needed here).
    // The projection matrix already handles the Vulkan Y-flip (data[1][1] = -1/tan).
    // If we flip Y here AND the proj matrix flips Y, we get double-flip → wrong result.
    float ndcX = (2.0f * static_cast<float>(mouseX) / static_cast<float>(screenW)) - 1.0f;
    float ndcY = (2.0f * static_cast<float>(mouseY) / static_cast<float>(screenH)) - 1.0f;

    // Step 2: clip-space ray (w=1 for direction, z=-1 = near plane)
    // We use z=-1 so the ray points forward into the scene
    core::math::Mat4 proj = getProjectionMatrix();
    core::math::Mat4 view = getViewMatrix();
    core::math::Mat4 viewProj = proj * view;
    core::math::Mat4 invVP = core::math::Mat4::inverse(viewProj);

    // Unproject two points: near (z=-1) and far (z=+1) in NDC
    // Then direction = far - near, normalized
    auto unproject = [&](float z) -> core::math::Vec3 {
        // Homogeneous clip coords
        float cx = ndcX;
        float cy = ndcY;
        float cz = z;
        float cw = 1.0f;

        // Multiply by inverse VP (data[col][row], column-major)
        // result[row] = sum over col: invVP.data[col][row] * input[col]
        const auto& d = invVP.data;
        float wx = d[0][0]*cx + d[1][0]*cy + d[2][0]*cz + d[3][0]*cw;
        float wy = d[0][1]*cx + d[1][1]*cy + d[2][1]*cz + d[3][1]*cw;
        float wz = d[0][2]*cx + d[1][2]*cy + d[2][2]*cz + d[3][2]*cw;
        float ww = d[0][3]*cx + d[1][3]*cy + d[2][3]*cz + d[3][3]*cw;

        if (std::abs(ww) < 1e-7f) return m_front;
        return { wx / ww, wy / ww, wz / ww };
    };

    core::math::Vec3 nearPt = unproject(-1.0f);
    core::math::Vec3 farPt  = unproject( 1.0f);

    core::math::Vec3 dir = {
        farPt.x - nearPt.x,
        farPt.y - nearPt.y,
        farPt.z - nearPt.z
    };

    return core::math::Vec3::normalize(dir);
}

} // namespace scene
