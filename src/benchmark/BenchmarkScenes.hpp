#pragma once
#include "scene/Scene.hpp"
#include "runtime/PlayerGraphics.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace proto {
struct BenchmarkScene {
    Scene scene;
    PlayerGraphics graphics;
    std::string scenario;
    std::string contentHash;
    std::vector<EntityId> moving;
    std::vector<Transform> originalTransforms;
    enum class Motion { None, Lights, Casters, Parent } motion{Motion::None};
    size_t expectedMeshes{}, expectedPointLights{};
    void advance(uint64_t frame);
    void advanceAt(double seconds);
};
std::vector<std::string> benchmarkScenarioNames();
BenchmarkScene makeBenchmarkScene(std::string_view scenario);
std::string benchmarkSceneManifest(const BenchmarkScene& scene);
}
