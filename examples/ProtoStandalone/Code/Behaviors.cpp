#include <Proto/Behavior.hpp>
#include <cmath>
#include <string>

using namespace proto;
using namespace proto::sdk;

// Stable IDs identify serialized classes. Keep them when renaming a class.
const auto spinType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000001");
const auto moveLightType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000002");
const auto keyboardType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000003");
const auto readDataType = BehaviorTypeId::parse("60000000-0000-4000-8000-000000000004");
const auto standaloneRawId = AssetId::parse("9f21b5d0-7f62-4e93-a8b3-68d2c4f11022");

class Spin final : public Behavior {
    void OnUpdate(BehaviorContext& context, double dt) override {
        const double half = context.Property<double>("speedDegreesPerSecond") * dt * 3.141592653589793 / 360;
        const auto c = float(std::cos(half)), s = float(std::sin(half));
        auto transform = context.GetTransform(context.Self());
        const auto q = transform.rotation;
        transform.rotation = {c * q.w - s * q.y, c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x};
        context.SetTransform(context.Self(), transform);
    }
};

class MoveLight final : public Behavior {
    Vec3 origin_;
    double time_{};
    void OnStart(BehaviorContext& context) override {
        if (!context.GetPointLight(context.Self()))
            throw std::runtime_error("Attach MoveLight to a point light");
        origin_ = context.GetTransform(context.Self()).position;
    }
    void OnUpdate(BehaviorContext& context, double dt) override {
        time_ += dt;
        auto transform = context.GetTransform(context.Self());
        const auto radius = context.Property<double>("radius");
        const auto speed = context.Property<double>("speed");
        transform.position.x = origin_.x + float(std::sin(time_ * speed) * radius);
        transform.position.z = origin_.z + float((std::cos(time_ * speed) - 1) * radius);
        context.SetTransform(context.Self(), transform);
    }
};

class KeyboardMove final : public Behavior {
    void OnUpdate(BehaviorContext& context, double dt) override {
        auto transform = context.GetTransform(context.Self());
        const float distance = float(context.Property<double>("speed") * dt);
        if (context.KeyDown(Key::D) || context.KeyDown(Key::Right))
            transform.position.x += distance;
        if (context.KeyDown(Key::A) || context.KeyDown(Key::Left))
            transform.position.x -= distance;
        if (context.KeyDown(Key::W) || context.KeyDown(Key::Up))
            transform.position.z -= distance;
        if (context.KeyDown(Key::S) || context.KeyDown(Key::Down))
            transform.position.z += distance;
        if (context.KeyPressed(Key::Space)) {
            transform.position.y += .5f;
            context.Log("Space pressed: object moved up by 0.5 m");
        }
        context.SetTransform(context.Self(), transform);
    }
};

class ReadData final : public Behavior {
    void OnStart(BehaviorContext& context) override {
        const auto bytes = context.GetAssetBytes(standaloneRawId);
        std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
            content.pop_back();
        context.Log("ReadData loaded " + standaloneRawId.string() + ": " + content);
    }
};

void RegisterProjectBehaviors(BehaviorRegistry& registry) {
    registry.Add<Spin>(
        {spinType, "Spin", {{"speedDegreesPerSecond", PropertyType::Float, 30.0, -360.0, 360.0, "Обертання, град/с"}}});
    registry.Add<MoveLight>({moveLightType,
                             "MoveLight",
                             {{"radius", PropertyType::Float, 2.0, 0.0, 20.0, "Радіус руху, м"},
                              {"speed", PropertyType::Float, 1.0, 0.0, 10.0, "Швидкість, рад/с"}}});
    registry.Add<KeyboardMove>(
        {keyboardType, "KeyboardMove", {{"speed", PropertyType::Float, 2.0, 0.0, 30.0, "Швидкість, м/с"}}});
    registry.Add<ReadData>({readDataType, "ReadData", {}});
}
