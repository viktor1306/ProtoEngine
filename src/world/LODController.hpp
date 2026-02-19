#pragma once

#include "core/Math.hpp"
#include <array>
#include <unordered_map>
#include "world/ChunkManager.hpp" // For IVec3Key and IVec3Hash

namespace world {

class LODController {
public:
    float m_lodDist0      = 64.0f;   // LOD 0 → LOD 1 boundary
    float m_lodDist1      = 128.0f;  // LOD 1 → LOD 2 boundary
    float m_lodHysteresis = 4.0f;    // Hysteresis to prevent flickering

    void setCameraPosition(const core::math::Vec3& pos) { m_cameraPos = pos; }
    const core::math::Vec3& getCameraPosition() const { return m_cameraPos; }

    int calculateLOD(int cx, int cy, int cz, int currentLOD = -1) const;

private:
    core::math::Vec3 m_cameraPos{0.0f, 0.0f, 0.0f};
};

} // namespace world
