#include "LODController.hpp"
#include "Chunk.hpp"
#include <cmath>
#include <algorithm>

namespace world {

int LODController::calculateLOD(int cx, int cy, int cz, int currentLOD) const {
    const float half = static_cast<float>(CHUNK_SIZE) * 0.5f;
    float centerX = static_cast<float>(cx * CHUNK_SIZE) + half;
    float centerY = static_cast<float>(cy * CHUNK_SIZE) + half;
    float centerZ = static_cast<float>(cz * CHUNK_SIZE) + half;

    float dx = centerX - m_cameraPos.x;
    float dy = centerY - m_cameraPos.y;
    float dz = centerZ - m_cameraPos.z;
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    float d0 = std::max(0.0f, m_lodDist0);
    float d1 = std::max(d0,   m_lodDist1);
    float hy = std::max(0.0f, m_lodHysteresis);

    if (currentLOD < 0) {
        if (dist < d0) return 0;
        if (dist < d1) return 1;
        return 2;
    }

    switch (currentLOD) {
        case 0:
            if (dist > d0 + hy) return (dist > d1 + hy) ? 2 : 1;
            return 0;
        case 1:
            if (dist < d0 - hy) return 0;
            if (dist > d1 + hy) return 2;
            return 1;
        case 2:
            if (dist < d1 - hy) return (dist < d0 - hy) ? 0 : 1;
            return 2;
        default:
            if (dist < d0) return 0;
            if (dist < d1) return 1;
            return 2;
    }
}

} // namespace world
