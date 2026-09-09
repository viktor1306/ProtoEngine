#include "scene/Scene.hpp"
#include "behavior/BehaviorSchema.hpp"
#include "core/Diagnostics.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace proto {
namespace {
std::atomic_uint64_t nextGeneration{1};
glm::quat normalizeRotation(glm::quat q) {
    // Avoid accumulating float roundoff on repeated save/load or unchanged edits.
    return std::abs(glm::length(q) - 1.0f) > 0.000001f ? glm::normalize(q) : q;
}
bool finite(const glm::mat4& value) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(value[c][r]))
                return false;
    return true;
}
void validateLightColor(const glm::vec3& color) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0.0f || color[i] > 1.0f)
            throw std::runtime_error("Колір освітлення має бути в діапазоні 0..1");
}
void validateLightIntensity(float intensity) {
    if (!std::isfinite(intensity) || intensity < 0.0f || intensity > 100000.0f)
        throw std::runtime_error("Інтенсивність освітлення має бути в діапазоні 0..100000");
}
void validateBehaviors(const std::vector<sdk::BehaviorBinding>& behaviors) {
    if (behaviors.size() > 4096)
        throw std::runtime_error("Об’єкт має забагато поведінок");
    std::unordered_set<BehaviorBindingId> ids;
    for (const auto& behavior : behaviors) {
        validateBehaviorBinding(behavior);
        if (!ids.insert(behavior.id).second)
            throw std::runtime_error("Дублікат BehaviorBindingId на об’єкті");
    }
}
void validateBehaviorIdsAvailable(const std::unordered_map<BehaviorBindingId, EntityId>& existing,
                                  const std::vector<sdk::BehaviorBinding>& behaviors, EntityId replacing = {}) {
    for (const auto& behavior : behaviors) {
        const auto found = existing.find(behavior.id);
        if (found != existing.end() && found->second != replacing)
            throw std::runtime_error("Дублікат BehaviorBindingId у сцені");
    }
}
} // namespace
glm::mat4 matrix(const Transform& t) {
    return glm::translate(glm::mat4(1), t.position) * glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1), t.scale);
}
Transform decomposeTrs(const glm::mat4& value) {
    Transform result;
    glm::vec3 skew;
    glm::vec4 perspective;
    if (!finite(value) || !glm::decompose(value, result.scale, result.rotation, result.position, skew, perspective))
        throw std::runtime_error(
            "Неможливо зберегти світову трансформацію: вироджена матриця. Оберіть збереження локальних параметрів.");
    result.rotation = normalizeRotation(result.rotation);
    const auto rebuilt = matrix(result);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(rebuilt[c][r] - value[c][r]) > 0.0001f * std::max(1.0f, std::abs(value[c][r])))
                throw std::runtime_error(
                    "Переприв’язування створює shear. Оберіть збереження локальних параметрів або скасуйте дію.");
    return result;
}
void validateRecord(const EntityRecord& r) {
    if (!r.id)
        throw std::runtime_error("Об’єкт не має UUID");
    if (r.name.empty() || r.name.size() > 1024 || r.name.find('\0') != std::string::npos || !validUtf8(r.name))
        throw std::runtime_error("Некоректна назва об’єкта UTF-8");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(r.transform.position[i]) || !std::isfinite(r.transform.scale[i]))
            throw std::runtime_error("Трансформація повинна містити скінченні числа");
    const float length = glm::length(r.transform.rotation);
    if (!std::isfinite(length) || length < 0.000001f)
        throw std::runtime_error("Некоректний quaternion");
    if (r.mesh) {
        if (!r.mesh->mesh)
            throw std::runtime_error("Missing mesh AssetId");
        if ((r.mesh->mesh == builtin::cube || r.mesh->mesh == builtin::plane) && !r.mesh->materials.empty())
            throw std::runtime_error("Builtin debug geometry does not have PBR material slots");
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(r.mesh->color[i]) || r.mesh->color[i] < 0 || r.mesh->color[i] > 1)
                throw std::runtime_error("Колір має бути в діапазоні 0..1");
        if (r.mesh->color.a != 1)
            throw std::runtime_error("M1 підтримує лише непрозорі кольори");
    }
    if (r.camera) {
        const auto& c = *r.camera;
        if (!std::isfinite(c.verticalFovDegrees) || c.verticalFovDegrees < 5 || c.verticalFovDegrees > 150 ||
            !std::isfinite(c.nearPlane) || c.nearPlane < 0.001f || !std::isfinite(c.farPlane) ||
            c.farPlane <= c.nearPlane || c.farPlane > 1000000 || !std::isfinite(c.exposure) || c.exposure <= 0 ||
            c.exposure > 100)
            throw std::runtime_error("Некоректні параметри камери");
    }
    if (r.directionalLight) {
        validateLightColor(r.directionalLight->color);
        validateLightIntensity(r.directionalLight->intensity);
    }
    if (r.pointLight) {
        validateLightColor(r.pointLight->color);
        validateLightIntensity(r.pointLight->intensity);
        if (!std::isfinite(r.pointLight->radius) || r.pointLight->radius < 0.1f || r.pointLight->radius > 10000.0f)
            throw std::runtime_error("Радіус точкового освітлення має бути в діапазоні 0.1..10000");
    }
    if (r.directionalLight && r.pointLight)
        throw std::runtime_error("Об’єкт не може містити одночасно DirectionalLight і PointLight");
    for (const auto& behavior : r.behaviors)
        validateBehaviorBinding(behavior);
}
void validateLighting(const LightingSettings& settings) {
    if (settings.shadowResolution != 256 && settings.shadowResolution != 512 && settings.shadowResolution != 1024 &&
        settings.shadowResolution != 2048)
        throw std::runtime_error("Некоректна роздільна здатність shadow map");
    if (settings.shadowCascades < 1 || settings.shadowCascades > 4)
        throw std::runtime_error("Кількість каскадів має бути в діапазоні 1..4");
    if (settings.shadowPoolMiB < 1 || settings.shadowPoolMiB > 512)
        throw std::runtime_error("Розмір shadow pool має бути в діапазоні 1..512 MiB");
    if (!std::isfinite(settings.shadowDistance) || settings.shadowDistance < 1.0f || settings.shadowDistance > 10000.0f)
        throw std::runtime_error("Дальність тіней має бути в діапазоні 1..10000");
    if (!std::isfinite(settings.depthBias) || settings.depthBias < 0.0f || settings.depthBias > 0.05f)
        throw std::runtime_error("Depth bias має бути в діапазоні 0..0.05");
    if (!std::isfinite(settings.normalBias) || settings.normalBias < 0.0f || settings.normalBias > 1.0f)
        throw std::runtime_error("Normal bias має бути в діапазоні 0..1");
    if (!std::isfinite(settings.ambient) || settings.ambient < 0.0f || settings.ambient > 1.0f)
        throw std::runtime_error("Ambient має бути в діапазоні 0..1");
    const uint64_t oneSlotBytes = uint64_t(settings.shadowResolution) * settings.shadowResolution * 4u * 6u;
    const uint64_t poolBytes = uint64_t(settings.shadowPoolMiB) * 1024u * 1024u;
    if (poolBytes < oneSlotBytes)
        throw std::runtime_error("Shadow pool замалий для одного shadow slot");
}
EntityHandle Scene::find(EntityId value) const {
    const auto found = ids_.find(value);
    return found == ids_.end() ? EntityHandle{} : found->second;
}
EntityRecord Scene::record(EntityHandle handle) const {
    const auto& e = entity(handle);
    EntityRecord result;
    result.id = e.id;
    result.name = e.name;
    result.enabled = e.enabled;
    result.parent = e.parent ? entity(e.parent).id : EntityId{};
    result.transform = transform(handle).local;
    if (const auto* m = mesh(handle))
        result.mesh = *m;
    if (const auto* c = camera(handle))
        result.camera = *c;
    if (const auto* light = directionalLight(handle))
        result.directionalLight = *light;
    if (const auto* light = pointLight(handle))
        result.pointLight = *light;
    if (behaviorBindings_.contains(handle))
        result.behaviors = behaviorBindings_.get(handle);
    return result;
}
EntityHandle Scene::create(const EntityRecord& r) {
    validateRecord(r);
    validateBehaviors(r.behaviors);
    validateBehaviorIdsAvailable(behaviorIds_, r.behaviors);
    if (r.mesh && r.mesh->mesh != builtin::cube && r.mesh->mesh != builtin::plane) {
        auto it = assets->meshes.find(r.mesh->mesh);
        if (it == assets->meshes.end())
            throw std::runtime_error("Unknown mesh AssetId");
        if (!r.mesh->materials.empty() && r.mesh->materials.size() != it->second->parts.size())
            throw std::runtime_error("Material slot count mismatch");
        for (auto id : r.mesh->materials)
            if (!assets->materials.contains(id))
                throw std::runtime_error("Unknown material AssetId");
    }
    if (find(r.id))
        throw std::runtime_error("Дублікат EntityId");
    const auto parent = find(r.parent);
    if (r.parent && !parent)
        throw std::runtime_error("Невідомий parent EntityId");
    size_t depth{};
    for (auto at = parent; at; at = entity(at).parent)
        if (++depth > 256)
            throw std::runtime_error("Ієрархія перевищує 256 рівнів");
    if (entities_.size() >= 100000)
        throw std::runtime_error("Ліміт M1: 100000 об’єктів");
    const auto generation = nextGeneration.fetch_add(1);
    if (generation > UINT32_MAX)
        throw std::runtime_error("Runtime generations exhausted; restart the process");
    const auto slot = free_.empty() ? slots_++ : free_.back();
    if (!free_.empty())
        free_.pop_back();
    EntityHandle handle{slot, static_cast<uint32_t>(generation)};
    entities_.insert(handle, Entity{r.id, r.name, {}, {}, r.enabled});
    TransformState transform;
    transform.local = r.transform;
    transform.local.rotation = normalizeRotation(transform.local.rotation);
    transforms_.insert(handle, transform);
    ids_.emplace(r.id, handle);
    if (r.mesh)
        meshes_.insert(handle, *r.mesh);
    if (r.camera)
        cameras_.insert(handle, *r.camera);
    if (r.directionalLight)
        directionalLights_.insert(handle, *r.directionalLight);
    if (r.pointLight)
        pointLights_.insert(handle, *r.pointLight);
    if (!r.behaviors.empty())
        behaviorBindings_.insert(handle, r.behaviors);
    for (const auto& binding : r.behaviors)
        behaviorIds_.emplace(binding.id, r.id);
    link(handle, parent);
    return handle;
}
void Scene::dirty(EntityHandle root) {
    std::vector<EntityHandle> stack{root};
    while (!stack.empty()) {
        const auto h = stack.back();
        stack.pop_back();
        transforms_.get(h).dirty = true;
        const auto& children = entity(h).children;
        stack.insert(stack.end(), children.begin(), children.end());
    }
    changed_ = true;
}
void Scene::validateParent(EntityHandle child, EntityHandle parent) const {
    entity(child);
    size_t depth{};
    for (auto at = parent; at; at = entity(at).parent) {
        if (at == child)
            throw std::runtime_error("Ієрархія не може містити цикл");
        if (++depth > 256)
            throw std::runtime_error("Ієрархія перевищує 256 рівнів");
    }
}
void Scene::link(EntityHandle child, EntityHandle parent) {
    validateParent(child, parent);
    auto& e = entities_.get(child);
    if (e.parent == parent) {
        dirty(child);
        return;
    }
    if (e.parent)
        std::erase(entities_.get(e.parent).children, child);
    e.parent = parent;
    if (parent)
        entities_.get(parent).children.push_back(child);
    dirty(child);
}
void validateTransformValue(const Transform& value) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(value.position[i]) || !std::isfinite(value.scale[i]))
            throw std::runtime_error("Трансформація повинна містити скінченні числа");
    const float length = glm::length(value.rotation);
    if (!std::isfinite(length) || length < 0.000001f)
        throw std::runtime_error("Некоректний quaternion");
}
void Scene::setTransform(EntityHandle h, Transform value) {
    entity(h);
    validateTransformValue(value);
    auto& local = transforms_.get(h).local;
    const auto previous = local;
    local = value;
    local.rotation = normalizeRotation(local.rotation);
    dirty(h);
    try {
        update();
    } catch (...) {
        local = previous;
        dirty(h);
        update();
        throw;
    }
}
void Scene::setTransformDeferred(EntityHandle h, Transform value) {
    entity(h);
    validateTransformValue(value);
    auto& state = transforms_.get(h);
    state.local = value;
    state.local.rotation = normalizeRotation(state.local.rotation);
    // dirty() establishes the invariant that a dirty subtree has all dirty
    // descendants. Repeated Player writes before the next update can therefore
    // avoid traversing the same subtree again.
    if (!state.dirty)
        dirty(h);
    else
        changed_ = true;
}
void Scene::setPointLight(EntityHandle h, const PointLight& value) {
    entity(h);
    if (!pointLights_.contains(h))
        throw std::runtime_error("Entity has no PointLight");
    validateLightColor(value.color);
    validateLightIntensity(value.intensity);
    if (!std::isfinite(value.radius) || value.radius < 0.1f || value.radius > 10000.0f)
        throw std::runtime_error("Радіус точкового освітлення має бути в діапазоні 0.1..10000");
    pointLights_.get(h) = value;
}
void Scene::setDirectionalLight(EntityHandle h, const DirectionalLight& value) {
    entity(h);
    if (!directionalLights_.contains(h))
        throw std::runtime_error("Entity has no DirectionalLight");
    validateLightColor(value.color);
    validateLightIntensity(value.intensity);
    directionalLights_.get(h) = value;
}
void Scene::reparent(EntityHandle child, EntityHandle parent, bool keepWorld) {
    validateParent(child, parent);
    update();
    auto local = transform(child).local;
    if (keepWorld) {
        const auto parentWorld = parent ? transform(parent).world : glm::mat4(1);
        if (std::abs(glm::determinant(parentWorld)) < 1e-8f)
            throw std::runtime_error("Батько має нульовий масштаб; збереження світової трансформації неможливе");
        local = decomposeTrs(glm::inverse(parentWorld) * transform(child).world);
    }
    const auto oldParent = entity(child).parent;
    const auto oldLocal = transform(child).local;
    link(child, parent);
    transforms_.get(child).local = local;
    try {
        update();
    } catch (...) {
        link(child, oldParent);
        transforms_.get(child).local = oldLocal;
        update();
        throw;
    }
}
void Scene::edit(const EntityRecord& r) {
    validateRecord(r);
    validateBehaviors(r.behaviors);
    const auto h = find(r.id);
    if (!h)
        throw std::runtime_error("Невідомий EntityId");
    if (r.mesh && r.mesh->mesh != builtin::cube && r.mesh->mesh != builtin::plane) {
        const auto found = assets->meshes.find(r.mesh->mesh);
        if (found == assets->meshes.end())
            throw std::runtime_error("Unknown mesh AssetId");
        if (!r.mesh->materials.empty() && r.mesh->materials.size() != found->second->parts.size())
            throw std::runtime_error("Material slot count mismatch");
        for (const auto id : r.mesh->materials)
            if (!assets->materials.contains(id))
                throw std::runtime_error("Unknown material AssetId");
    }
    const auto parent = find(r.parent);
    if (r.parent && !parent)
        throw std::runtime_error("Невідомий parent EntityId");
    validateParent(h, parent);
    validateBehaviorIdsAvailable(behaviorIds_, r.behaviors, r.id);
    const auto before = record(h);
    link(h, parent);
    try {
        setTransform(h, r.transform);
    } catch (...) {
        link(h, find(before.parent));
        setTransform(h, before.transform);
        throw;
    }
    auto& e = entities_.get(h);
    e.name = r.name;
    e.enabled = r.enabled;
    if (r.mesh) {
        if (meshes_.contains(h))
            meshes_.get(h) = *r.mesh;
        else
            meshes_.insert(h, *r.mesh);
    } else
        meshes_.erase(h);
    if (r.camera) {
        if (cameras_.contains(h))
            cameras_.get(h) = *r.camera;
        else
            cameras_.insert(h, *r.camera);
    } else
        cameras_.erase(h);
    if (r.directionalLight) {
        if (directionalLights_.contains(h))
            directionalLights_.get(h) = *r.directionalLight;
        else
            directionalLights_.insert(h, *r.directionalLight);
    } else
        directionalLights_.erase(h);
    if (r.pointLight) {
        if (pointLights_.contains(h))
            pointLights_.get(h) = *r.pointLight;
        else
            pointLights_.insert(h, *r.pointLight);
    } else
        pointLights_.erase(h);
    for (const auto& binding : before.behaviors)
        behaviorIds_.erase(binding.id);
    if (!r.behaviors.empty()) {
        if (behaviorBindings_.contains(h))
            behaviorBindings_.get(h) = r.behaviors;
        else
            behaviorBindings_.insert(h, r.behaviors);
    } else
        behaviorBindings_.erase(h);
    for (const auto& binding : r.behaviors)
        behaviorIds_.emplace(binding.id, r.id);
    if (!r.camera && activeCamera == r.id)
        activeCamera = {};
    dirty(h);
    update();
}
void Scene::update() {
    if (!changed_)
        return;
    std::vector<std::pair<EntityHandle, unsigned>> queue;
    queue.reserve(entities_.size());
    for (const auto h : entities())
        if (!entity(h).parent)
            queue.emplace_back(h, 0);
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto [h, depth] = queue[i];
        if (depth > 256)
            throw std::runtime_error("Ієрархія перевищує 256 рівнів");
        const auto& e = entity(h);
        auto& t = transforms_.get(h);
        if (t.dirty) {
            t.world = (e.parent ? transform(e.parent).world : glm::mat4(1)) * matrix(t.local);
            if (!finite(t.world))
                throw std::runtime_error("Переповнення світової трансформації");
            t.visible = e.enabled && (!e.parent || transform(e.parent).visible);
            const float determinant = glm::determinant(glm::mat3(t.world));
            t.degenerate = !std::isfinite(determinant) || determinant == 0;
            t.normal = t.degenerate ? glm::mat3(1) : glm::transpose(glm::inverse(glm::mat3(t.world)));
            const auto* mesh = this->mesh(h);
            glm::vec3 half(0.5f);
            if (mesh && mesh->mesh == builtin::plane)
                half.y = 0;
            glm::vec3 localCenter(0);
            if (mesh && assets->meshes.contains(mesh->mesh)) {
                const auto& b = assets->meshes.at(mesh->mesh)->bounds;
                localCenter = (b.min + b.max) * .5f;
                half = (b.max - b.min) * .5f;
            }
            const glm::vec3 center(t.world * glm::vec4(localCenter, 1));
            const auto extent = glm::abs(glm::vec3(t.world[0])) * half.x + glm::abs(glm::vec3(t.world[1])) * half.y +
                                glm::abs(glm::vec3(t.world[2])) * half.z;
            t.bounds = {center - extent, center + extent};
            t.dirty = false;
            ++evaluations_;
        }
        for (const auto child : e.children)
            queue.emplace_back(child, depth + 1);
    }
    if (queue.size() != entities_.size())
        throw std::runtime_error("Некоректна ієрархія сцени");
    changed_ = false;
}
std::vector<EntityRecord> Scene::subtree(EntityHandle root) const {
    entity(root);
    std::vector<EntityHandle> queue{root};
    std::vector<EntityRecord> result;
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto h = queue[i];
        result.push_back(record(h));
        const auto& children = entity(h).children;
        queue.insert(queue.end(), children.begin(), children.end());
    }
    return result;
}
void Scene::eraseSubtree(EntityHandle root) {
    entity(root);
    std::vector<EntityHandle> handles{root};
    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& children = entity(handles[i]).children;
        handles.insert(handles.end(), children.begin(), children.end());
    }
    const auto parent = entity(root).parent;
    if (parent)
        std::erase(entities_.get(parent).children, root);
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const auto h = *it;
        const auto removed = entity(h).id;
        meshes_.erase(h);
        cameras_.erase(h);
        directionalLights_.erase(h);
        pointLights_.erase(h);
        for (const auto& binding : behaviors(h))
            behaviorIds_.erase(binding.id);
        behaviorBindings_.erase(h);
        transforms_.erase(h);
        entities_.erase(h);
        ids_.erase(removed);
        free_.push_back(h.slot);
        if (activeCamera == removed)
            activeCamera = {};
    }
    changed_ = true;
}
void Scene::restore(const std::vector<EntityRecord>& records) {
    std::unordered_set<EntityId> incoming;
    std::unordered_set<BehaviorBindingId> incomingBehaviors;
    for (const auto& r : records) {
        validateRecord(r);
        validateBehaviors(r.behaviors);
        if (find(r.id) || !incoming.insert(r.id).second)
            throw std::runtime_error("Дублікат EntityId");
        for (const auto& behavior : r.behaviors)
            if (!incomingBehaviors.insert(behavior.id).second)
                throw std::runtime_error("Дублікат BehaviorBindingId у відновлених об’єктах");
    }
    for (const auto id : incomingBehaviors)
        if (behaviorIds_.contains(id))
            throw std::runtime_error("Дублікат BehaviorBindingId у сцені");
    for (const auto& r : records)
        if (r.parent && !find(r.parent) && !incoming.contains(r.parent))
            throw std::runtime_error("Невідомий parent EntityId");
    std::vector<EntityHandle> created;
    try {
        for (auto r : records) {
            r.parent = {};
            created.push_back(create(r));
        }
        for (const auto& r : records)
            link(find(r.id), find(r.parent));
        update();
    } catch (...) {
        for (const auto& r : records)
            if (const auto h = find(r.id))
                eraseSubtree(h);
        update();
        throw;
    }
}
SceneSnapshot Scene::snapshot() const {
    SceneSnapshot result;
    result.id = id;
    result.name = name;
    result.activeCamera = activeCamera;
    result.assetRoot = assetRoot;
    result.modelSources = modelSources;
    result.lighting = lighting;
    for (const auto h : entities())
        result.entities.push_back(record(h));
    std::sort(result.entities.begin(), result.entities.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}
Scene Scene::fromSnapshot(const SceneSnapshot& snapshot, std::shared_ptr<AssetCatalog> assets) {
    if (!snapshot.id || snapshot.name.empty() || snapshot.name.size() > 1024 ||
        snapshot.name.find('\0') != std::string::npos || !validUtf8(snapshot.name))
        throw std::runtime_error("Некоректна назва або UUID сцени");
    validateLighting(snapshot.lighting);
    Scene result;
    if (assets)
        result.assets = std::move(assets);
    result.assetRoot = snapshot.assetRoot;
    result.modelSources = snapshot.modelSources;
    result.id = snapshot.id;
    result.name = snapshot.name;
    result.lighting = snapshot.lighting;
    result.restore(snapshot.entities);
    if (snapshot.activeCamera &&
        (!result.find(snapshot.activeCamera) || !result.camera(result.find(snapshot.activeCamera))))
        throw std::runtime_error("activeCamera посилається на невідому камеру");
    result.activeCamera = snapshot.activeCamera;
    return result;
}
Scene Scene::demo() {
    Scene result;
    EntityRecord cube;
    cube.id = EntityId::create();
    cube.name = "Куб";
    cube.transform.position.y = 0.5f;
    cube.mesh = MeshRenderer{};
    result.create(cube);
    EntityRecord plane;
    plane.id = EntityId::create();
    plane.name = "Площина";
    plane.transform.scale = {10, 1, 10};
    plane.mesh = MeshRenderer{builtin::plane, {0.16f, 0.20f, 0.25f, 1}};
    result.create(plane);
    EntityRecord camera;
    camera.id = EntityId::create();
    camera.name = "Камера";
    camera.transform.position = {5, 3, 6};
    camera.camera = Camera{};
    camera.transform.rotation =
        glm::quatLookAt(glm::normalize(glm::vec3(0, 0.5f, 0) - camera.transform.position), glm::vec3(0, 1, 0));
    result.create(camera);
    result.activeCamera = camera.id;
    result.update();
    return result;
}
std::optional<Hit> Scene::pick(const Ray& ray) const {
    std::optional<Hit> closest;
    for (const auto h : meshes()) {
        const auto& t = transform(h);
        if (!t.visible || t.degenerate)
            continue;
        const auto inverse = glm::inverse(t.world);
        const glm::vec3 origin(inverse * glm::vec4(ray.origin, 1)), direction(inverse * glm::vec4(ray.direction, 0));
        if (auto it = assets->meshes.find(mesh(h)->mesh); it != assets->meshes.end()) {
            const auto distance = pickMesh(*it->second, *assets, mesh(h)->materials, origin, direction,
                                           closest ? closest->distance : std::numeric_limits<float>::max());
            if (distance)
                closest = Hit{entity(h).id, *distance, ray.origin + ray.direction * *distance};
            continue;
        }
        glm::vec3 half(0.5f);
        if (mesh(h)->mesh == builtin::plane) {
            half.y = 0;
            if (direction.y >= -1e-8f)
                continue;
        }
        float near = -std::numeric_limits<float>::max(), far = std::numeric_limits<float>::max();
        bool hit = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(direction[axis]) < 1e-8f) {
                if (origin[axis] < -half[axis] || origin[axis] > half[axis])
                    hit = false;
                continue;
            }
            float a = (-half[axis] - origin[axis]) / direction[axis], b = (half[axis] - origin[axis]) / direction[axis];
            if (a > b)
                std::swap(a, b);
            near = std::max(near, a);
            far = std::min(far, b);
        }
        if (hit && near >= 0 && near <= far && far > 0 && (!closest || near < closest->distance))
            closest = Hit{entity(h).id, near, ray.origin + ray.direction * near};
    }
    return closest;
}
void Scene::assetsChanged() {
    for (auto h : entities())
        if (!entity(h).parent)
            dirty(h);
    update();
}
} // namespace proto
