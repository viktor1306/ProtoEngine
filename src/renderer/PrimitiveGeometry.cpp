#include "renderer/PrimitiveGeometry.hpp"

namespace proto {
PrimitiveGeometry makePrimitives() {
    PrimitiveGeometry result;
    auto face = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 color) {
        const auto base = static_cast<uint32_t>(result.vertices.size());
        for (const auto p : {a, b, c, d}) result.vertices.push_back({p, color});
        for (const auto i : {0u, 1u, 2u, 2u, 3u, 0u}) result.indices.push_back(base + i);
    };
    // Face tints are an unlit orientation aid, not lights or shadows.
    face({-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}, {0.80f,0.86f,0.91f});
    face({.5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{.5f,.5f,-.5f}, {0.65f,0.70f,0.78f});
    face({.5f,-.5f,.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f}, {0.60f,0.68f,0.77f});
    face({-.5f,-.5f,-.5f},{-.5f,-.5f,.5f},{-.5f,.5f,.5f},{-.5f,.5f,-.5f}, {0.72f,0.78f,0.86f});
    face({-.5f,.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f}, {1,1,1});
    face({-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{-.5f,-.5f,.5f}, {.45f,.5f,.6f});
    result.ranges[0] = {0, 36};
    result.ranges[1].firstIndex = static_cast<uint32_t>(result.indices.size());
    face({-.5f,0,.5f},{.5f,0,.5f},{.5f,0,-.5f},{-.5f,0,-.5f}, {1,1,1});
    result.ranges[1].indexCount = 6;
    const auto line = [&](glm::vec3 a, glm::vec3 b, glm::vec3 color) {
        const auto base = static_cast<uint32_t>(result.vertices.size());
        result.vertices.push_back({a, color}); result.vertices.push_back({b, color});
        result.indices.push_back(base); result.indices.push_back(base + 1);
    };
    result.ranges[2].firstIndex = static_cast<uint32_t>(result.indices.size());
    for (int i = -10; i <= 10; ++i) {
        const float p = static_cast<float>(i); const glm::vec3 gray(i % 5 == 0 ? .26f : .18f);
        line({p,.006f,-10}, {p,.006f,10}, i == 0 ? glm::vec3(.18f,.35f,.65f) : gray);
        line({-10,.006f,p}, {10,.006f,p}, i == 0 ? glm::vec3(.65f,.24f,.22f) : gray);
    }
    result.ranges[2].indexCount = static_cast<uint32_t>(result.indices.size()) - result.ranges[2].firstIndex;
    result.ranges[3].firstIndex = static_cast<uint32_t>(result.indices.size());
    for (int a : {-1, 1}) for (int b : {-1, 1}) {
        const float x = float(a) * .5f, y = float(b) * .5f;
        line({-.5f,x,y},{.5f,x,y},{1,1,1}); line({x,-.5f,y},{x,.5f,y},{1,1,1}); line({x,y,-.5f},{x,y,.5f},{1,1,1});
    }
    result.ranges[3].indexCount = 24; return result;
}
}
