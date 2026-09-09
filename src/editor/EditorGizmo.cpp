#include "editor/Editor.hpp"
#include "editor/GizmoState.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>

namespace proto {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr float hitRadius = 9.0f;
constexpr int ringSegments = 64;

struct Visual {
    GizmoBasis basis{};
    glm::vec3 pivot{};
    glm::vec2 pivotScreen{};
    std::array<glm::vec3, 3> axis{};
    std::array<float, 3> length{};
    std::array<glm::vec2, 3> screen{};
    std::array<glm::vec3, 3> ringU{};
    std::array<glm::vec3, 3> ringV{};
    std::array<glm::vec2, 3> ringStart{};
    std::array<glm::vec2, 3> ringTangent{{{1, 0}, {1, 0}, {1, 0}}};
    std::array<float, 3> ringRadius{{1, 1, 1}};
    bool valid{};
};

struct GizmoHit {
    GizmoAxis axis{GizmoAxis::X};
    float distance{std::numeric_limits<float>::max()};
    bool ring{};
    bool valid{};
};

bool finite2(glm::vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
glm::vec2 toGlm(ImVec2 value) {
    return {value.x, value.y};
}
ImVec2 toIm(glm::vec2 value) {
    return {value.x, value.y};
}

bool closeMatrix(const glm::mat4& actual, const glm::mat4& expected, float tolerance = 1.0e-4f) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(actual[column][row]) ||
                std::abs(actual[column][row] - expected[column][row]) > tolerance)
                return false;
    return true;
}

bool project(const glm::mat4& viewProjection, glm::vec3 world, ImVec2 position, ImVec2 size, glm::vec2& screen) {
    const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
        clip.w <= 1.0e-6f)
        return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z) || ndc.z < 0.0f || ndc.z > 1.0f)
        return false;
    screen = {position.x + (ndc.x * 0.5f + 0.5f) * size.x, position.y + (0.5f - ndc.y * 0.5f) * size.y};
    return finite2(screen);
}

float distanceSquaredPointSegment(glm::vec2 point, glm::vec2 start, glm::vec2 end) {
    const glm::vec2 delta = end - start;
    const float lengthSquared = glm::dot(delta, delta);
    const float parameter =
        lengthSquared > 1.0e-8f ? std::clamp(glm::dot(point - start, delta) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 closest = start + delta * parameter;
    return glm::dot(point - closest, point - closest);
}

GizmoAxis axisFrom(int index) {
    return index == 0 ? GizmoAxis::X : index == 1 ? GizmoAxis::Y : GizmoAxis::Z;
}

const char* operationLabel(GizmoOperation operation) {
    switch (operation) {
    case GizmoOperation::Translate:
        return "W  Рух";
    case GizmoOperation::Rotate:
        return "E  Обертання";
    case GizmoOperation::Scale:
        return "R  Масштаб";
    }
    return "Рух";
}

glm::mat4 parentWorld(const Scene& scene, EntityHandle handle) {
    const auto parent = scene.entity(handle).parent;
    return parent ? scene.transform(parent).world : glm::mat4(1);
}

bool inside(ImVec2 mouse, ImVec2 position, ImVec2 size) {
    return mouse.x >= position.x && mouse.x <= position.x + size.x && mouse.y >= position.y &&
           mouse.y <= position.y + size.y;
}

Visual visualFor(const RenderView& view, const Scene& scene, EntityHandle handle, ImVec2 position, ImVec2 size,
                 GizmoOperation operation, const GizmoBasis* fixedBasis = nullptr,
                 const glm::vec3* fixedPivot = nullptr) {
    Visual result;
    const auto& transform = scene.transform(handle);
    const auto local = scene.record(handle).transform;
    result.pivot = fixedPivot ? *fixedPivot : glm::vec3(transform.world[3]);
    const auto pWorld = parentWorld(scene, handle);
    result.basis = fixedBasis ? *fixedBasis : makeGizmoBasis(local, pWorld, result.pivot);
    if (!finiteMatrix(result.basis.parentWorld) || !invertibleMatrix(result.basis.parentWorld) ||
        !finite2({position.x, position.y}) || size.x <= 1.0f || size.y <= 1.0f)
        return result;
    if (!project(view.viewProjection, result.pivot, position, size, result.pivotScreen))
        return result;
    for (int index = 0; index < 3; ++index) {
        result.axis[index] = operation == GizmoOperation::Translate ? unitAxis(axisFrom(index))
                                                                    : basisAxis(result.basis, axisFrom(index));
        if (glm::length(result.axis[index]) <= 1.0e-6f)
            return result;
        glm::vec2 one;
        if (!project(view.viewProjection, result.pivot + result.axis[index], position, size, one))
            return result;
        const float pixelsPerWorld = glm::length(one - result.pivotScreen);
        if (!std::isfinite(pixelsPerWorld) || pixelsPerWorld <= 1.0e-5f)
            return result;
        result.length[index] = std::clamp(84.0f / pixelsPerWorld, 0.05f, 10000.0f);
        if (!project(view.viewProjection, result.pivot + result.axis[index] * result.length[index], position, size,
                     result.screen[index]))
            return result;
    }
    const float smallAngle = 2.0f * pi / static_cast<float>(ringSegments);
    for (int ringIndex = 0; ringIndex < 3; ++ringIndex) {
        const int uIndex = (ringIndex + 1) % 3;
        const int vIndex = (ringIndex + 2) % 3;
        result.ringU[ringIndex] = result.basis.worldBasis[uIndex];
        result.ringV[ringIndex] = result.basis.worldBasis[vIndex];
        const float uLength = glm::length(result.ringU[ringIndex]);
        if (!std::isfinite(uLength) || uLength <= 1.0e-6f)
            return result;
        glm::vec2 ringOne;
        if (!project(view.viewProjection, result.pivot + result.ringU[ringIndex], position, size, ringOne))
            return result;
        const float ringPixelsPerUnit = glm::length(ringOne - result.pivotScreen);
        if (!std::isfinite(ringPixelsPerUnit) || ringPixelsPerUnit <= 1.0e-5f)
            return result;
        result.ringRadius[ringIndex] = std::clamp(65.0f / ringPixelsPerUnit, 0.05f, 10000.0f);
        if (!project(view.viewProjection, result.pivot + result.ringU[ringIndex] * result.ringRadius[ringIndex],
                     position, size, result.ringStart[ringIndex]))
            return result;
        glm::vec2 ringNext;
        if (project(view.viewProjection,
                    result.pivot + (result.ringU[ringIndex] * std::cos(smallAngle) +
                                    result.ringV[ringIndex] * std::sin(smallAngle)) *
                                       result.ringRadius[ringIndex],
                    position, size, ringNext)) {
            result.ringTangent[ringIndex] = ringNext - result.ringStart[ringIndex];
            const float tangentLength = glm::length(result.ringTangent[ringIndex]);
            if (tangentLength > 1.0e-5f)
                result.ringTangent[ringIndex] /= tangentLength;
            else
                result.ringTangent[ringIndex] = {0, 1};
        }
    }
    result.valid = true;
    return result;
}

void syncStateVisual(GizmoState& state, const Visual& visual, ImVec2 position, ImVec2 size) {
    state.pivotWorld = visual.pivot;
    state.basis = visual.basis;
    state.pivotScreen = visual.pivotScreen;
    state.axisWorld = visual.axis;
    state.axisLength = visual.length;
    state.axisScreen = visual.screen;
    state.ringStartScreens = visual.ringStart;
    state.ringTangentScreens = visual.ringTangent;
    state.ringStartScreen = visual.ringStart[1];
    state.ringTangentScreen = visual.ringTangent[1];
    state.ringRadius = visual.ringRadius[1];
    state.viewportPosition = position;
    state.viewportSize = size;
}

glm::vec3 interactionAxis(const GizmoState& state) {
    return state.operation == GizmoOperation::Translate ? unitAxis(state.axis) : basisAxis(state.basis, state.axis);
}

GizmoHit hitAxis(const Visual& visual, glm::vec2 mouse) {
    GizmoHit hit;
    const float threshold = hitRadius * hitRadius;
    for (int index = 0; index < 3; ++index) {
        const float distance = distanceSquaredPointSegment(mouse, visual.pivotScreen, visual.screen[index]);
        if (distance <= threshold && distance < hit.distance) {
            hit.axis = axisFrom(index);
            hit.distance = distance;
            hit.valid = true;
        }
    }
    return hit;
}

GizmoHit hitRingProjected(const RenderView& view, const Visual& visual, ImVec2 position, ImVec2 size, glm::vec2 mouse) {
    GizmoHit hit;
    hit.ring = true;
    const float threshold = hitRadius * hitRadius;
    for (int ringIndex = 0; ringIndex < 3; ++ringIndex) {
        glm::vec2 previous{};
        bool havePrevious{};
        for (int i = 0; i <= ringSegments; ++i) {
            const float angle = 2.0f * pi * static_cast<float>(i) / static_cast<float>(ringSegments);
            glm::vec2 current;
            if (!project(view.viewProjection,
                         visual.pivot +
                             (visual.ringU[ringIndex] * std::cos(angle) + visual.ringV[ringIndex] * std::sin(angle)) *
                                 visual.ringRadius[ringIndex],
                         position, size, current))
                continue;
            if (havePrevious) {
                const float distance = distanceSquaredPointSegment(mouse, previous, current);
                if (distance <= threshold && distance < hit.distance) {
                    hit.distance = distance;
                    hit.axis = axisFrom(ringIndex);
                    hit.valid = true;
                }
            }
            previous = current;
            havePrevious = true;
        }
    }
    return hit;
}

void drawToolbar(ImDrawList* draw, ImVec2 position, ImVec2 size, GizmoOperation& operation, bool active,
                 bool& consumed) {
    const float font = ImGui::GetFontSize();
    const float padding = std::max(4.0f, font * .25f);
    const float height = font + padding * 2.0f;
    ImVec2 cursor{position.x + 8.0f, position.y + 8.0f};
    const auto mouse = ImGui::GetIO().MousePos;
    for (const auto candidate : {GizmoOperation::Translate, GizmoOperation::Rotate, GizmoOperation::Scale}) {
        const char* label = operationLabel(candidate);
        const ImVec2 text = ImGui::CalcTextSize(label);
        const float width = text.x + padding * 2.0f;
        const ImVec2 minimum = cursor;
        const ImVec2 maximum{cursor.x + width, cursor.y + height};
        const bool hovered = !active && mouse.x >= minimum.x && mouse.x <= maximum.x && mouse.y >= minimum.y &&
                             mouse.y <= maximum.y && inside(mouse, position, size);
        const bool selected = operation == candidate;
        const ImU32 background = ImGui::GetColorU32(selected  ? ImGuiCol_ButtonActive
                                                    : hovered ? ImGuiCol_ButtonHovered
                                                              : ImGuiCol_Button);
        draw->AddRectFilled(minimum, maximum, background, 3.0f);
        draw->AddText({cursor.x + padding, cursor.y + padding}, ImGui::GetColorU32(ImGuiCol_Text), label);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            operation = candidate;
            consumed = true;
        }
        cursor.x += width + 4.0f;
    }
}

void drawAxis(ImDrawList* draw, const Visual& visual, GizmoAxis axis, GizmoAxis active, GizmoAxis hovered) {
    const int index = axisIndex(axis);
    const ImU32 colors[] = {IM_COL32(235, 84, 78, 255), IM_COL32(87, 220, 126, 255), IM_COL32(91, 147, 244, 255)};
    const bool emphasized = axis == active || axis == hovered;
    const float thickness = emphasized ? 4.0f : 2.7f;
    const float radius = emphasized ? 6.0f : 5.0f;
    draw->AddLine(toIm(visual.pivotScreen), toIm(visual.screen[index]), colors[index], thickness);
    draw->AddCircleFilled(toIm(visual.screen[index]), radius, colors[index], 12);
    draw->AddCircle(toIm(visual.screen[index]), radius + 1.0f, ImGui::GetColorU32(ImGuiCol_WindowBg), 12, 1.0f);
}

void drawRing(ImDrawList* draw, const RenderView& view, const Visual& visual, ImVec2 position, ImVec2 size,
              GizmoAxis active, GizmoAxis hovered) {
    for (int ringIndex = 0; ringIndex < 3; ++ringIndex) {
        const GizmoAxis axis = axisFrom(ringIndex);
        const bool emphasized = axis == active || axis == hovered;
        const ImU32 color = emphasized ? IM_COL32(238, 205, 86, 255) : IM_COL32(220, 183, 70, 190);
        glm::vec2 previous{};
        bool havePrevious{};
        for (int i = 0; i <= ringSegments; ++i) {
            const float angle = 2.0f * pi * static_cast<float>(i) / static_cast<float>(ringSegments);
            glm::vec2 current;
            if (!project(view.viewProjection,
                         visual.pivot +
                             (visual.ringU[ringIndex] * std::cos(angle) + visual.ringV[ringIndex] * std::sin(angle)) *
                                 visual.ringRadius[ringIndex],
                         position, size, current))
                continue;
            if (havePrevious)
                draw->AddLine(toIm(previous), toIm(current), color, emphasized ? 3.0f : 1.7f);
            previous = current;
            havePrevious = true;
        }
    }
}

void drawVisual(ImDrawList* draw, const RenderView& view, const Visual& visual, GizmoOperation operation,
                GizmoAxis active, GizmoAxis hovered, ImVec2 position, ImVec2 size) {
    draw->PushClipRect(position, {position.x + size.x, position.y + size.y}, true);
    if (operation == GizmoOperation::Rotate)
        drawRing(draw, view, visual, position, size, active, hovered);
    else {
        for (const auto axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
            drawAxis(draw, visual, axis, active, hovered);
        if (operation == GizmoOperation::Translate)
            draw->AddCircleFilled(toIm(visual.pivotScreen), 4.5f, ImGui::GetColorU32(ImGuiCol_Text), 12);
    }
    draw->PopClipRect();
}

void setOperationFromKeys(GizmoState& state, bool insideViewport, bool active) {
    auto& io = ImGui::GetIO();
    if (!insideViewport || active || io.WantTextInput || io.KeyCtrl || io.KeyAlt ||
        io.MouseDown[ImGuiMouseButton_Right])
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false))
        state.operation = GizmoOperation::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        state.operation = GizmoOperation::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
        state.operation = GizmoOperation::Scale;
}

void cancelGesture(Editor& editor, GizmoState& state) {
    try {
        editor.document().cancelGesture();
    } catch (...) {
        // A captured record is already validated. Keep a lost focus/input
        // cancellation from escaping the editor frame loop.
    }
    state.clearActive();
}

} // namespace

bool Editor::gizmoActive() const {
    return gizmo_ && gizmo_->active;
}

void Editor::transformGizmo(ImVec2 position, ImVec2 size) {
    if (m0_)
        return;
    if (!gizmo_)
        gizmo_ = std::make_shared<GizmoState>();
    gizmoInputConsumed_ = false;
    gizmo_->hovered = false;
    gizmo_->consumed = false;

    auto& scene = document_.scene;
    scene.update();
    auto* draw = ImGui::GetWindowDrawList();
    drawToolbar(draw, position, size, gizmo_->operation, gizmo_->active, gizmoInputConsumed_);
    const auto& io = ImGui::GetIO();
    const bool viewportInside = inside(io.MousePos, position, size);
    setOperationFromKeys(*gizmo_, viewportInside, gizmo_->active);

    const auto selected = scene.find(document_.selection);
    if (!selected || scene.transform(selected).degenerate || !scene.transform(selected).visible) {
        if (gizmo_->active) {
            gizmoInputConsumed_ = true;
            cancelGesture(*this, *gizmo_);
        }
        return;
    }

    const auto selectedRecord = scene.record(selected);
    const auto pWorld = parentWorld(scene, selected);
    const glm::vec3 selectedPivot(scene.transform(selected).world[3]);
    GizmoBasis liveBasis = makeGizmoBasis(selectedRecord.transform, pWorld, selectedPivot);
    if (!invertibleMatrix(pWorld) || !finiteMatrix(scene.transform(selected).world)) {
        if (gizmo_->active) {
            gizmoInputConsumed_ = true;
            cancelGesture(*this, *gizmo_);
        }
        return;
    }

    // While active, preserve the frame captured at mouse down. Parent changes
    // and entity replacement are invalid contexts and are cancelled below.
    const GizmoBasis* fixedBasis = gizmo_->active ? &gizmo_->basis : nullptr;
    const glm::vec3* fixedPivot = gizmo_->active ? &selectedPivot : nullptr;
    const Visual visual =
        visualFor(sceneView_, scene, selected, position, size, gizmo_->operation, fixedBasis, fixedPivot);
    if (!visual.valid) {
        if (gizmo_->active) {
            gizmoInputConsumed_ = true;
            cancelGesture(*this, *gizmo_);
        }
        return;
    }
    syncStateVisual(*gizmo_, visual, position, size);

    GizmoHit hit;
    if (gizmo_->operation == GizmoOperation::Rotate)
        hit = hitRingProjected(sceneView_, visual, position, size, toGlm(io.MousePos));
    else
        hit = hitAxis(visual, toGlm(io.MousePos));
    gizmo_->hovered = !gizmo_->active && viewportInside && hit.valid &&
                      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const GizmoAxis hoverAxis = hit.valid ? hit.axis : GizmoAxis::All;

    if (gizmo_->active) {
        gizmoInputConsumed_ = true;
        gizmo_->consumed = true;
        if (scene.find(gizmo_->id) != selected || selectedRecord.parent != gizmo_->startRecord.parent ||
            !closeMatrix(parentWorld(scene, selected), gizmo_->parentWorld) || io.AppFocusLost) {
            cancelGesture(*this, *gizmo_);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cancelGesture(*this, *gizmo_);
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !io.MouseDown[ImGuiMouseButton_Left]) {
            try {
                document_.endGesture();
            } catch (const std::exception& error) {
                showError(error);
            }
            gizmo_->clearActive();
        } else {
            const float u = (io.MousePos.x - position.x) / size.x;
            const float v = (io.MousePos.y - position.y) / size.y;
            const Ray ray = viewportRay(sceneView_.viewProjection, u, v);
            try {
                auto next = gizmo_->startRecord;
                if (gizmo_->operation == GizmoOperation::Rotate) {
                    const auto currentAngle = ringAngle(ray, gizmo_->basis, gizmo_->axis);
                    if (currentAngle) {
                        const float delta = wrapAngle(*currentAngle - gizmo_->startAngle);
                        if (const auto transformed =
                                applyLocalRotation(gizmo_->startRecord.transform, gizmo_->axis, delta)) {
                            next.transform = *transformed;
                            document_.preview(next);
                        }
                    }
                } else if (const auto parameter =
                               closestAxisParameter(ray, gizmo_->startPivotWorld, interactionAxis(*gizmo_))) {
                    if (gizmo_->operation == GizmoOperation::Translate) {
                        const glm::vec3 desired =
                            gizmo_->startPivotWorld + interactionAxis(*gizmo_) * (*parameter - gizmo_->startParameter);
                        if (const auto transformed =
                                applyWorldTranslation(gizmo_->startRecord.transform, gizmo_->parentWorld, desired)) {
                            next.transform = *transformed;
                            document_.preview(next);
                        }
                    } else {
                        const float length = std::max(gizmo_->startHandleLength, 1.0e-4f);
                        const float factor = 1.0f + (*parameter - gizmo_->startParameter) / length;
                        if (const auto transformed =
                                applyLocalScale(gizmo_->startRecord.transform, gizmo_->axis, factor)) {
                            next.transform = *transformed;
                            document_.preview(next);
                        }
                    }
                }
            } catch (const std::exception& error) {
                cancelGesture(*this, *gizmo_);
                showError(error);
            }
        }
    } else if (!gizmoInputConsumed_ && viewportInside && !io.WantTextInput &&
               !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit.valid) {
        try {
            const float u = (io.MousePos.x - position.x) / size.x;
            const float v = (io.MousePos.y - position.y) / size.y;
            const Ray ray = viewportRay(sceneView_.viewProjection, u, v);
            gizmo_->active = true;
            gizmo_->consumed = true;
            gizmo_->id = selectedRecord.id;
            gizmo_->startRecord = selectedRecord;
            gizmo_->startWorld = scene.transform(selected).world;
            gizmo_->parentWorld = pWorld;
            gizmo_->basis = liveBasis;
            gizmo_->pivotWorld = selectedPivot;
            gizmo_->startPivotWorld = selectedPivot;
            gizmo_->axis = hit.axis;
            gizmo_->startHandleLength = visual.length[std::max(0, axisIndex(hit.axis))];
            if (gizmo_->operation == GizmoOperation::Rotate) {
                const auto angle = ringAngle(ray, gizmo_->basis, gizmo_->axis);
                if (!angle) {
                    gizmo_->clearActive();
                    return;
                }
                gizmo_->startAngle = *angle;
            } else {
                const auto parameter = closestAxisParameter(ray, gizmo_->startPivotWorld, interactionAxis(*gizmo_));
                if (!parameter) {
                    gizmo_->clearActive();
                    return;
                }
                gizmo_->startParameter = *parameter;
            }
            gizmoInputConsumed_ = true;
            document_.beginGesture(selectedRecord.id);
        } catch (const std::exception& error) {
            gizmo_->clearActive();
            showError(error);
        }
    }

    // Draw the live frame after processing input. The viewport caller uses
    // gizmoInputConsumed_ to prevent the same click reaching CPU picking or a
    // light marker.
    const auto drawVisualState =
        gizmo_->active ? visual : visualFor(sceneView_, scene, selected, position, size, gizmo_->operation);
    if (drawVisualState.valid)
        drawVisual(draw, sceneView_, drawVisualState, gizmo_->operation, gizmo_->active ? gizmo_->axis : GizmoAxis::All,
                   gizmo_->hovered ? hoverAxis : GizmoAxis::All, position, size);
}

} // namespace proto
