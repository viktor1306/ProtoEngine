#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "assets/AssetTypes.hpp"
#include "scene/Lighting.hpp"

namespace proto {
enum class Primitive { Cube, Plane };
struct RenderItem {
    glm::mat4 world{1};
    glm::vec4 color{1};
    Primitive primitive{};
    bool selected{}, wireOnly{};
};
struct ImportedItem {
    AssetId mesh;
    glm::mat4 world{1};
    glm::mat3 normal{1};
    glm::vec4 color{1};
    std::vector<AssetId> materials;
    EntityId id;
    Bounds bounds;
    bool receiveShadows{true};
};
enum class LightingMode { Scene, Studio };
struct SceneLight {
    EntityId id;
    glm::vec3 position{}, direction{0, -1, 0}, color{1};
    float intensity{1}, radius{8};
    bool directional{}, shadows{true};
};
struct ShadowAtlasSettings {
    bool enabled{true};
    uint32_t resolution{512};
    uint32_t filterTaps{4};
    uint32_t poolBudgetMiB{};
};
struct RuntimeLightingSettings {
    // False means preserve the scene's legacy shared lighting path exactly.
    bool explicitSettings{};
    float viewDistance{};
    ShadowAtlasSettings sun;
    ShadowAtlasSettings point;
    uint32_t textureTopMipDrop{};
};
struct DepthProbe {
    float u{}, v{}, depth{};
};
struct ColorProbe {
    float u{}, v{};
    glm::vec3 rgb{};
    float tolerance{4};
    std::string label;
};
struct RenderView {
    glm::mat4 viewProjection{1};
    std::vector<RenderItem> items;
    std::vector<DepthProbe> depthProbes;
    std::vector<ColorProbe> colorProbes;
    bool grid{true};
    std::shared_ptr<AssetCatalog> assets;
    std::vector<ImportedItem> imported;
    glm::vec3 eye{};
    bool exactDepth{true};
    LightingMode lightingMode{LightingMode::Scene};
    LightingSettings lighting;
    glm::mat4 cameraView{1};
    float verticalFovDegrees{60}, cameraNear{.05f}, cameraFar{500}, exposure{1};
    std::vector<SceneLight> lights;
    std::vector<ImportedItem> casters;
    RuntimeLightingSettings runtimeLighting;
};
struct SceneReadback {
    size_t geometryPixels{}, depthSamples{}, colorSamples{};
    bool finiteDepth{}, matchesCpu{}, exactDepth{true};
    bool passed() const {
        return geometryPixels > 100 && (depthSamples > 0 || !exactDepth) && finiteDepth && matchesCpu;
    }
};
} // namespace proto
