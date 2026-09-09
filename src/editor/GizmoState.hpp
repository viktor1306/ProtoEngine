#pragma once

#include "editor/GizmoMath.hpp"

#include <imgui.h>
#include <array>

namespace proto {

// Runtime-only state shared by the viewport manipulator and its deterministic
// native smoke driver. Screen values use the same logical ImGui coordinates as
// ImGuiIO::MousePos and the viewport draw list.
struct GizmoState {
    GizmoOperation operation{GizmoOperation::Translate};
    GizmoAxis axis{GizmoAxis::X};
    bool active{};
    bool hovered{};
    bool consumed{};
    EntityId id{};
    EntityRecord startRecord{};
    glm::mat4 startWorld{1};
    glm::mat4 parentWorld{1};
    GizmoBasis basis{};
    glm::vec3 pivotWorld{};
    glm::vec3 startPivotWorld{};
    glm::vec2 pivotScreen{};
    std::array<glm::vec3, 3> axisWorld{};
    std::array<float, 3> axisLength{};
    std::array<glm::vec2, 3> axisScreen{};
    std::array<glm::vec2, 3> ringStartScreens{};
    std::array<glm::vec2, 3> ringTangentScreens{{{1, 0}, {1, 0}, {1, 0}}};
    glm::vec2 ringStartScreen{};
    glm::vec2 ringTangentScreen{1, 0};
    float ringRadius{1};
    float startParameter{};
    float startAngle{};
    float startHandleLength{1};
    ImVec2 viewportPosition{};
    ImVec2 viewportSize{};

    void clearActive() {
        active = false;
        consumed = false;
        id = {};
        startRecord = {};
        startWorld = glm::mat4(1);
        parentWorld = glm::mat4(1);
        basis = {};
        startPivotWorld = {};
        startParameter = 0;
        startAngle = 0;
        startHandleLength = 1;
    }
};

} // namespace proto
