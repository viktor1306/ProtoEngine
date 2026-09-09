#pragma once

#include "editor/GizmoState.hpp"

#include <cstddef>
#include <string>

namespace proto {

// State for the native ImGui event-queue smoke. The scene is created directly
// in memory and is never saved, loaded, or attached to the user's project.
struct GizmoSmokeState {
    bool enabled{};
    bool passed{};
    int stage{};
    EntityId parentId{};
    EntityId childId{};
    EntityId selectionBefore{};
    EntityRecord parentBaseline{};
    EntityRecord baseline{};
    EntityRecord afterTranslate{};
    EntityRecord afterRotate{};
    EntityRecord afterScale{};
    glm::mat4 baselineWorld{1};
    size_t baselineHistory{};
    bool dirtyBefore{};
    glm::vec3 cameraPivot{};
    float cameraYaw{};
    float cameraPitch{};
    float cameraDistance{};
    std::string error;
};

} // namespace proto
