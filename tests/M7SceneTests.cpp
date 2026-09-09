#include "benchmark/BenchmarkScenes.hpp"
#include "runtime/SceneViewBuilder.hpp"
#include "scene/SceneIO.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        auto empty = proto::makeBenchmarkScene("empty");
        check(empty.scene.meshes().empty() && empty.expectedPointLights == 0, "Empty fixture does work");
        auto visible = proto::makeBenchmarkScene("instances-10000-visible");
        auto culled = proto::makeBenchmarkScene("instances-10000-culled");
        proto::RenderView a, b;
        proto::buildRuntimeView(visible.scene, {1280,720}, a);
        proto::buildRuntimeView(culled.scene, {1280,720}, b);
        check(a.imported.size() >= 9000 && b.imported.size() < 200,
              "Visible/culled fixtures do not represent their declared workloads");
        check(a.casters.size() == b.casters.size() && a.casters.size() == 10004,
              "Culled fixture dropped potential shadow casters");
        auto overflow = proto::makeBenchmarkScene("pool-overflow");
        auto resident = proto::makeBenchmarkScene("pool-resident");
        check(overflow.contentHash == resident.contentHash, "Pool comparison changed scene content");
        check(overflow.graphics.pointShadows.poolBudgetMiB < resident.graphics.pointShadows.poolBudgetMiB,
              "Pool comparison has no budget difference");
        auto moving = proto::makeBenchmarkScene("instances-10000-moving");
        auto original = proto::encodeScene(moving.scene);
        moving.advance(60);
        moving.scene.update();
        check(proto::encodeScene(moving.scene) != original, "Moving parent fixture does not move");
        moving.advance(0);
        moving.scene.update();
        check(proto::encodeScene(moving.scene) == original, "Animation does not return to deterministic start");
        moving.advanceAt(3.37);
        moving.advanceAt(0);
        check(proto::encodeScene(moving.scene) == original, "Timed animation origin depends on warmup duration");
        moving.advanceAt(.001);
        check(proto::encodeScene(moving.scene) != original, "Continuous motion is quantized to a fixed frame rate");
        auto lights = proto::makeBenchmarkScene("lights-128");
        check(lights.scene.pointLights().size() == 128 && lights.expectedPointLights == 128,
              "Light fixture silently changed the requested count");
        check(lights.contentHash == proto::makeBenchmarkScene("lights-128").contentHash,
              "Benchmark content hash changes between identical scenes");
        std::cout << "M7 scene workloads, culling, deterministic animation and pool equivalence passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
