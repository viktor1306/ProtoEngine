#pragma once
#include "behavior/BehaviorSchema.hpp"
#include "scene/Scene.hpp"
#include <array>
#include <functional>

namespace proto {
struct RuntimeInput {
    std::array<bool, size_t(sdk::Key::Count)> down{}, pressed{};
};
struct BehaviorMetrics {
    uint64_t starts{}, updates{}, stops{};
    double updateMs{};
};
class BehaviorRuntime {
  public:
    BehaviorRuntime(Scene& scene, const sdk::BehaviorRegistry& registry, std::function<void(std::string_view)> log);
    ~BehaviorRuntime();
    void start();
    void update(double dt, const RuntimeInput& input);
    void stop();
    const BehaviorMetrics& metrics() const { return metrics_; }

  private:
    class Context;
    struct Instance {
        EntityId entity;
        sdk::BehaviorBinding binding;
        std::unique_ptr<sdk::Behavior> object;
        std::unique_ptr<Context> context;
        bool started{};
    };
    Scene& scene_;
    std::function<void(std::string_view)> log_;
    std::vector<Instance> instances_;
    RuntimeInput input_;
    BehaviorMetrics metrics_;
    bool active_{};
};
} // namespace proto
