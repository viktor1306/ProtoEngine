#include "editor/Editor.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace proto {
namespace {
constexpr std::array<uint32_t, 4> shadowResolutions{256, 512, 1024, 2048};

const char* resolutionLabel(uint32_t resolution) {
    switch (resolution) {
    case 256:
        return "256 px";
    case 512:
        return "512 px";
    case 1024:
        return "1024 px";
    case 2048:
        return "2048 px";
    default:
        return "Невідома роздільна здатність";
    }
}

LightingSettings lowLightingPreset() {
    LightingSettings result;
    result.shadowResolution = 512;
    result.shadowCascades = 2;
    result.shadowPoolMiB = 16;
    result.shadowDistance = 30;
    result.depthBias = .003f;
    result.normalBias = .03f;
    result.ambient = .04f;
    result.filteredShadows = false;
    return result;
}

LightingSettings balancedLightingPreset() {
    return LightingSettings{};
}

LightingSettings highLightingPreset() {
    LightingSettings result;
    result.shadowResolution = 2048;
    result.shadowCascades = 4;
    result.shadowPoolMiB = 256;
    result.shadowDistance = 100;
    result.depthBias = .0005f;
    result.normalBias = .01f;
    result.ambient = .02f;
    return result;
}

void setFullWidth() {
    ImGui::SetNextItemWidth(-1);
}

struct LightMarker {
    EntityId id;
    ImVec2 screen{};
    glm::vec3 world{}, direction{}, color{1};
    float radius{};
    bool point{};
};
} // namespace

void Editor::addLight(bool point) {
    try {
        EntityRecord record;
        record.name = point ? "Точкове світло" : "Сонце";
        if (point) {
            record.pointLight = PointLight{};
            record.transform.position = {2, 3, 2};
        } else {
            record.directionalLight = DirectionalLight{};
            record.transform.position = {0, 4, 0};
            const auto rays = glm::normalize(glm::vec3(-.4f, -.7f, -1.0f));
            record.transform.rotation = glm::normalize(glm::quatLookAt(rays, glm::vec3(0, 1, 0)));
        }
        document_.create(std::move(record));
    } catch (const std::exception& e) {
        showError(e);
    }
}

void Editor::lightInspector(EntityRecord& record) {
    if (!record.directionalLight && !record.pointLight)
        return;

    const bool point = record.pointLight.has_value();
    ImGui::SeparatorText(point ? "Точкове освітлення" : "Напрямлене освітлення");
    ImGui::TextDisabled("Параметри світла");
    if (point) {
        auto& light = *record.pointLight;
        ImGui::TextDisabled("Колір");
        propertyGesture(ImGui::ColorEdit3("##LightColor", &light.color.r), record);
        ImGui::TextDisabled("Відносна інтенсивність");
        propertyGesture(ImGui::DragFloat("##LightIntensity", &light.intensity, .1f, 0, 100000, "%.3f"), record);
        ImGui::TextDisabled("Радіус, світові метри");
        propertyGesture(ImGui::DragFloat("##LightRadius", &light.radius, .05f, .1f, 10000, "%.2f м"), record);
        ImGui::TextDisabled("Радіус не масштабується разом із Transform.");
        propertyGesture(ImGui::Checkbox("Тіні", &light.shadows), record);
    } else {
        auto& light = *record.directionalLight;
        ImGui::TextDisabled("Колір");
        propertyGesture(ImGui::ColorEdit3("##LightColor", &light.color.r), record);
        ImGui::TextDisabled("Відносна інтенсивність");
        propertyGesture(ImGui::DragFloat("##LightIntensity", &light.intensity, .1f, 0, 100000, "%.3f"), record);
        propertyGesture(ImGui::Checkbox("Тіні", &light.shadows), record);
        ImGui::TextDisabled("Промені йдуть уздовж локальної осі -Z.");
    }
}

void Editor::lightGizmos(ImVec2 position, ImVec2 size) {
    auto& scene = document_.scene;
    scene.update();

    const auto project = [&](const glm::vec3& world, ImVec2& screen) {
        const glm::vec4 clip = sceneView_.viewProjection * glm::vec4(world, 1);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
            clip.w <= .00001f)
            return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z) || ndc.z < 0 || ndc.z > 1)
            return false;
        screen = {position.x + (ndc.x * .5f + .5f) * size.x, position.y + (.5f - ndc.y * .5f) * size.y};
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    };

    std::vector<LightMarker> markers;
    markers.reserve(scene.directionalLights().size() + scene.pointLights().size());
    const auto addMarker = [&](EntityHandle handle, bool point, const glm::vec3& color, float radius,
                               const glm::vec3& direction) {
        const auto& transform = scene.transform(handle);
        if (!transform.visible || transform.degenerate)
            return;
        const glm::vec3 world(transform.world[3]);
        ImVec2 screen;
        if (!project(world, screen))
            return;
        constexpr float edgeMargin = 12;
        if (screen.x < position.x - edgeMargin || screen.x > position.x + size.x + edgeMargin ||
            screen.y < position.y - edgeMargin || screen.y > position.y + size.y + edgeMargin)
            return;
        markers.push_back({scene.entity(handle).id, screen, world, direction, color, radius, point});
    };

    for (const auto handle : scene.directionalLights()) {
        const auto& transform = scene.transform(handle);
        const auto* light = scene.directionalLight(handle);
        if (!light)
            continue;
        glm::vec3 direction = -glm::vec3(transform.world[2]);
        const float length = glm::length(direction);
        if (!std::isfinite(length) || length < .00001f)
            direction = {0, -1, 0};
        else
            direction /= length;
        addMarker(handle, false, light->color, 0, direction);
    }
    for (const auto handle : scene.pointLights()) {
        const auto* light = scene.pointLight(handle);
        if (light)
            addMarker(handle, true, light->color, light->radius, {0, -1, 0});
    }

    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(position, {position.x + size.x, position.y + size.y}, true);
    const auto packedColor = [](const glm::vec3& color) {
        return ImGui::ColorConvertFloat4ToU32(
            {std::clamp(color.r, 0.0f, 1.0f), std::clamp(color.g, 0.0f, 1.0f), std::clamp(color.b, 0.0f, 1.0f), .95f});
    };
    const ImU32 selectionColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 outlineColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
    for (const auto& marker : markers) {
        const bool selected = marker.id == document_.selection;
        const auto color = packedColor(marker.color);
        const float markerSize = selected ? 6.0f : 5.0f;
        if (selected && marker.point) {
            ImVec2 projectedRadius;
            float ringRadius = 12;
            if (project(marker.world + glm::vec3(marker.radius, 0, 0), projectedRadius)) {
                ringRadius = std::clamp(
                    std::hypot(projectedRadius.x - marker.screen.x, projectedRadius.y - marker.screen.y), 8.0f, 72.0f);
            }
            draw->AddCircle(marker.screen, ringRadius, color, 24, 1.2f);
        } else if (selected) {
            ImVec2 endpoint;
            const float distance = glm::length(marker.world - sceneView_.eye);
            const float arrowLength = std::clamp(std::isfinite(distance) ? distance * .08f : 2.0f, 1.0f, 8.0f);
            if (project(marker.world + marker.direction * arrowLength, endpoint)) {
                draw->AddLine(marker.screen, endpoint, color, 1.8f);
                const glm::vec2 delta(endpoint.x - marker.screen.x, endpoint.y - marker.screen.y);
                const float screenLength = glm::length(delta);
                if (screenLength > .001f) {
                    const glm::vec2 unit = delta / screenLength;
                    const glm::vec2 normal(-unit.y, unit.x);
                    const ImVec2 wingA(endpoint.x - unit.x * 8 + normal.x * 4, endpoint.y - unit.y * 8 + normal.y * 4);
                    const ImVec2 wingB(endpoint.x - unit.x * 8 - normal.x * 4, endpoint.y - unit.y * 8 - normal.y * 4);
                    draw->AddTriangleFilled(endpoint, wingA, wingB, color);
                }
            }
        }
        if (marker.point) {
            draw->AddCircleFilled(marker.screen, markerSize, color, 12);
            draw->AddCircle(marker.screen, markerSize, outlineColor, 12, 1.0f);
        } else {
            const ImVec2 top(marker.screen.x, marker.screen.y - markerSize);
            const ImVec2 right(marker.screen.x + markerSize, marker.screen.y);
            const ImVec2 bottom(marker.screen.x, marker.screen.y + markerSize);
            const ImVec2 left(marker.screen.x - markerSize, marker.screen.y);
            draw->AddTriangleFilled(top, right, bottom, color);
            draw->AddTriangleFilled(top, bottom, left, color);
            draw->AddLine(top, right, outlineColor, 1.0f);
            draw->AddLine(right, bottom, outlineColor, 1.0f);
            draw->AddLine(bottom, left, outlineColor, 1.0f);
            draw->AddLine(left, top, outlineColor, 1.0f);
        }
        if (selected)
            draw->AddCircle(marker.screen, markerSize + 3, selectionColor, 16, 1.4f);
    }
    draw->PopClipRect();

    auto& io = ImGui::GetIO();
    const bool insideViewport = io.MousePos.x >= position.x && io.MousePos.x <= position.x + size.x &&
                                io.MousePos.y >= position.y && io.MousePos.y <= position.y + size.y;
    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool cameraDrag = io.MouseDown[ImGuiMouseButton_Right] || io.MouseDown[ImGuiMouseButton_Middle];
    size_t nearest{};
    float nearestDistance = 100.0f;
    if (insideViewport && !popupOpen) {
        for (size_t i = 0; i < markers.size(); ++i) {
            const float dx = markers[i].screen.x - io.MousePos.x;
            const float dy = markers[i].screen.y - io.MousePos.y;
            const float distance = dx * dx + dy * dy;
            if (distance < nearestDistance) {
                nearest = i;
                nearestDistance = distance;
            }
        }
    }
    if (nearestDistance < 100.0f && !markers.empty()) {
        const auto& marker = markers[nearest];
        const auto handle = scene.find(marker.id);
        if (handle)
            ImGui::SetTooltip("%s\n%s", scene.entity(handle).name.c_str(), marker.point ? "Точкове світло" : "Сонце");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !cameraDrag && !io.WantTextInput && !gizmoActive() &&
            !gizmoInputConsumed_) {
            try {
                document_.endGesture();
                document_.selection = marker.id;
            } catch (const std::exception& e) {
                showError(e);
            }
        }
    }
}

void Editor::lightingSettings() {
    if (lightingDraftScene_ != document_.scene.id) {
        lightingDraft_ = document_.scene.lighting;
        lightingDraftScene_ = document_.scene.id;
    }

    if (!ImGui::Begin("Освітлення сцени###LightingSettings", &showLightingSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Зміни застосовуються до сцени після натискання «Застосувати».");
    ImGui::SeparatorText("Профіль");
    if (ImGui::Button("Низький"))
        lightingDraft_ = lowLightingPreset();
    ImGui::SameLine();
    if (ImGui::Button("Збалансований"))
        lightingDraft_ = balancedLightingPreset();
    ImGui::SameLine();
    if (ImGui::Button("Високий"))
        lightingDraft_ = highLightingPreset();

    ImGui::SeparatorText("Карти тіней");
    ImGui::TextDisabled("Роздільна здатність");
    setFullWidth();
    if (ImGui::BeginCombo("##ShadowResolution", resolutionLabel(lightingDraft_.shadowResolution))) {
        for (const auto resolution : shadowResolutions) {
            const bool selected = lightingDraft_.shadowResolution == resolution;
            if (ImGui::Selectable(resolutionLabel(resolution), selected))
                lightingDraft_.shadowResolution = resolution;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    int cascades = static_cast<int>(lightingDraft_.shadowCascades);
    ImGui::TextDisabled("Каскади сонця");
    setFullWidth();
    if (ImGui::SliderInt("##ShadowCascades", &cascades, 1, 4, "%d"))
        lightingDraft_.shadowCascades = static_cast<uint32_t>(cascades);

    ImGui::TextDisabled("Дальність тіней, м");
    setFullWidth();
    ImGui::DragFloat("##ShadowDistance", &lightingDraft_.shadowDistance, .1f, 1, 10000, "%.1f м");

    int poolMiB = static_cast<int>(lightingDraft_.shadowPoolMiB);
    ImGui::TextDisabled("Бюджет карт тіней, MiB");
    setFullWidth();
    if (ImGui::SliderInt("##ShadowPoolMiB", &poolMiB, 1, 512, "%d MiB"))
        lightingDraft_.shadowPoolMiB = static_cast<uint32_t>(poolMiB);

    ImGui::SeparatorText("Стабільність і якість");
    ImGui::TextDisabled("Depth bias");
    setFullWidth();
    ImGui::DragFloat("##DepthBias", &lightingDraft_.depthBias, .0001f, 0, .05f, "%.5f");
    ImGui::TextDisabled("Normal bias");
    setFullWidth();
    ImGui::DragFloat("##NormalBias", &lightingDraft_.normalBias, .001f, 0, 1, "%.3f");
    ImGui::TextDisabled("Ambient");
    setFullWidth();
    ImGui::SliderFloat("##Ambient", &lightingDraft_.ambient, 0, 1, "%.3f");

    ImGui::SeparatorText("Режими");
    ImGui::Checkbox("Тіні", &lightingDraft_.shadows);
    ImGui::Checkbox("Фільтровані тіні", &lightingDraft_.filteredShadows);
    if (ImGui::CollapsingHeader("Діагностика")) {
        ImGui::Checkbox("Порівняння: усі лампи", &lightingDraft_.referenceLighting);
        ImGui::Checkbox("Оновлювати тіні щокадру", &lightingDraft_.forceShadowRefresh);
    }

    ImGui::Separator();
    if (ImGui::Button("Застосувати")) {
        try {
            document_.lighting(lightingDraft_);
        } catch (const std::exception& e) {
            showError(e);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Скинути чернетку"))
        lightingDraft_ = LightingSettings{};

    ImGui::End();
}
} // namespace proto
