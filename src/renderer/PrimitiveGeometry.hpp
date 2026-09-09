#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace proto {
struct PrimitiveVertex { glm::vec3 position, color; };
struct MeshRange { uint32_t firstIndex{}, indexCount{}; };
struct PrimitiveGeometry {
    std::vector<PrimitiveVertex> vertices;
    std::vector<uint32_t> indices;
    std::array<MeshRange, 4> ranges; // cube, plane, editor grid, selection wire box
};
PrimitiveGeometry makePrimitives();
}
