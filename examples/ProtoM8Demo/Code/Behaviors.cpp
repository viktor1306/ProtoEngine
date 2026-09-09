#include <Proto/Behavior.hpp>
#include <algorithm>
#include <cmath>

using namespace proto;
using namespace proto::sdk;

// Stable IDs identify serialized classes. Keep them when renaming a class.
const auto spinType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000001");
const auto moveLightType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000002");
const auto keyboardType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000003");

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
        if (!context.Property<bool>("spaceOnly")) {
            if (context.KeyDown(Key::D) || context.KeyDown(Key::Right))
                transform.position.x += distance;
            if (context.KeyDown(Key::A) || context.KeyDown(Key::Left))
                transform.position.x -= distance;
            if (context.KeyDown(Key::W) || context.KeyDown(Key::Up))
                transform.position.z -= distance;
            if (context.KeyDown(Key::S) || context.KeyDown(Key::Down))
                transform.position.z += distance;
        }
        if (context.KeyPressed(Key::Space)) {
            transform.position.y += .5f;
            context.Log("Space pressed: object moved up by 0.5 m");
        }
        context.SetTransform(context.Self(), transform);
    }
};


namespace {
sdk::Vec3 add(sdk::Vec3 a, sdk::Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
sdk::Vec3 scale(sdk::Vec3 a, float value) { return {a.x * value, a.y * value, a.z * value}; }
float length(sdk::Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
sdk::Vec3 normalize(sdk::Vec3 value) {
    const auto n = length(value);
    return n > .00001f ? scale(value, 1.0f / n) : sdk::Vec3{0, 0, -1};
}
sdk::Quat normalize(sdk::Quat value) {
    const auto n = std::sqrt(value.w * value.w + value.x * value.x + value.y * value.y + value.z * value.z);
    return n > .00001f ? sdk::Quat{value.w / n, value.x / n, value.y / n, value.z / n} : sdk::Quat{};
}
sdk::Quat multiply(sdk::Quat a, sdk::Quat b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
sdk::Quat axisAngle(sdk::Vec3 axis, float angle) {
    axis = normalize(axis);
    const auto half = angle * .5f;
    const auto s = std::sin(half);
    return normalize({std::cos(half), axis.x * s, axis.y * s, axis.z * s});
}
sdk::Vec3 rotate(sdk::Quat q, sdk::Vec3 value) {
    const sdk::Quat v{0, value.x, value.y, value.z};
    const sdk::Quat inverse{q.w, -q.x, -q.y, -q.z};
    const auto result = multiply(multiply(q, v), inverse);
    return {result.x, result.y, result.z};
}
} // namespace

class FreeCamera final : public Behavior {
    double pitch_{};
    void OnStart(BehaviorContext& context) override {
        const auto transform = context.GetTransform(context.Self());
        const auto forward = normalize(rotate(transform.rotation, {0, 0, -1}));
        pitch_ = std::asin(std::clamp(static_cast<double>(forward.y), -1.0, 1.0));
    }
    void OnUpdate(BehaviorContext& context, double dt) override {
        auto transform = context.GetTransform(context.Self());
        float speed = static_cast<float>(context.Property<double>("speed"));
        if (context.KeyDown(Key::Shift))
            speed *= 3.0f;
        const auto forward = normalize(rotate(transform.rotation, {0, 0, -1}));
        const auto right = normalize(rotate(transform.rotation, {1, 0, 0}));
        const auto up = sdk::Vec3{0, 1, 0};
        sdk::Vec3 movement{};
        if (context.KeyDown(Key::W))
            movement = add(movement, forward);
        if (context.KeyDown(Key::S))
            movement = add(movement, scale(forward, -1));
        if (context.KeyDown(Key::D))
            movement = add(movement, right);
        if (context.KeyDown(Key::A))
            movement = add(movement, scale(right, -1));
        if (context.KeyDown(Key::E))
            movement = add(movement, up);
        if (context.KeyDown(Key::Q))
            movement = add(movement, scale(up, -1));
        const auto distance = speed * static_cast<float>(dt);
        if (length(movement) > .00001f)
            transform.position = add(transform.position, scale(normalize(movement), distance));

        const auto lookSpeed = static_cast<float>(context.Property<double>("lookSpeed") * dt * 3.141592653589793 / 180.0);
        const float yaw = (context.KeyDown(Key::Left) ? 1.0f : 0.0f) * lookSpeed -
                          (context.KeyDown(Key::Right) ? 1.0f : 0.0f) * lookSpeed;
        const float pitch = (context.KeyDown(Key::Up) ? 1.0f : 0.0f) * lookSpeed -
                            (context.KeyDown(Key::Down) ? 1.0f : 0.0f) * lookSpeed;
        if (std::abs(yaw) > .00001f)
            transform.rotation = normalize(multiply(axisAngle({0, 1, 0}, yaw), transform.rotation));
        if (std::abs(pitch) > .00001f) {
            const auto nextPitch = std::max(-1.45, std::min(1.45, pitch_ + static_cast<double>(pitch)));
            const auto pitchDelta = static_cast<float>(nextPitch - pitch_);
            pitch_ = nextPitch;
            if (std::abs(pitchDelta) > .00001f)
                transform.rotation = normalize(multiply(transform.rotation, axisAngle({1, 0, 0}, pitchDelta)));
        }
        context.SetTransform(context.Self(), transform);
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
        {keyboardType, "KeyboardMove", {{"speed", PropertyType::Float, 2.0, 0.0, 30.0, "Швидкість, м/с"},
                                          {"spaceOnly", PropertyType::Bool, false, {}, {}, "Лише Space"}}});
    registry.Add<FreeCamera>({BehaviorTypeId::parse("50000000-0000-4000-8000-000000000004"),
                               "FreeCamera",
                               {{"speed", PropertyType::Float, 4.0, 0.0, 30.0, "Швидкість камери, м/с"},
                                {"lookSpeed", PropertyType::Float, 90.0, 0.0, 360.0, "Огляд, град/с"}}});

}
