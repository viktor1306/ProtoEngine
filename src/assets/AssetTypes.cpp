#include "assets/AssetTypes.hpp"
#include <cmath>
#include <stdexcept>
namespace proto {
void validateMaterial(const MaterialValues& v) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(v.baseColor[i]) || v.baseColor[i] < 0 || v.baseColor[i] > 1)
            throw std::runtime_error("Invalid base color");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v.emissive[i]) || v.emissive[i] < 0)
            throw std::runtime_error("Invalid emissive color");
    for (const float n : {v.metallic, v.roughness, v.occlusion, v.alphaCutoff})
        if (!std::isfinite(n) || n < 0 || n > 1)
            throw std::runtime_error("Material factor outside 0..1");
    if (!std::isfinite(v.normalScale) || v.normalScale < 0 || v.normalScale > 100)
        throw std::runtime_error("Invalid normal scale");
}
void AssetCatalog::publish(std::shared_ptr<const ModelBundle> model) {
    for (const auto& m : model->meshes) {
        auto it = meshes.find(m->id);
        if (it == meshes.end() || it->second->revision != m->revision)
            meshes[m->id] = m;
    }
    for (const auto& t : model->textures)
        textures[t->id] = t;
    for (const auto& m : model->materials)
        materials[m->id] = m;
    models[model->id] = std::move(model);
}
} // namespace proto
