#include "runtime/BehaviorRuntime.hpp"
#include "core/Diagnostics.hpp"
#include <cmath>

namespace proto {
namespace {
sdk::Vec3 pack(glm::vec3 v) {
    return {v.x, v.y, v.z};
}
glm::vec3 unpack(sdk::Vec3 v) {
    return {v.x, v.y, v.z};
}
std::string location(EntityId entity, const sdk::BehaviorBinding& binding) {
    return "entity=" + entity.string() + " behavior=" + binding.type.string() + " binding=" + binding.id.string();
}
} // namespace
class BehaviorRuntime::Context final : public sdk::BehaviorContext {
  public:
    Context(BehaviorRuntime& owner, EntityId self, sdk::PropertyMap properties)
        : owner_(owner), self_(self), properties_(std::move(properties)) {}
    EntityId Self() const override { return self_; }
    const sdk::PropertyMap& Properties() const override { return properties_; }
    bool Exists(EntityId id) const override { return bool(owner_.scene_.find(id)); }
    sdk::Transform GetTransform(EntityId id) const override {
        const auto& t = owner_.scene_.transform(handle(id)).local;
        return {pack(t.position), {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z}, pack(t.scale)};
    }
    void SetTransform(EntityId id, const sdk::Transform& t) override {
        owner_.scene_.setTransformDeferred(
            handle(id),
            {unpack(t.position), {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z}, unpack(t.scale)});
    }
    sdk::Vec3 GetWorldPosition(EntityId id) const override {
        owner_.scene_.update();
        return pack(glm::vec3(owner_.scene_.transform(handle(id)).world[3]));
    }
    std::span<const uint8_t> GetAssetBytes(AssetId id) const override {
        const auto& files = owner_.scene_.assets->dataFiles;
        const auto found = files.find(id);
        if (found == files.end())
            throw std::runtime_error("Raw asset is not included in this Player: " + id.string());
        return *found->second;
    }
    std::optional<sdk::PointLight> GetPointLight(EntityId id) const override {
        if (const auto* l = owner_.scene_.pointLight(handle(id)))
            return sdk::PointLight{pack(l->color), l->intensity, l->radius, l->shadows};
        return {};
    }
    void SetPointLight(EntityId id, const sdk::PointLight& l) override {
        owner_.scene_.setPointLight(handle(id), {unpack(l.color), l.intensity, l.radius, l.shadows});
    }
    std::optional<sdk::DirectionalLight> GetDirectionalLight(EntityId id) const override {
        if (const auto* l = owner_.scene_.directionalLight(handle(id)))
            return sdk::DirectionalLight{pack(l->color), l->intensity, l->shadows};
        return {};
    }
    void SetDirectionalLight(EntityId id, const sdk::DirectionalLight& l) override {
        owner_.scene_.setDirectionalLight(handle(id), {unpack(l.color), l.intensity, l.shadows});
    }
    bool KeyDown(sdk::Key key) const override {
        return size_t(key) < size_t(sdk::Key::Count) && owner_.input_.down[size_t(key)];
    }
    bool KeyPressed(sdk::Key key) const override {
        return size_t(key) < size_t(sdk::Key::Count) && owner_.input_.pressed[size_t(key)];
    }
    void Log(std::string_view message) override {
        if (message.size() > 16384 || !validUtf8(message))
            throw std::runtime_error("Behavior log must be UTF-8 and at most 16 KiB");
        owner_.log_("[" + self_.string() + "] " + std::string(message));
    }

  private:
    EntityHandle handle(EntityId id) const {
        auto h = owner_.scene_.find(id);
        if (!h)
            throw std::runtime_error("Runtime entity does not exist: " + id.string());
        return h;
    }
    BehaviorRuntime& owner_;
    EntityId self_;
    sdk::PropertyMap properties_;
};
BehaviorRuntime::BehaviorRuntime(Scene& scene, const sdk::BehaviorRegistry& registry,
                                 std::function<void(std::string_view)> log)
    : scene_(scene), log_(std::move(log)) {
    const auto schema = describeRegistry(registry, "runtime");
    validateSceneBehaviors(scene, schema);
    scene_.update();
    // Snapshot sorting gives reproducible lifecycle order independent of dense-pool relocation.
    for (const auto& entity : scene.snapshot().entities) {
        const auto h = scene.find(entity.id);
        if (!scene.transform(h).visible)
            continue;
        for (const auto& binding : entity.behaviors) {
            if (!binding.enabled)
                continue;
            const auto* entry = registry.Find(binding.type);
            auto object = entry->factory();
            if (!object)
                throw std::runtime_error("Behavior factory returned null: " + location(entity.id, binding));
            auto properties = normalizeBehaviorProperties(entry->descriptor, binding.properties);
            instances_.push_back({entity.id, binding, std::move(object),
                                  std::make_unique<Context>(*this, entity.id, std::move(properties)), false});
        }
    }
}
BehaviorRuntime::~BehaviorRuntime() {
    try {
        stop();
    } catch (...) {
    }
}
void BehaviorRuntime::start() {
    if (active_)
        throw std::logic_error("Behavior runtime already started");
    active_ = true;
    for (auto& instance : instances_) {
        try {
            instance.object->OnStart(*instance.context);
            instance.started = true;
            ++metrics_.starts;
        } catch (const std::exception& e) {
            const auto message = "OnStart " + location(instance.entity, instance.binding) + ": " + e.what();
            try {
                stop();
            } catch (...) {
            }
            throw std::runtime_error(message);
        } catch (...) {
            try {
                stop();
            } catch (...) {
            }
            throw std::runtime_error("OnStart unknown exception: " + location(instance.entity, instance.binding));
        }
    }
    scene_.update();
}
void BehaviorRuntime::update(double dt, const RuntimeInput& input) {
    if (!active_)
        throw std::logic_error("Behavior runtime is not started");
    if (!std::isfinite(dt) || dt < 0 || dt > .25)
        throw std::runtime_error("Invalid runtime dt");
    input_ = input;
    const auto before = Clock::now();
    for (auto& instance : instances_) {
        try {
            instance.object->OnUpdate(*instance.context, dt);
            ++metrics_.updates;
        } catch (const std::exception& e) {
            throw std::runtime_error("OnUpdate " + location(instance.entity, instance.binding) + ": " + e.what());
        } catch (...) {
            throw std::runtime_error("OnUpdate unknown exception: " + location(instance.entity, instance.binding));
        }
    }
    scene_.update();
    metrics_.updateMs = milliseconds(before);
}
void BehaviorRuntime::stop() {
    if (!active_)
        return;
    active_ = false;
    std::string error;
    for (auto it = instances_.rbegin(); it != instances_.rend(); ++it) {
        if (!it->started)
            continue;
        it->started = false;
        ++metrics_.stops;
        try {
            it->object->OnStop(*it->context);
        } catch (const std::exception& e) {
            if (error.empty())
                error = "OnStop " + location(it->entity, it->binding) + ": " + e.what();
        } catch (...) {
            if (error.empty())
                error = "OnStop unknown exception: " + location(it->entity, it->binding);
        }
    }
    // OnStop may publish deferred transforms; make the final lifecycle state
    // observable even when one callback reports an error.
    scene_.update();
    if (!error.empty())
        throw std::runtime_error(error);
}
} // namespace proto
