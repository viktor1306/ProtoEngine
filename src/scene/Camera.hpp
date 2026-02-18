#pragma once

#include "core/Math.hpp"
#include <algorithm>

namespace scene {

class Camera {
public:
    Camera(core::math::Vec3 position, float fov, float aspect);

    // Update camera movement/rotation using InputManager (no Window needed).
    void update(float dt);

    core::math::Mat4 getViewMatrix()       const;
    core::math::Mat4 getProjectionMatrix() const;

    void setAspectRatio(float aspect) { m_aspect = aspect; }
    void setYaw(float yaw)     { m_yaw = yaw;     updateVectors(); }
    void setPitch(float pitch) { m_pitch = std::clamp(pitch, -89.0f, 89.0f); updateVectors(); }
    core::math::Vec3 getPosition()    const { return m_position; }

    float getYaw()   const { return m_yaw; }
    float getPitch() const { return m_pitch; }

private:
    void updateVectors();

    core::math::Vec3 m_position;
    core::math::Vec3 m_front;
    core::math::Vec3 m_up;
    core::math::Vec3 m_right;
    core::math::Vec3 m_worldUp;

    float m_yaw   = -90.0f;
    float m_pitch =   0.0f;

    float m_fov    = 60.0f;
    float m_aspect = 1.0f;
    float m_zNear  = 0.1f;
    float m_zFar   = 1000.0f;

    float m_speed       = 5.0f;
    float m_sensitivity = 0.1f;

    bool m_firstMouse = true;
};

} // namespace scene
