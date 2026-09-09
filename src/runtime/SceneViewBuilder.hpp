#pragma once

#include "renderer/RenderView.hpp"
#include "scene/Scene.hpp"
#include <volk.h>

namespace proto {

// Builds the renderer-facing view used by an isolated Player. The builder
// deliberately contains no editor state: there is no grid, selection, probe,
// or editor-camera fallback involved in this path.
class SceneViewBuilder {
  public:
    static void buildRuntimeView(Scene& scene, VkExtent2D extent, RenderView& view);
    static void buildRuntimeView(Scene& scene, uint32_t width, uint32_t height, RenderView& view) {
        buildRuntimeView(scene, VkExtent2D{width, height}, view);
    }
};

inline void buildRuntimeView(Scene& scene, VkExtent2D extent, RenderView& view) {
    SceneViewBuilder::buildRuntimeView(scene, extent, view);
}

inline void buildRuntimeView(Scene& scene, uint32_t width, uint32_t height, RenderView& view) {
    SceneViewBuilder::buildRuntimeView(scene, width, height, view);
}

} // namespace proto
