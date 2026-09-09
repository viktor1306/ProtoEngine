#include "assets/AssetTypes.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace proto {
namespace {
Bounds triangleBounds(const MeshAsset& mesh, const MeshTriangle& triangle) {
    Bounds bounds{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
    for (uint32_t i = 0; i < 3; ++i) {
        const auto point = mesh.vertices[mesh.indices[triangle.firstIndex + i]].position;
        bounds.min = glm::min(bounds.min, point);
        bounds.max = glm::max(bounds.max, point);
    }
    return bounds;
}
bool intersects(const Bounds& bounds, glm::vec3 origin, glm::vec3 direction, float maximum) {
    float near = 0, far = maximum;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-12f) {
            if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis])
                return false;
        } else {
            float a = (bounds.min[axis] - origin[axis]) / direction[axis],
                  b = (bounds.max[axis] - origin[axis]) / direction[axis];
            if (a > b)
                std::swap(a, b);
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far)
                return false;
        }
    }
    return true;
}
float alpha(const TextureAsset& texture, glm::vec2 uv) {
    // Picking samples the authored mask at mip 0. Screen minification can soften its edge.
    const auto& image = texture.pixels->mips[0];
    const auto address = [](int index, int size, int mode) {
        if (mode == 33071)
            return std::clamp(index, 0, size - 1);
        const int period = mode == 33648 ? 2 * size : size;
        index = (index % period + period) % period;
        return index < size ? index : period - index - 1;
    };
    const auto tap = [&](int x, int y) {
        const auto at = (size_t(address(y, static_cast<int>(image.height), texture.wrapT)) * image.width +
                         size_t(address(x, static_cast<int>(image.width), texture.wrapS))) *
                            4 +
                        3;
        return float(image.rgba[at]) / 255;
    };
    // Reduce very large coordinates before converting to integers.
    for (int axis = 0; axis < 2; ++axis) {
        const int mode = axis ? texture.wrapT : texture.wrapS;
        uv[axis] = mode == 33071 ? std::clamp(uv[axis], 0.0f, 1.0f) : std::fmod(uv[axis], 2.0f);
    }
    const float x = uv.x * float(image.width) - .5f, y = uv.y * float(image.height) - .5f;
    if (texture.magFilter == 9728)
        return tap(static_cast<int>(std::floor(x + .5f)), static_cast<int>(std::floor(y + .5f)));
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    return glm::mix(glm::mix(tap(ix, iy), tap(ix + 1, iy), x - float(ix)),
                    glm::mix(tap(ix, iy + 1), tap(ix + 1, iy + 1), x - float(ix)), y - float(iy));
}
} // namespace
void buildMeshBvh(MeshAsset& mesh) {
    mesh.triangles.clear();
    mesh.bvh.clear();
    for (size_t part = 0; part < mesh.parts.size(); ++part) {
        const auto& p = mesh.parts[part];
        for (uint32_t i = p.firstIndex; i < p.firstIndex + p.indexCount; i += 3)
            mesh.triangles.push_back({i, static_cast<uint32_t>(part)});
    }
    std::function<uint32_t(uint32_t, uint32_t)> split = [&](uint32_t first, uint32_t count) {
        const auto index = static_cast<uint32_t>(mesh.bvh.size());
        mesh.bvh.push_back({});
        auto bounds = triangleBounds(mesh, mesh.triangles[first]);
        for (uint32_t i = first + 1; i < first + count; ++i) {
            const auto b = triangleBounds(mesh, mesh.triangles[i]);
            bounds.min = glm::min(bounds.min, b.min);
            bounds.max = glm::max(bounds.max, b.max);
        }
        mesh.bvh[index] = {bounds, first, count, 0, 0};
        if (count > 8) {
            const auto extent = bounds.max - bounds.min;
            const int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
            const auto middle = first + count / 2;
            std::nth_element(mesh.triangles.begin() + first, mesh.triangles.begin() + middle,
                             mesh.triangles.begin() + first + count, [&](const auto& a, const auto& b) {
                                 const auto aa = triangleBounds(mesh, a), bb = triangleBounds(mesh, b);
                                 return aa.min[axis] + aa.max[axis] < bb.min[axis] + bb.max[axis];
                             });
            const auto left = split(first, count / 2), right = split(middle, count - count / 2);
            mesh.bvh[index].count = 0;
            mesh.bvh[index].left = left;
            mesh.bvh[index].right = right;
        }
        return index;
    };
    if (!mesh.triangles.empty())
        split(0, static_cast<uint32_t>(mesh.triangles.size()));
}
std::optional<float> pickMesh(const MeshAsset& mesh, const AssetCatalog& assets, const std::vector<AssetId>& materials,
                              glm::vec3 origin, glm::vec3 direction, float maxDistance) {
    if (mesh.bvh.empty())
        return {};
    std::optional<float> result;
    std::vector<uint32_t> stack{0};
    while (!stack.empty()) {
        const auto node = mesh.bvh[stack.back()];
        stack.pop_back();
        if (!intersects(node.bounds, origin, direction, result.value_or(maxDistance)))
            continue;
        if (!node.count) {
            stack.push_back(node.left);
            stack.push_back(node.right);
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            const auto triangle = mesh.triangles[i];
            const auto& a = mesh.vertices[mesh.indices[triangle.firstIndex]];
            const auto& b = mesh.vertices[mesh.indices[triangle.firstIndex + 1]];
            const auto& c = mesh.vertices[mesh.indices[triangle.firstIndex + 2]];
            const auto& material = *assets.materials.at(materials.empty() ? mesh.parts[triangle.part].material
                                                                          : materials.at(triangle.part));
            const auto e1 = b.position - a.position, e2 = c.position - a.position, p = glm::cross(direction, e2);
            const float determinant = glm::dot(e1, p);
            if ((!material.values.doubleSided && determinant <= 1e-8f) || std::abs(determinant) < 1e-8f)
                continue;
            const auto delta = origin - a.position, q = glm::cross(delta, e1);
            const float u = glm::dot(delta, p) / determinant, v = glm::dot(direction, q) / determinant,
                        distance = glm::dot(e2, q) / determinant;
            if (u < 0 || v < 0 || u + v > 1 || distance < 0 || distance >= result.value_or(maxDistance))
                continue;
            if (material.values.mask) {
                const auto& slot = material.textures[0];
                float opacity = material.values.baseColor.a * (a.color.a * (1 - u - v) + b.color.a * u + c.color.a * v);
                if (slot.texture) {
                    auto uv = slot.texCoord ? a.uv1 * (1 - u - v) + b.uv1 * u + c.uv1 * v
                                            : a.uv0 * (1 - u - v) + b.uv0 * u + c.uv0 * v;
                    uv *= slot.scale;
                    const float cosine = std::cos(slot.rotation), sine = std::sin(slot.rotation);
                    uv = glm::vec2(cosine * uv.x - sine * uv.y, sine * uv.x + cosine * uv.y) + slot.offset;
                    opacity *= alpha(*assets.textures.at(slot.texture), uv);
                }
                if (opacity < material.values.alphaCutoff)
                    continue;
            }
            result = distance;
        }
    }
    return result;
}
} // namespace proto
