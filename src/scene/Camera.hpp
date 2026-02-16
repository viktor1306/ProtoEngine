#pragma once

#include "../core/Math.hpp"
#include "../core/Window.hpp"

namespace scene {

class Camera {
public:
    Camera(core::math::Vec3 position, float fov, float aspect);

    void update(core::Window& window, float dt);
    
    core::math::Mat4 getViewMatrix() const;
    core::math::Mat4 getProjectionMatrix() const;
    
    void setAspectRatio(float aspect) { m_aspect = aspect; }
    core::math::Vec3 getPosition() const { return m_position; }

private:
    void updateVectors();

    core::math::Vec3 m_position;
    core::math::Vec3 m_front;
    core::math::Vec3 m_up;
    core::math::Vec3 m_right;
    core::math::Vec3 m_worldUp;

    float m_yaw;
    float m_pitch;
    
    float m_fov;
    float m_aspect;
    float m_zNear = 0.1f;
    float m_zFar = 1000.0f;

    float m_speed = 5.0f;
    float m_sensitivity = 0.1f;

    bool m_firstMouse = true;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
};

} // namespace scene
