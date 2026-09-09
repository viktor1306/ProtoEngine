#include "editor/Editor.hpp"
#include "editor/GizmoSmoke.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace proto {
namespace {
bool close(float actual, float expected, float tolerance = 1.0e-3f) {
    return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= tolerance;
}

bool closeVec(glm::vec3 actual, glm::vec3 expected, float tolerance = 1.0e-3f) {
    return close(actual.x, expected.x, tolerance) && close(actual.y, expected.y, tolerance) &&
           close(actual.z, expected.z, tolerance);
}

bool closeMat3(const glm::mat3& actual, const glm::mat3& expected, float tolerance = 2.0e-3f) {
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            if (!close(actual[column][row], expected[column][row], tolerance))
                return false;
    return true;
}

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace

void Editor::startGizmoSmoke() {
    gizmoSmoke_ = std::make_shared<GizmoSmokeState>();
    gizmoSmoke_->enabled = true;
    document_.newScene();
    if (!gizmo_)
        gizmo_ = std::make_shared<GizmoState>();
    gizmo_->clearActive();
    gizmo_->operation = GizmoOperation::Translate;
    camera_ = {};
    camera_.pivot = {0.0f, 0.5f, 0.0f};
    camera_.yaw = 0.65f;
    camera_.pitch = 0.45f;
    camera_.distance = 8.0f;
    useSceneCamera_ = false;

    EntityRecord parent;
    parent.id = EntityId::create();
    parent.name = "Gizmo parent";
    parent.transform.position = {0.6f, 0.2f, -0.4f};
    parent.transform.rotation =
        glm::normalize(glm::angleAxis(0.35f, glm::vec3(0, 1, 0)) * glm::angleAxis(-0.18f, glm::vec3(1, 0, 0)));
    parent.transform.scale = {-1.7f, 1.3f, 0.6f};
    document_.scene.create(parent);

    EntityRecord child;
    child.id = EntityId::create();
    child.name = "Gizmo child";
    child.parent = parent.id;
    child.transform.position = {0.1f, 0.65f, -0.2f};
    child.transform.rotation = glm::normalize(glm::angleAxis(-0.22f, glm::vec3(0, 1, 0)));
    child.transform.scale = {1.2f, 0.8f, 0.9f};
    child.mesh = MeshRenderer{};
    document_.scene.create(child);
    document_.scene.update();
    document_.selection = child.id;

    gizmoSmoke_->parentId = parent.id;
    gizmoSmoke_->childId = child.id;
    gizmoSmoke_->selectionBefore = document_.selection;
    gizmoSmoke_->parentBaseline = document_.scene.record(document_.scene.find(parent.id));
    gizmoSmoke_->baseline = document_.scene.record(document_.scene.find(child.id));
    gizmoSmoke_->baselineWorld = document_.scene.transform(document_.scene.find(child.id)).world;
    gizmoSmoke_->baselineHistory = document_.undoCount();
    gizmoSmoke_->dirtyBefore = document_.dirty();
    gizmoSmoke_->cameraPivot = camera_.pivot;
    gizmoSmoke_->cameraYaw = camera_.yaw;
    gizmoSmoke_->cameraPitch = camera_.pitch;
    gizmoSmoke_->cameraDistance = camera_.distance;
}

bool Editor::gizmoSmokePassed() const {
    return gizmoSmoke_ && gizmoSmoke_->passed;
}

void Editor::gizmoSmokeStep(uint64_t frame) {
    if (!gizmoSmoke_ || !gizmoSmoke_->enabled || gizmoSmoke_->passed)
        return;
    auto& smoke = *gizmoSmoke_;
    const auto selected = document_.scene.find(smoke.childId);
    if (!selected)
        throw std::runtime_error("Gizmo smoke child disappeared");
    const auto requireStableContext = [&] {
        if (document_.selection != smoke.selectionBefore)
            throw std::runtime_error("Gizmo changed selection during smoke at stage " + std::to_string(smoke.stage) +
                                     ": expected " + smoke.selectionBefore.string() + ", got " +
                                     document_.selection.string() +
                                     ", active=" + (gizmo_ && gizmo_->active ? "true" : "false") +
                                     ", consumed=" + (gizmoInputConsumed_ ? "true" : "false"));
        require(document_.scene.record(document_.scene.find(smoke.parentId)) == smoke.parentBaseline,
                "Gizmo changed the nonselected parent during smoke");
        require(closeVec(camera_.pivot, smoke.cameraPivot) && close(camera_.yaw, smoke.cameraYaw) &&
                    close(camera_.pitch, smoke.cameraPitch) && close(camera_.distance, smoke.cameraDistance),
                "Gizmo changed editor camera during smoke");
    };
    const auto queueMouse = [&](glm::vec2 point, int button) {
        testInput_ = [point, button] {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            if (button >= 0)
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, button != 0);
        };
    };
    const auto queueEscape = [&](bool down) {
        testInput_ = [down] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, down); };
    };
    const auto axisDirection = [&] {
        const glm::vec2 delta = gizmo_->axisScreen[0] - gizmo_->pivotScreen;
        const float length = glm::length(delta);
        return length > 1.0e-4f ? delta / length : glm::vec2{1, 0};
    };
    const auto checkTransformAndParent = [&](const EntityRecord& before, const EntityRecord& after,
                                             bool positionChanged, bool rotationChanged, bool scaleChanged) {
        require(after.parent == smoke.parentId, "Gizmo changed parent while dragging");
        require(after.transform.position == before.transform.position || positionChanged,
                "Unexpected local position change");
        require(after.transform.rotation == before.transform.rotation || rotationChanged,
                "Unexpected local rotation change");
        require(after.transform.scale == before.transform.scale || scaleChanged, "Unexpected local scale change");
    };

    // Wait for a stable viewport projection and then drive the translate axis.
    if (smoke.stage == 0 && frame >= 8) {
        require(gizmo_ && glm::length(gizmo_->axisScreen[0] - gizmo_->pivotScreen) > 10.0f,
                "Gizmo translate handle was not projected");
        gizmo_->operation = GizmoOperation::Translate;
        queueMouse(gizmo_->axisScreen[0], 1);
        smoke.stage = 1;
    } else if (smoke.stage == 1) {
        requireStableContext();
        queueMouse(gizmo_->axisScreen[0] + axisDirection() * 40.0f, -1);
        smoke.stage = 2;
    } else if (smoke.stage == 2) {
        // The active gesture must commit even after the cursor leaves the
        // viewport rectangle; this also guards against the old IsItemHovered
        // lifetime mistake.
        queueMouse({-20.0f, -20.0f}, 0);
        smoke.stage = 3;
    } else if (smoke.stage == 3 && frame >= 12) {
        const auto after = document_.scene.record(selected);
        const auto& afterWorld = document_.scene.transform(selected).world;
        const glm::vec3 worldDelta = glm::vec3(afterWorld[3]) - glm::vec3(smoke.baselineWorld[3]);
        require(glm::length(worldDelta) > 0.05f, "Translate gizmo did not move the child");
        require(closeMat3(glm::mat3(afterWorld), glm::mat3(smoke.baselineWorld)),
                "World translation changed the child's linear transform");
        checkTransformAndParent(smoke.baseline, after, true, false, false);
        require(document_.undoCount() == smoke.baselineHistory + 1, "Translate drag did not create one Undo command");
        smoke.afterTranslate = after;
        document_.undo();
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.baseline, "Translate Undo failed");
        document_.redo();
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.afterTranslate,
                "Translate Redo failed");
        document_.undo();
        requireStableContext();
        smoke.stage = 4;
    } else if (smoke.stage == 4 && frame >= 14) {
        gizmo_->operation = GizmoOperation::Rotate;
        require(glm::length(gizmo_->ringStartScreens[1] - gizmo_->pivotScreen) > 20.0f,
                "Gizmo rotation ring was not projected");
        queueMouse(gizmo_->ringStartScreens[1], 1);
        smoke.stage = 5;
    } else if (smoke.stage == 5) {
        const int index = std::max(0, axisIndex(gizmo_->axis));
        queueMouse(gizmo_->ringStartScreens[index] + gizmo_->ringTangentScreens[index] * 32.0f, -1);
        smoke.stage = 6;
    } else if (smoke.stage == 6) {
        const int index = std::max(0, axisIndex(gizmo_->axis));
        queueMouse(gizmo_->ringStartScreens[index] + gizmo_->ringTangentScreens[index] * 32.0f, 0);
        smoke.stage = 7;
    } else if (smoke.stage == 7 && frame >= 18) {
        const auto after = document_.scene.record(selected);
        const auto& afterWorld = document_.scene.transform(selected).world;
        require(after.transform.rotation != smoke.baseline.transform.rotation, "Rotate gizmo did not change rotation");
        require(closeVec(glm::vec3(afterWorld[3]), glm::vec3(smoke.baselineWorld[3])),
                "Local rotation moved the child pivot");
        checkTransformAndParent(smoke.baseline, after, false, true, false);
        require(document_.undoCount() == smoke.baselineHistory + 1, "Rotate drag did not create one Undo command");
        smoke.afterRotate = after;
        document_.undo();
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.baseline, "Rotate Undo failed");
        document_.redo();
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.afterRotate, "Rotate Redo failed");
        document_.undo();
        requireStableContext();
        smoke.stage = 8;
    } else if (smoke.stage == 8 && frame >= 20) {
        gizmo_->operation = GizmoOperation::Scale;
        require(glm::length(gizmo_->axisScreen[0] - gizmo_->pivotScreen) > 10.0f,
                "Gizmo scale handle was not projected");
        queueMouse(gizmo_->axisScreen[0], 1);
        smoke.stage = 9;
    } else if (smoke.stage == 9) {
        queueMouse(gizmo_->axisScreen[0] + axisDirection() * 30.0f, -1);
        smoke.stage = 10;
    } else if (smoke.stage == 10) {
        queueMouse(gizmo_->axisScreen[0] + axisDirection() * 30.0f, 0);
        smoke.stage = 11;
    } else if (smoke.stage == 11 && frame >= 24) {
        const auto after = document_.scene.record(selected);
        const auto& afterWorld = document_.scene.transform(selected).world;
        require(after.transform.scale.x != smoke.baseline.transform.scale.x, "Scale gizmo did not change scale");
        require(std::signbit(after.transform.scale.x) == std::signbit(smoke.baseline.transform.scale.x),
                "Scale gizmo did not preserve the scale sign");
        require(closeVec(glm::vec3(afterWorld[3]), glm::vec3(smoke.baselineWorld[3])),
                "Local scale moved the child pivot");
        checkTransformAndParent(smoke.baseline, after, false, false, true);
        require(document_.undoCount() == smoke.baselineHistory + 1, "Scale drag did not create one Undo command");
        smoke.afterScale = after;
        document_.undo();
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.baseline, "Scale Undo failed");
        requireStableContext();
        smoke.stage = 12;
    } else if (smoke.stage == 12 && frame >= 26) {
        gizmo_->operation = GizmoOperation::Translate;
        smoke.stage = 21;
    } else if (smoke.stage == 21) {
        queueMouse(gizmo_->axisScreen[0], 1);
        smoke.stage = 13;
    } else if (smoke.stage == 13) {
        requireStableContext();
        queueMouse(gizmo_->axisScreen[0] + axisDirection() * 35.0f, -1);
        smoke.stage = 14;
    } else if (smoke.stage == 14) {
        queueEscape(true);
        smoke.stage = 15;
    } else if (smoke.stage == 15) {
        queueEscape(false);
        smoke.stage = 16;
    } else if (smoke.stage == 16) {
        queueMouse(gizmo_->axisScreen[0] + axisDirection() * 35.0f, 0);
        smoke.stage = 17;
    } else if (smoke.stage == 17 && frame >= 31) {
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.baseline,
                "Escape did not cancel gizmo preview");
        require(document_.undoCount() == smoke.baselineHistory, "Escape cancellation polluted Undo history");
        require(document_.dirty() == smoke.dirtyBefore, "Escape cancellation changed dirty state");
        requireStableContext();
        smoke.stage = 18;
    } else if (smoke.stage == 18 && frame >= 33) {
        gizmo_->operation = GizmoOperation::Translate;
        queueMouse(gizmo_->axisScreen[0], 1);
        smoke.stage = 19;
    } else if (smoke.stage == 19) {
        queueMouse(gizmo_->axisScreen[0], 0);
        smoke.stage = 20;
    } else if (smoke.stage == 20 && frame >= 36) {
        require(document_.scene.record(document_.scene.find(smoke.childId)) == smoke.baseline,
                "No-op gizmo drag changed the transform");
        require(document_.undoCount() == smoke.baselineHistory, "No-op gizmo drag polluted Undo history");
        requireStableContext();
        smoke.passed = true;
        smoke.enabled = false;
    }
}

} // namespace proto
