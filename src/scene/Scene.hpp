#pragma once
#include <Proto/Behavior.hpp>
#include "scene/DensePool.hpp"
#include "scene/Lighting.hpp"
#include "assets/AssetTypes.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <span>
#include <unordered_map>

namespace proto {
namespace builtin {
inline const AssetId cube = AssetId::parse("00000000-0000-4000-8000-000000000001");
inline const AssetId plane = AssetId::parse("00000000-0000-4000-8000-000000000002");
} // namespace builtin
struct Transform {
    glm::vec3 position{0};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1};
    bool operator==(const Transform&) const = default;
};
struct TransformState {
    Transform local;
    glm::mat4 world{1};
    glm::mat3 normal{1};
    Bounds bounds;
    bool dirty{true}, visible{true}, degenerate{};
};
struct MeshRenderer {
    AssetId mesh{builtin::cube};
    glm::vec4 color{0.24f, 0.72f, 0.64f, 1};
    bool castShadows{true}, receiveShadows{true};
    std::vector<AssetId> materials;
    bool operator==(const MeshRenderer&) const = default;
};
struct Camera {
    float verticalFovDegrees{60}, nearPlane{0.05f}, farPlane{500}, exposure{1};
    bool operator==(const Camera&) const = default;
};
struct Entity {
    EntityId id;
    std::string name;
    EntityHandle parent;
    std::vector<EntityHandle> children;
    bool enabled{true};
};
struct EntityRecord {
    EntityId id;
    std::string name{"Об’єкт"};
    EntityId parent;
    bool enabled{true};
    Transform transform;
    std::optional<MeshRenderer> mesh;
    std::optional<Camera> camera;
    std::optional<DirectionalLight> directionalLight;
    std::optional<PointLight> pointLight;
    std::vector<sdk::BehaviorBinding> behaviors;
    bool operator==(const EntityRecord&) const = default;
};
struct SceneSnapshot {
    AssetId id;
    std::string name{"Без назви"};
    EntityId activeCamera;
    std::vector<EntityRecord> entities;
    std::string assetRoot;
    std::vector<AssetId> modelSources;
    LightingSettings lighting;
};
struct Ray {
    glm::vec3 origin, direction;
};
struct Hit {
    EntityId id;
    float distance{};
    glm::vec3 position;
};
glm::mat4 matrix(const Transform& transform);
Transform decomposeTrs(const glm::mat4& matrix);
void validateRecord(const EntityRecord& record);

class Scene {
  public:
    Scene() = default;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    AssetId id{AssetId::create()};
    std::string name{"Без назви"};
    EntityId activeCamera;
    std::shared_ptr<AssetCatalog> assets{std::make_shared<AssetCatalog>()};
    std::string assetRoot;
    std::vector<AssetId> modelSources;
    LightingSettings lighting;
    EntityHandle find(EntityId id) const;
    const Entity& entity(EntityHandle handle) const { return entities_.get(handle); }
    const TransformState& transform(EntityHandle handle) const { return transforms_.get(handle); }
    const MeshRenderer* mesh(EntityHandle handle) const {
        return meshes_.contains(handle) ? &meshes_.get(handle) : nullptr;
    }
    const Camera* camera(EntityHandle handle) const {
        return cameras_.contains(handle) ? &cameras_.get(handle) : nullptr;
    }
    const DirectionalLight* directionalLight(EntityHandle handle) const {
        return directionalLights_.contains(handle) ? &directionalLights_.get(handle) : nullptr;
    }
    const PointLight* pointLight(EntityHandle handle) const {
        return pointLights_.contains(handle) ? &pointLights_.get(handle) : nullptr;
    }
    std::span<const sdk::BehaviorBinding> behaviors(EntityHandle handle) const {
        return behaviorBindings_.contains(handle) ? std::span<const sdk::BehaviorBinding>(behaviorBindings_.get(handle))
                                                  : std::span<const sdk::BehaviorBinding>{};
    }
    std::span<const EntityHandle> entities() const { return entities_.owners(); }
    std::span<const EntityHandle> meshes() const { return meshes_.owners(); }
    std::span<const EntityHandle> directionalLights() const { return directionalLights_.owners(); }
    std::span<const EntityHandle> pointLights() const { return pointLights_.owners(); }
    EntityRecord record(EntityHandle handle) const;
    EntityHandle create(const EntityRecord& record);
    void edit(const EntityRecord& record);
    void setTransform(EntityHandle handle, Transform transform);
    // Player-only transform path: validates and publishes local data now;
    // world matrices are rebuilt by the caller's frame/lifecycle update.
    void setTransformDeferred(EntityHandle handle, Transform transform);
    void setPointLight(EntityHandle handle, const PointLight& light);
    void setDirectionalLight(EntityHandle handle, const DirectionalLight& light);
    void reparent(EntityHandle child, EntityHandle parent, bool keepWorld);
    std::vector<EntityRecord> subtree(EntityHandle root) const;
    void eraseSubtree(EntityHandle root);
    void restore(const std::vector<EntityRecord>& records);
    void update();
    SceneSnapshot snapshot() const;
    static Scene fromSnapshot(const SceneSnapshot& snapshot, std::shared_ptr<AssetCatalog> assets = {});
    void assetsChanged();
    static Scene demo();
    std::optional<Hit> pick(const Ray& ray) const;
    uint64_t transformEvaluations() const { return evaluations_; }

  private:
    void dirty(EntityHandle root);
    void link(EntityHandle child, EntityHandle parent);
    void validateParent(EntityHandle child, EntityHandle parent) const;
    DensePool<Entity> entities_;
    DensePool<TransformState> transforms_;
    DensePool<MeshRenderer> meshes_;
    DensePool<Camera> cameras_;
    DensePool<DirectionalLight> directionalLights_;
    DensePool<PointLight> pointLights_;
    DensePool<std::vector<sdk::BehaviorBinding>> behaviorBindings_;
    std::unordered_map<EntityId, EntityHandle> ids_;
    std::unordered_map<BehaviorBindingId, EntityId> behaviorIds_;
    std::vector<uint32_t> free_;
    uint32_t slots_{};
    bool changed_{true};
    uint64_t evaluations_{};
};
} // namespace proto
