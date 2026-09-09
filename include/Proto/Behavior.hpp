#pragma once
#include <Proto/Id.hpp>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace proto::sdk {
inline constexpr unsigned behaviorApiVersion = 1;
struct Vec3 {
    float x{}, y{}, z{};
    bool operator==(const Vec3&) const = default;
};
struct Quat {
    float w{1}, x{}, y{}, z{};
    bool operator==(const Quat&) const = default;
};
struct Color {
    float r{1}, g{1}, b{1}, a{1};
    bool operator==(const Color&) const = default;
};
struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1, 1, 1};
    bool operator==(const Transform&) const = default;
};
struct EntityRef {
    EntityId id;
    bool operator==(const EntityRef&) const = default;
};
struct AssetRef {
    AssetId id;
    bool operator==(const AssetRef&) const = default;
};
using PropertyValue = std::variant<bool, int64_t, double, std::string, Vec3, Color, EntityRef, AssetRef>;
using PropertyMap = std::map<std::string, PropertyValue, std::less<>>;
enum class PropertyType { Bool, Integer, Float, String, Vec3, Color, EntityRef, AssetRef };
struct PropertyDescriptor {
    std::string name;
    PropertyType type{PropertyType::Float};
    PropertyValue defaultValue{0.0};
    std::optional<double> min, max;
    std::string label;
    bool operator==(const PropertyDescriptor&) const = default;
};
struct BehaviorDescriptor {
    BehaviorTypeId id;
    std::string name;
    std::vector<PropertyDescriptor> properties;
    bool operator==(const BehaviorDescriptor&) const = default;
};
struct BehaviorBinding {
    BehaviorBindingId id;
    BehaviorTypeId type;
    bool enabled{true};
    PropertyMap properties;
    bool operator==(const BehaviorBinding&) const = default;
};
struct PointLight {
    Vec3 color{1, 1, 1};
    float intensity{30}, radius{8};
    bool shadows{true};
};
struct DirectionalLight {
    Vec3 color{1, 1, 1};
    float intensity{3};
    bool shadows{true};
};
enum class Key { W, A, S, D, Q, E, Up, Down, Left, Right, Space, Shift, Escape, Count };

// A context belongs to one instance. IDs remain stable; component values are copies.
class BehaviorContext {
  public:
    virtual ~BehaviorContext() = default;
    virtual EntityId Self() const = 0;
    virtual const PropertyMap& Properties() const = 0;
    template <class T> const T& Property(std::string_view name) const {
        const auto it = Properties().find(name);
        if (it == Properties().end())
            throw std::runtime_error("Unknown behavior property: " + std::string(name));
        return std::get<T>(it->second);
    }
    virtual bool Exists(EntityId entity) const = 0;
    virtual Transform GetTransform(EntityId entity) const = 0;
    virtual void SetTransform(EntityId entity, const Transform& value) = 0;
    virtual Vec3 GetWorldPosition(EntityId entity) const = 0;
    // Immutable raw file data prepared before OnStart. The view remains valid
    // for this Player session; this does not perform disk I/O in the callback.
    virtual std::span<const uint8_t> GetAssetBytes(AssetId asset) const = 0;
    virtual std::optional<PointLight> GetPointLight(EntityId entity) const = 0;
    virtual void SetPointLight(EntityId entity, const PointLight& value) = 0;
    virtual std::optional<DirectionalLight> GetDirectionalLight(EntityId entity) const = 0;
    virtual void SetDirectionalLight(EntityId entity, const DirectionalLight& value) = 0;
    virtual bool KeyDown(Key key) const = 0;
    virtual bool KeyPressed(Key key) const = 0;
    virtual void Log(std::string_view message) = 0;
};
class Behavior {
  public:
    virtual ~Behavior() = default;
    virtual void OnStart(BehaviorContext&) {}
    virtual void OnUpdate(BehaviorContext&, double) {}
    virtual void OnStop(BehaviorContext&) {}
};
class BehaviorRegistry {
  public:
    using Factory = std::unique_ptr<Behavior> (*)();
    struct Entry {
        BehaviorDescriptor descriptor;
        Factory factory{};
    };
    void Add(BehaviorDescriptor descriptor, Factory factory);
    template <class T> void Add(BehaviorDescriptor descriptor) {
        Add(std::move(descriptor), +[]() -> std::unique_ptr<Behavior> { return std::make_unique<T>(); });
    }
    const Entry* Find(BehaviorTypeId id) const;
    const std::vector<Entry>& Entries() const { return entries_; }

  private:
    std::vector<Entry> entries_;
};
} // namespace proto::sdk

// Implement this function once in Code/*.cpp. Registration must only declare types.
void RegisterProjectBehaviors(proto::sdk::BehaviorRegistry& registry);
