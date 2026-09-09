#include "editor/LightingSmoke.hpp"

#include "editor/Editor.hpp"
#include "editor/LightingOracle.hpp"
#include "renderer/AssetRenderer.hpp"
#include "renderer/VulkanRenderer.hpp"

#include "core/Diagnostics.hpp"
#include "editor/EditorCamera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace proto {
namespace {

constexpr glm::vec3 groundAlbedo{.5f};
constexpr float probeTolerance = 3.0f;
constexpr uint32_t minimumProbes = 8;
constexpr uint64_t noFrame = std::numeric_limits<uint64_t>::max();

constexpr std::array<LightingSmokeState::Phase, 18> phaseOrder{{
    LightingSmokeState::Phase::StaticWarm,
    LightingSmokeState::Phase::ColorIntensity,
    LightingSmokeState::Phase::MovingLight,
    LightingSmokeState::Phase::MovingCaster,
    LightingSmokeState::Phase::SunMask,
    LightingSmokeState::Phase::PointMask,
    LightingSmokeState::Phase::OffscreenCaster,
    LightingSmokeState::Phase::RadiusChange,
    LightingSmokeState::Phase::GeometryRevision,
    LightingSmokeState::Phase::MaskMaterial,
    LightingSmokeState::Phase::MaskSampler,
    LightingSmokeState::Phase::NativeMeshReplacement,
    LightingSmokeState::Phase::ShadowToggles,
    LightingSmokeState::Phase::ReferenceLighting,
    LightingSmokeState::Phase::CacheRefresh,
    LightingSmokeState::Phase::TinyPool,
    LightingSmokeState::Phase::Overflow,
    LightingSmokeState::Phase::MotionSequence,
}};

EntityId entityId(const char* value) {
    return EntityId::parse(value);
}

EntityRecord record(const SceneDocument& document, EntityId id) {
    const auto handle = document.scene.find(id);
    if (!handle)
        throw std::runtime_error("Lighting smoke entity disappeared: " + id.string());
    return document.scene.record(handle);
}

template <class Fn> void editEntity(Editor& editor, EntityId id, Fn&& edit) {
    auto value = record(editor.document(), id);
    edit(value);
    editor.document().edit(std::move(value), "M3 lighting smoke");
}

glm::quat downwardLightRotation() {
    return glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1, 0, 0));
}

void setLighting(Editor& editor, const LightingSettings& value) {
    editor.document().lighting(value);
}

void setMaskCutoff(Scene& scene, AssetId id, float cutoff) {
    const auto found = scene.assets->materials.find(id);
    if (found == scene.assets->materials.end() || !found->second)
        throw std::runtime_error("Lighting smoke MASK material disappeared");
    auto next = std::make_shared<MaterialAsset>(*found->second);
    next->values.alphaCutoff = cutoff;
    ++next->revision;
    scene.assets->materials[id] = std::move(next);
    scene.assetsChanged();
}

void replaceMaskGeometry(Scene& scene, const LightingSmokeState& state) {
    const auto found = scene.assets->meshes.find(state.maskMesh);
    if (found == scene.assets->meshes.end() || !found->second)
        throw std::runtime_error("Lighting smoke MASK mesh disappeared");
    auto next = std::make_shared<MeshAsset>(*found->second);
    next->revision += "-lighting-geometry";
    // Keep the same asset identity and bounds while changing the ray/cached
    // geometry. The small Z displacement changes both CPU and GPU shadow
    // intersections without moving the entity.
    for (auto& vertex : next->vertices)
        vertex.position.z += 0.06f;
    buildMeshBvh(*next);
    scene.assets->meshes[state.maskMesh] = next;
    if (const auto model = scene.assets->models.find(state.modelId);
        model != scene.assets->models.end() && model->second) {
        auto changed = std::make_shared<ModelBundle>(*model->second);
        for (auto& mesh : changed->meshes)
            if (mesh && mesh->id == state.maskMesh)
                mesh = next;
        scene.assets->models[state.modelId] = std::move(changed);
    }
    scene.assetsChanged();
}
void changeMaskSampler(Scene& scene, const LightingSmokeState& state, bool clamp) {
    auto material = std::make_shared<MaterialAsset>(*scene.assets->materials.at(state.maskMaterial));
    material->textures[0].scale.x = 2;
    material->textures[0].offset.x = -.25f;
    ++material->revision;
    auto texture = std::make_shared<TextureAsset>(*scene.assets->textures.at(material->textures[0].texture));
    texture->wrapS = clamp ? 33071 : 10497;
    scene.assets->textures[texture->id] = texture;
    scene.assets->materials[material->id] = material;
    scene.assetsChanged();
}

LightingSettings baseLighting() {
    LightingSettings settings;
    settings.filteredShadows = false;
    settings.referenceLighting = false;
    settings.forceShadowRefresh = false;
    settings.shadowResolution = 512;
    settings.shadowCascades = 3;
    settings.shadowPoolMiB = 64;
    settings.shadowDistance = 40;
    settings.depthBias = .001f;
    settings.normalBias = .015f;
    settings.ambient = .025f;
    settings.shadows = true;
    return settings;
}

void ensureFixtureEntities(Editor& editor, const LightingSmokeState& state) {
    for (const auto id : {state.groundId, state.cubeId, state.maskId, state.offscreenId, state.sunId, state.pointId,
                          state.parentAId, state.parentBId})
        if (!editor.document().scene.find(id))
            throw std::runtime_error("Lighting smoke fixture entity missing: " + id.string());
}

double luminance(glm::vec3 value) {
    return .2126 * static_cast<double>(value.r) + .7152 * static_cast<double>(value.g) +
           .0722 * static_cast<double>(value.b);
}

LightingSmokeState::Phase phaseAt(size_t index) {
    return index < phaseOrder.size() ? phaseOrder[index] : LightingSmokeState::Phase::Complete;
}
uint32_t captureCount(LightingSmokeState::Phase phase) {
    using Phase = LightingSmokeState::Phase;
    switch (phase) {
    case Phase::ColorIntensity:
    case Phase::ShadowToggles:
    case Phase::TinyPool:
        return 4;
    case Phase::StaticWarm:
    case Phase::ReferenceLighting:
    case Phase::CacheRefresh:
    case Phase::Overflow:
    case Phase::GeometryRevision:
    case Phase::MaskMaterial:
        return 2;
    case Phase::NativeMeshReplacement:
    case Phase::MaskSampler:
        return 3;
    case Phase::MotionSequence:
        return 48;
    default:
        return 1;
    }
}
bool comparesImages(LightingSmokeState::Phase phase) {
    using Phase = LightingSmokeState::Phase;
    return phase == Phase::ReferenceLighting || phase == Phase::CacheRefresh || phase == Phase::TinyPool ||
           phase == Phase::Overflow || phase == Phase::NativeMeshReplacement || phase == Phase::MaskSampler ||
           phase == Phase::MotionSequence;
}
uint32_t comparisonStart(LightingSmokeState::Phase phase, uint32_t index) {
    if (phase == LightingSmokeState::Phase::MotionSequence || phase == LightingSmokeState::Phase::TinyPool)
        return index - index % 2;
    return phase == LightingSmokeState::Phase::NativeMeshReplacement || phase == LightingSmokeState::Phase::MaskSampler
               ? 1u
               : 0u;
}

bool phaseNeedsContrast(LightingSmokeState::Phase phase) {
    return phase == LightingSmokeState::Phase::SunMask || phase == LightingSmokeState::Phase::PointMask ||
           phase == LightingSmokeState::Phase::MovingCaster || phase == LightingSmokeState::Phase::MovingLight ||
           phase == LightingSmokeState::Phase::OffscreenCaster || phase == LightingSmokeState::Phase::RadiusChange;
}

bool phaseNeedsShadowMiss(LightingSmokeState::Phase phase) {
    return phase == LightingSmokeState::Phase::MovingLight || phase == LightingSmokeState::Phase::MovingCaster ||
           phase == LightingSmokeState::Phase::RadiusChange || phase == LightingSmokeState::Phase::GeometryRevision ||
           phase == LightingSmokeState::Phase::MaskMaterial || phase == LightingSmokeState::Phase::MaskSampler ||
           phase == LightingSmokeState::Phase::NativeMeshReplacement ||
           phase == LightingSmokeState::Phase::OffscreenCaster;
}

void writeManifest(const LightingSmokeState& state) {
    ensureParent(state.manifestPath);
    std::ofstream out(state.manifestPath, std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot write M3 lighting smoke manifest");
    out << std::boolalpha << "{\n"
        << "  \"passed\": " << state.passed << ",\n"
        << "  \"finished\": " << state.finished << ",\n"
        << "  \"scene_reopened\": " << state.sceneReopened << ",\n"
        << "  \"phase_count\": " << state.phases.size() << ",\n"
        << "  \"source\": " << jsonString(utf8(state.sourcePath.wstring())) << ",\n"
        << "  \"scene\": " << jsonString(utf8(state.scenePath.wstring())) << ",\n"
        << "  \"phases\": [\n";
    for (size_t i = 0; i < state.phases.size(); ++i) {
        const auto& phase = state.phases[i];
        out << "    {\"name\": " << jsonString(phase.name) << ", \"start_frame\": " << phase.startFrame
            << ", \"capture_frame\": " << phase.captureFrame << ", \"capture_index\": " << phase.captureIndex
            << ", \"captures_expected\": " << phase.capturesExpected << ", \"probe_count\": " << phase.probeCount
            << ", \"probe_passes\": " << phase.probePasses << ", \"max_error\": " << std::setprecision(8)
            << phase.maxError << ", \"expected_contrast\": " << phase.expectedContrast
            << ", \"expected_shadow_contrast\": " << phase.expectedShadowContrast
            << ", \"full_image_compared\": " << phase.fullImageCompared
            << ", \"full_image_max_difference\": " << phase.fullImageMaxDifference
            << ", \"full_image_mean_difference\": " << phase.fullImageMeanDifference
            << ", \"shadow_cache_hits\": " << phase.shadowCacheHits << ", \"shadow_misses\": " << phase.shadowMisses
            << ", \"shadow_faces\": " << phase.shadowFaces << ", \"shadow_slots\": " << phase.shadowSlots
            << ", \"batches\": " << phase.batches << ", \"tile_overflows\": " << phase.tileOverflows
            << ", \"shadow_bytes\": " << phase.shadowBytes << ", \"passed\": " << phase.passed
            << ", \"peak_shadow_misses\": " << phase.peakShadowMisses
            << ", \"peak_shadow_faces\": " << phase.peakShadowFaces << "}"
            << (i + 1 == state.phases.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"failure\": " << jsonString(state.failure) << "\n}\n";
}

void configureBaseFixture(Editor& editor, const LightingSmokeState& state) {
    ensureFixtureEntities(editor, state);
    editor.document().scene.assets->publish(state.model);
    editor.document().scene.assetsChanged();
    setLighting(editor, baseLighting());

    editEntity(editor, state.parentAId, [](EntityRecord& value) {
        value.enabled = true;
        value.transform.position = {0, 0, 0};
        value.transform.rotation = glm::quat(1, 0, 0, 0);
        value.transform.scale = {1, 1, 1};
    });
    editEntity(editor, state.parentBId, [](EntityRecord& value) {
        value.enabled = true;
        value.transform.position = {0, 0, 0};
        value.transform.rotation = glm::quat(1, 0, 0, 0);
        value.transform.scale = {1, 1, 1};
    });
    editEntity(editor, state.groundId, [](EntityRecord& value) {
        value.enabled = true;
        value.transform.position = {0, 0, 0};
        value.transform.rotation = glm::quat(1, 0, 0, 0);
        value.transform.scale = {10, 1, 10};
        value.mesh->castShadows = false;
        value.mesh->receiveShadows = true;
    });
    editEntity(editor, state.cubeId, [&](EntityRecord& value) {
        value.enabled = true;
        value.parent = state.parentAId;
        value.transform.position = {0, 1.0f, 0};
        value.transform.rotation = glm::angleAxis(glm::radians(22.5f), glm::vec3(0, 1, 0));
        value.transform.scale = {1.4f, 2.0f, 1.2f};
        value.mesh->mesh = builtin::cube;
        value.mesh->castShadows = true;
        value.mesh->receiveShadows = true;
    });
    editEntity(editor, state.maskId, [&](EntityRecord& value) {
        value.enabled = true;
        value.parent = state.parentBId;
        value.transform.position = {-1.8f, 2.2f, 0};
        value.transform.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0));
        value.transform.scale = {1.55f, 1.55f, 1.0f};
        value.mesh->mesh = state.maskMesh;
        value.mesh->castShadows = true;
        value.mesh->receiveShadows = false;
    });
    editEntity(editor, state.offscreenId, [&](EntityRecord& value) {
        value.enabled = false;
        value.parent = {};
        value.transform.position = {30, 1, 30};
        value.transform.rotation = glm::quat(1, 0, 0, 0);
        value.transform.scale = {1.2f, 1.2f, 1.2f};
        value.mesh->mesh = builtin::cube;
        value.mesh->castShadows = true;
    });
    editEntity(editor, state.sunId, [&](EntityRecord& value) {
        value.enabled = true;
        value.transform.position = {0, 8, 0};
        value.transform.rotation = downwardLightRotation();
        value.directionalLight->intensity = 3.0f;
        value.directionalLight->color = {1.0f, .95f, .85f};
        value.directionalLight->shadows = true;
    });
    editEntity(editor, state.pointId, [&](EntityRecord& value) {
        value.enabled = true;
        value.parent = {};
        value.transform.position = {3.0f, 4.5f, 3.0f};
        value.transform.rotation = glm::quat(1, 0, 0, 0);
        value.pointLight->intensity = 28.0f;
        value.pointLight->color = {1.0f, .72f, .48f};
        value.pointLight->radius = 12.0f;
        value.pointLight->shadows = true;
    });
    for (const auto id : state.lampIds)
        editEntity(editor, id, [](EntityRecord& value) { value.enabled = false; });
    setMaskCutoff(editor.document().scene, state.maskMaterial, .5f);
}

void applyPhase(Editor& editor, LightingSmokeState& state) {
    const auto phase = state.phase;
    state.settleFrames = phase == LightingSmokeState::Phase::MotionSequence ? 1 : 6;
    state.capturesRemaining = captureCount(phase);
    state.motionSamplesRemaining = phase == LightingSmokeState::Phase::MotionSequence ? 24 : 0;
    if (phase == LightingSmokeState::Phase::Complete) {
        state.finished = true;
        state.passed = state.failure.empty();
        return;
    }
    if (phase == LightingSmokeState::Phase::StaticWarm)
        return;

    if (phase == LightingSmokeState::Phase::ColorIntensity) {
        editEntity(editor, state.pointId, [](EntityRecord& value) {
            value.pointLight->color = {1.0f, .25f, .12f};
            value.pointLight->intensity = 46.0f;
        });
    } else if (phase == LightingSmokeState::Phase::MovingLight) {
        configureBaseFixture(editor, state);
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.transform.position = {-3.0f, 4.0f, 2.0f}; });
    } else if (phase == LightingSmokeState::Phase::MovingCaster) {
        editor.document().reparent(state.cubeId, state.parentBId, false);
        editEntity(editor, state.parentBId, [](EntityRecord& value) {
            value.transform.position = {.85f, 0, -.7f};
            value.transform.rotation = glm::angleAxis(glm::radians(-18.0f), glm::vec3(0, 1, 0));
        });
    } else if (phase == LightingSmokeState::Phase::SunMask) {
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.pointLight->intensity = 0.0f; });
        editEntity(editor, state.sunId, [](EntityRecord& value) { value.directionalLight->intensity = 4.0f; });
        editEntity(editor, state.maskId, [](EntityRecord& value) {
            value.enabled = true;
            value.transform.position = {0, 2.0f, 0};
            value.transform.scale = {1.8f, 1.8f, 1.0f};
        });
    } else if (phase == LightingSmokeState::Phase::PointMask) {
        editEntity(editor, state.sunId, [](EntityRecord& value) { value.directionalLight->intensity = 0.0f; });
        editEntity(editor, state.pointId, [](EntityRecord& value) {
            value.pointLight->intensity = 44.0f;
            value.pointLight->color = {.4f, .72f, 1.0f};
            value.pointLight->radius = 12.0f;
            value.transform.position = {2.8f, 4.2f, 2.8f};
        });
        editEntity(editor, state.maskId, [](EntityRecord& value) { value.transform.position = {0, 1.9f, 0}; });
    } else if (phase == LightingSmokeState::Phase::OffscreenCaster) {
        editEntity(editor, state.maskId, [](EntityRecord& value) { value.enabled = false; });
        editEntity(editor, state.parentBId, [](EntityRecord& value) {
            value.transform.position = {0, 0, 0};
            value.transform.rotation = glm::quat(1, 0, 0, 0);
        });
        editEntity(editor, state.offscreenId, [](EntityRecord& value) {
            value.enabled = true;
            value.transform.position = {-9.0f, 4.5f, 0};
            value.transform.scale = {2.0f, 2.0f, 2.0f};
        });
        const glm::vec3 lightRay = glm::normalize(glm::vec3(2, -1, 0));
        editEntity(editor, state.sunId, [lightRay](EntityRecord& value) {
            value.directionalLight->intensity = 4.0f;
            value.transform.rotation = glm::quatLookAtRH(lightRay, glm::vec3(0, 1, 0));
        });
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.pointLight->intensity = 0.0f; });
    } else if (phase == LightingSmokeState::Phase::RadiusChange) {
        configureBaseFixture(editor, state);
        editEntity(editor, state.sunId, [](auto& value) { value.directionalLight->intensity = 0; });
        editEntity(editor, state.offscreenId, [](EntityRecord& value) { value.enabled = false; });
        editEntity(editor, state.cubeId, [](EntityRecord& value) { value.enabled = true; });
        auto settings = editor.document().scene.lighting;
        settings.depthBias = 0.0f;
        settings.normalBias = .02f;
        setLighting(editor, settings);
        editEntity(editor, state.pointId, [](EntityRecord& value) {
            value.pointLight->intensity = 1200.0f;
            value.pointLight->radius = 10000.0f;
            value.transform.position = {2.5f, 4.0f, 2.5f};
        });
    } else if (phase == LightingSmokeState::Phase::GeometryRevision) {
        configureBaseFixture(editor, state);
    } else if (phase == LightingSmokeState::Phase::MaskMaterial) {
        // First capture the existing cutout, then change only its alpha cutoff.
    } else if (phase == LightingSmokeState::Phase::MaskSampler) {
        configureBaseFixture(editor, state);
        changeMaskSampler(editor.document().scene, state, false);
    } else if (phase == LightingSmokeState::Phase::NativeMeshReplacement) {
        configureBaseFixture(editor, state);
    } else if (phase == LightingSmokeState::Phase::ShadowToggles) {
        configureBaseFixture(editor, state);
        editEntity(editor, state.cubeId, [](EntityRecord& value) { value.mesh->castShadows = false; });
    } else if (phase == LightingSmokeState::Phase::ReferenceLighting) {
        configureBaseFixture(editor, state);
        auto settings = editor.document().scene.lighting;
        settings.referenceLighting = false;
        settings.forceShadowRefresh = false;
        setLighting(editor, settings);
    } else if (phase == LightingSmokeState::Phase::CacheRefresh) {
        auto settings = editor.document().scene.lighting;
        settings.referenceLighting = false;
        settings.forceShadowRefresh = false;
        setLighting(editor, settings);
    } else if (phase == LightingSmokeState::Phase::TinyPool) {
        configureBaseFixture(editor, state);
        auto settings = editor.document().scene.lighting;
        settings.shadowResolution = 256;
        settings.shadowCascades = 1;
        settings.shadowPoolMiB = 32;
        settings.referenceLighting = false;
        settings.forceShadowRefresh = false;
        setLighting(editor, settings);
        editEntity(editor, state.sunId, [](EntityRecord& value) { value.directionalLight->intensity = 3.0f; });
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.pointLight->intensity = 30.0f; });
        for (size_t i = 0; i < state.lampIds.size() && i < 5; ++i)
            editEntity(editor, state.lampIds[i], [i](EntityRecord& value) {
                value.enabled = true;
                value.transform.position = {-2.0f + float(i) * 1.0f, 3.0f, 2.0f};
                value.pointLight->intensity = 6.0f + float(i);
                value.pointLight->radius = 12.0f;
                value.pointLight->shadows = true;
            });
    } else if (phase == LightingSmokeState::Phase::Overflow) {
        auto settings = editor.document().scene.lighting;
        settings.shadowPoolMiB = 64;
        settings.shadowResolution = 512;
        settings.shadowCascades = 3;
        settings.forceShadowRefresh = false;
        setLighting(editor, settings);
        editEntity(editor, state.sunId, [](EntityRecord& value) { value.directionalLight->intensity = 0.0f; });
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.pointLight->intensity = 0.0f; });
        for (size_t i = 0; i < state.lampIds.size(); ++i)
            editEntity(editor, state.lampIds[i], [i](EntityRecord& value) {
                value.enabled = true;
                value.transform.position = {-.75f + float(i % 17) * .095f, 2.2f, -.7f + float(i / 17) * .095f};
                value.pointLight->intensity = .04f;
                value.pointLight->radius = 8.0f;
                value.pointLight->shadows = false;
            });
    } else if (phase == LightingSmokeState::Phase::MotionSequence) {
        configureBaseFixture(editor, state);
        auto filtered = editor.document().scene.lighting;
        filtered.filteredShadows = true;
        setLighting(editor, filtered);
        editor.document().reparent(state.pointId, state.parentAId, false);
        for (const auto id : state.lampIds)
            editEntity(editor, id, [](EntityRecord& value) { value.enabled = false; });
        editEntity(editor, state.maskId, [&](EntityRecord& value) {
            value.enabled = true;
            value.mesh->mesh = state.maskMesh;
            value.transform.position = {-1.8f, 2.2f, 0};
            value.transform.scale = {1.55f, 1.55f, 1};
        });
        editEntity(editor, state.sunId, [](EntityRecord& value) { value.directionalLight->intensity = 3.0f; });
        editEntity(editor, state.pointId, [](EntityRecord& value) { value.pointLight->intensity = 28.0f; });
        editEntity(editor, state.cubeId, [](EntityRecord& value) {
            value.enabled = true;
            value.mesh->mesh = builtin::cube;
            value.mesh->castShadows = true;
        });
    }
}

void advanceToggle(Editor& editor, LightingSmokeState& state) {
    if (state.phase != LightingSmokeState::Phase::ShadowToggles)
        return;
    if (state.captureIndex == 1)
        editEntity(editor, state.cubeId, [](EntityRecord& value) { value.mesh->castShadows = true; });
    else if (state.captureIndex == 2)
        editEntity(editor, state.groundId, [](EntityRecord& value) { value.mesh->receiveShadows = false; });
    else if (state.captureIndex == 3)
        editEntity(editor, state.groundId, [](EntityRecord& value) { value.mesh->receiveShadows = true; });
}

void advanceReference(Editor& editor, LightingSmokeState& state) {
    if ((state.phase == LightingSmokeState::Phase::ReferenceLighting ||
         state.phase == LightingSmokeState::Phase::Overflow) &&
        state.captureIndex == 1) {
        auto settings = editor.document().scene.lighting;
        settings.referenceLighting = true;
        setLighting(editor, settings);
    }
}
void advanceMutation(Editor& editor, LightingSmokeState& state) {
    using Phase = LightingSmokeState::Phase;
    if (state.phase == Phase::ColorIntensity) {
        if (state.captureIndex == 1)
            editEntity(editor, state.pointId, [](auto& v) { v.pointLight->intensity = 0; });
        else if (state.captureIndex == 2)
            editEntity(editor, state.pointId, [](auto& v) { v.pointLight->intensity = 46; });
        else if (state.captureIndex == 3) {
            editEntity(editor, state.pointId, [](auto& v) { v.pointLight->intensity = 0; });
            editEntity(editor, state.sunId, [](auto& v) { v.directionalLight->intensity = 0; });
        }
    }
    if (state.captureIndex == 1) {
        if (state.phase == Phase::GeometryRevision)
            replaceMaskGeometry(editor.document().scene, state);
        if (state.phase == Phase::MaskMaterial)
            setMaskCutoff(editor.document().scene, state.maskMaterial, 0);
        if (state.phase == Phase::MaskSampler)
            changeMaskSampler(editor.document().scene, state, true);
        if (state.phase == Phase::NativeMeshReplacement)
            editEntity(editor, state.cubeId, [](auto& v) { v.mesh->mesh = builtin::plane; });
    }
    if ((state.phase == Phase::NativeMeshReplacement || state.phase == Phase::MaskSampler) && state.captureIndex == 2) {
        auto settings = editor.document().scene.lighting;
        settings.forceShadowRefresh = true;
        setLighting(editor, settings);
    }
}

void advanceCacheRefresh(Editor& editor, LightingSmokeState& state) {
    if (state.phase == LightingSmokeState::Phase::CacheRefresh && state.captureIndex == 1) {
        auto settings = editor.document().scene.lighting;
        settings.forceShadowRefresh = true;
        setLighting(editor, settings);
    }
}

void advanceTinyPool(Editor& editor, LightingSmokeState& state) {
    if (state.phase == LightingSmokeState::Phase::TinyPool && state.captureIndex < 4) {
        auto settings = editor.document().scene.lighting;
        settings.shadowResolution = 256;
        settings.shadowCascades = 1;
        settings.shadowPoolMiB = state.captureIndex % 2 ? 2 : 32;
        settings.filteredShadows = state.captureIndex >= 2;
        settings.forceShadowRefresh = false;
        setLighting(editor, settings);
    }
}

void advanceMotion(Editor& editor, LightingSmokeState& state) {
    if (state.phase != LightingSmokeState::Phase::MotionSequence || !state.motionSamplesRemaining)
        return;
    const uint32_t sample = 24u - state.motionSamplesRemaining;
    const float angle = float(sample) * .28f;
    editEntity(editor, state.pointId, [angle](EntityRecord& value) {
        value.transform.position = {2.5f + std::sin(angle) * 1.8f, 3.8f, 2.5f + std::cos(angle) * 1.8f};
    });
    editEntity(editor, state.parentBId, [angle](EntityRecord& value) {
        value.transform.position = {std::sin(angle) * .7f, 0, std::cos(angle) * .7f};
        value.transform.rotation = glm::angleAxis(angle * .2f, glm::vec3(0, 1, 0));
    });
    editEntity(editor, state.parentAId, [angle](auto& value) {
        value.transform.position = {std::sin(angle) * .25f, 0, std::cos(angle) * .25f};
        value.transform.rotation = glm::angleAxis(angle * .15f, glm::vec3(0, 1, 0));
    });
    --state.motionSamplesRemaining;
}

std::string captureName(const LightingSmokeState& state) {
    std::ostringstream name;
    name << lightingSmokePhaseName(state.phase) << '-' << state.captureIndex << ".png";
    return name.str();
}

} // namespace

const char* lightingSmokePhaseName(LightingSmokeState::Phase phase) {
    switch (phase) {
    case LightingSmokeState::Phase::StaticWarm:
        return "static-warm";
    case LightingSmokeState::Phase::ColorIntensity:
        return "color-intensity";
    case LightingSmokeState::Phase::MovingLight:
        return "moving-light";
    case LightingSmokeState::Phase::MovingCaster:
        return "moving-caster";
    case LightingSmokeState::Phase::SunMask:
        return "sun-mask";
    case LightingSmokeState::Phase::PointMask:
        return "point-mask";
    case LightingSmokeState::Phase::OffscreenCaster:
        return "offscreen-caster";
    case LightingSmokeState::Phase::RadiusChange:
        return "radius-change";
    case LightingSmokeState::Phase::GeometryRevision:
        return "geometry-revision";
    case LightingSmokeState::Phase::MaskMaterial:
        return "mask-material";
    case LightingSmokeState::Phase::MaskSampler:
        return "mask-sampler";
    case LightingSmokeState::Phase::NativeMeshReplacement:
        return "native-mesh-replacement";
    case LightingSmokeState::Phase::ShadowToggles:
        return "shadow-toggles";
    case LightingSmokeState::Phase::ReferenceLighting:
        return "reference-lighting";
    case LightingSmokeState::Phase::CacheRefresh:
        return "forced-refresh";
    case LightingSmokeState::Phase::TinyPool:
        return "tiny-pool";
    case LightingSmokeState::Phase::Overflow:
        return "tile-overflow";
    case LightingSmokeState::Phase::MotionSequence:
        return "motion-sequence";
    case LightingSmokeState::Phase::Complete:
        return "complete";
    }
    return "unknown";
}

void Editor::startLightingSmoke(const std::filesystem::path& source, const std::filesystem::path& scene) {
    if (source.empty() || !std::filesystem::exists(source))
        throw std::runtime_error("M3 lighting fixture does not exist: " + utf8(source.wstring()));
    if (scene.empty())
        throw std::runtime_error("M3 lighting smoke requires a dedicated scene path");

    lightingSmoke_ = std::make_shared<LightingSmokeState>();
    auto& state = *lightingSmoke_;
    state.sourcePath = std::filesystem::absolute(source).lexically_normal();
    state.scenePath = std::filesystem::absolute(scene).lexically_normal();
    state.resultDirectory = state.scenePath.parent_path() / L"m3-lighting";
    state.manifestPath = state.scenePath.parent_path() / L"m3-lighting-results.json";
    std::filesystem::create_directories(state.resultDirectory);

    document_.newScene();
    // Scene::demo is useful for ordinary editor startup, but the M3 fixture
    // must contain only the deterministic entities below.
    std::vector<EntityHandle> roots;
    for (const auto handle : document_.scene.entities())
        if (!document_.scene.entity(handle).parent)
            roots.push_back(handle);
    for (const auto handle : roots)
        if (document_.scene.find(document_.scene.entity(handle).id))
            document_.scene.eraseSubtree(handle);
    document_.scene.update();
    document_.selection = {};
    document_.save(state.scenePath);

    const auto workspace = document_.workspace();
    state.model = workspace->importFile(state.sourcePath);
    if (!state.model || state.model->meshes.size() < 3 || state.model->materials.size() < 3)
        throw std::runtime_error("M3 lighting fixture must provide mesh[2] and material[2]");
    state.modelId = state.model->id;
    state.maskMesh = state.model->meshes[2]->id;
    state.maskMaterial = state.model->materials[2]->id;
    document_.scene.assets->publish(state.model);
    document_.scene.modelSources.push_back(state.modelId);
    document_.scene.assetsChanged();

    state.groundId = entityId("00000000-0000-4000-8000-000000000201");
    state.cubeId = entityId("00000000-0000-4000-8000-000000000202");
    state.maskId = entityId("00000000-0000-4000-8000-000000000203");
    state.offscreenId = entityId("00000000-0000-4000-8000-000000000204");
    state.sunId = entityId("00000000-0000-4000-8000-000000000205");
    state.pointId = entityId("00000000-0000-4000-8000-000000000206");
    state.parentAId = entityId("00000000-0000-4000-8000-000000000207");
    state.parentBId = entityId("00000000-0000-4000-8000-000000000208");
    for (uint32_t i = 0; i < 137; ++i) {
        std::ostringstream value;
        value << "00000000-0000-4000-8000-000000000" << std::setw(3) << std::setfill('0') << (300 + i);
        state.lampIds.push_back(EntityId::parse(value.str()));
    }

    auto create = [&](EntityRecord value) {
        const auto name = value.name;
        try {
            document_.create(std::move(value));
        } catch (const std::exception& error) {
            throw std::runtime_error("M3 fixture entity failed (" + name + "): " + error.what());
        }
    };
    EntityRecord parentA{state.parentAId, "M3 parent A", {}, true};
    EntityRecord parentB{state.parentBId, "M3 parent B", {}, true};
    create(parentA);
    create(parentB);
    EntityRecord ground{state.groundId, "M3 ground", {}, true};
    ground.transform.scale = {10, 1, 10};
    ground.mesh = MeshRenderer{builtin::plane, glm::vec4(groundAlbedo, 1), false, true};
    create(ground);
    EntityRecord cube{state.cubeId, "M3 cube caster", state.parentAId, true};
    cube.transform.position = {0, 1, 0};
    cube.transform.rotation = glm::angleAxis(glm::radians(22.5f), glm::vec3(0, 1, 0));
    cube.transform.scale = {1.4f, 2.0f, 1.2f};
    cube.mesh = MeshRenderer{builtin::cube, glm::vec4(.42f, .5f, .62f, 1), true, true};
    create(cube);
    EntityRecord mask{state.maskId, "M3 MASK caster", state.parentBId, true};
    mask.transform.position = {-1.8f, 2.2f, 0};
    mask.transform.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0));
    mask.transform.scale = {1.55f, 1.55f, 1};
    mask.mesh = MeshRenderer{state.maskMesh, glm::vec4(1), true, false};
    create(mask);
    EntityRecord offscreen{state.offscreenId, "M3 offscreen caster", {}, false};
    offscreen.transform.position = {30, 1, 30};
    offscreen.mesh = MeshRenderer{builtin::cube, glm::vec4(.3f, .3f, .3f, 1), true, false};
    create(offscreen);
    EntityRecord sun{state.sunId, "M3 sun", {}, true};
    sun.transform.position = {0, 8, 0};
    sun.transform.rotation = downwardLightRotation();
    sun.directionalLight = DirectionalLight{};
    create(sun);
    EntityRecord point{state.pointId, "M3 point", {}, true};
    point.transform.position = {3, 4.5f, 3};
    point.pointLight = PointLight{};
    create(point);
    for (size_t i = 0; i < state.lampIds.size(); ++i) {
        EntityRecord lamp{state.lampIds[i], "M3 lamp " + std::to_string(i), {}, false};
        lamp.transform.position = {0, 2, 0};
        lamp.pointLight = PointLight{};
        lamp.pointLight->intensity = .04f;
        lamp.pointLight->radius = 8;
        lamp.pointLight->shadows = false;
        create(lamp);
    }
    document_.scene.update();
    configureBaseFixture(*this, state);
    document_.save(state.scenePath);
    document_.load(state.scenePath);
    state.sceneReopened = true;
    state.model = document_.scene.assets->models.at(state.modelId);
    ensureFixtureEntities(*this, state);

    document_.selection = {};
    showGrid_ = false;
    useSceneCamera_ = false;
    useStudioLighting_ = false;
    camera_.pivot = {0, .6f, 0};
    camera_.yaw = 0;
    camera_.pitch = .6f;
    camera_.distance = 11.0f;

    state.phaseIndex = 0;
    state.phase = phaseAt(0);
    state.phaseStartFrame = 0;
    state.captureIndex = 0;
    state.initialized = true;
    state.finished = false;
    state.passed = false;
    state.failure.clear();
    applyPhase(*this, state);
    writeManifest(state);
}

void Editor::lightingSmokeStep(uint64_t frame) {
    if (!lightingSmoke_ || !lightingSmoke_->initialized || lightingSmoke_->finished)
        return;
    auto& state = *lightingSmoke_;
    if (state.lastStepFrame == frame)
        return;
    state.lastStepFrame = frame;
    if (frame > state.trackAfterFrame) {
        const auto& stats = renderer_.assetRenderer()->lightingStats();
        state.peakShadowMisses = std::max(state.peakShadowMisses, stats.shadowMisses);
        state.peakShadowFaces = std::max(state.peakShadowFaces, stats.shadowFaces);
    }
    if (state.captureArmed)
        return;
    if (state.phase == LightingSmokeState::Phase::Complete) {
        state.finished = true;
        state.passed = state.failure.empty();
        writeManifest(state);
        return;
    }
    if (state.capturesRemaining == 0)
        return;
    if (!renderer_.assetRenderer() || renderer_.assetRenderer()->pending() != 0)
        return;
    if (frame < state.phaseStartFrame + state.settleFrames)
        return;
    state.capturePath = state.resultDirectory / captureName(state);
    state.captureArmed = true;
}

std::filesystem::path Editor::lightingSmokeCapture() const {
    if (!lightingSmoke_ || !lightingSmoke_->captureArmed)
        return {};
    return lightingSmoke_->capturePath;
}

void Editor::lightingColorProbes() {
    if (!lightingSmoke_ || !lightingSmoke_->initialized || lightingSmoke_->finished)
        return;
    auto& state = *lightingSmoke_;
    state.probes.clear();
    sceneView_.colorProbes.clear();
    sceneView_.depthProbes.clear();
    const auto groundHandle = document_.scene.find(state.groundId);
    if (!groundHandle)
        throw std::runtime_error("M3 lighting ground disappeared while preparing probes");
    const bool receiveShadows = document_.scene.mesh(groundHandle)->receiveShadows;
    if (state.phase == LightingSmokeState::Phase::OffscreenCaster) {
        if (std::any_of(sceneView_.imported.begin(), sceneView_.imported.end(),
                        [&](const auto& i) { return i.id == state.offscreenId; }) ||
            std::none_of(sceneView_.casters.begin(), sceneView_.casters.end(),
                         [&](const auto& i) { return i.id == state.offscreenId; }))
            throw std::runtime_error("Offscreen fixture must be camera-culled and remain a shadow caster");
    }
    const auto project = [&](glm::vec3 point) {
        const glm::vec4 projected = sceneView_.viewProjection * glm::vec4(point, 1);
        return glm::vec3(projected.x / projected.w * .5f + .5f, .5f - projected.y / projected.w * .5f,
                         projected.z / projected.w);
    };
    const auto candidate = [&](glm::vec3 point, const char* label) {
        auto projected = project(point);
        if (!std::isfinite(projected.x) || !std::isfinite(projected.y) || !std::isfinite(projected.z) ||
            projected.x <= .02f || projected.x >= .98f || projected.y <= .02f || projected.y >= .98f ||
            projected.z <= 0 || projected.z >= 1)
            return;
        const auto extent = renderer_.viewportExtent();
        projected.x = (std::floor(projected.x * float(extent.width)) + .5f) / float(extent.width);
        projected.y = (std::floor(projected.y * float(extent.height)) + .5f) / float(extent.height);
        const auto ray = viewportRay(sceneView_.viewProjection, projected.x, projected.y);
        const auto hit = document_.scene.pick(ray);
        if (!hit || hit->id != state.groundId || std::abs(hit->position.y) > .02f)
            return;
        point = hit->position;
        const glm::vec3 expected = expectedGroundRgb(document_.scene, sceneView_, point, groundAlbedo, receiveShadows);
        const glm::vec3 unshadowed = expectedGroundRgb(document_.scene, sceneView_, point, groundAlbedo, false);
        // Ray visibility for every light must agree throughout this footprint.
        // This excludes raster/PCF silhouette boundaries without excluding a
        // legitimate smooth change in point-light brightness.
        if (receiveShadows && !stableGroundVisibility(document_.scene, sceneView_, point, .25f))
            return;
        const bool shadowed = glm::length(unshadowed - expected) > 5.0f;
        state.probes.push_back({point, expected, unshadowed, projected.x, projected.y, label, shadowed});
        sceneView_.colorProbes.push_back({projected.x, projected.y, expected, probeTolerance, label});
        sceneView_.depthProbes.push_back({projected.x, projected.y, project(point).z});
    };
    int index{};
    for (int x = -7; x <= 7; ++x)
        for (int z = -7; z <= 7; ++z) {
            const glm::vec3 point{float(x) * .55f, 0, float(z) * .55f};
            candidate(point, (std::string("ground-") + std::to_string(index++)).c_str());
        }
    if (state.probes.size() < minimumProbes)
        throw std::runtime_error("M3 lighting smoke found too few visible interior ground probes");
}

void Editor::lightingSmokeCaptured() {
    if (!lightingSmoke_ || !lightingSmoke_->captureArmed)
        return;
    auto& state = *lightingSmoke_;
    const auto& pixels = renderer_.capturedViewportPixels();
    const auto extent = renderer_.capturedViewportExtent();
    if (pixels.empty() || !extent.width || !extent.height)
        throw std::runtime_error("M3 lighting smoke capture readback is empty");
    const size_t expectedBytes = size_t(extent.width) * extent.height * 4;
    if (pixels.size() < expectedBytes)
        throw std::runtime_error("M3 lighting smoke viewport readback is truncated");

    LightingSmokePhaseResult result;
    result.name = lightingSmokePhaseName(state.phase);
    result.startFrame = state.phaseStartFrame;
    result.captureFrame = state.lastStepFrame;
    result.captureIndex = state.captureIndex;
    result.capturesExpected = captureCount(state.phase);
    result.probeCount = static_cast<uint32_t>(state.probes.size());
    const auto& stats = renderer_.assetRenderer()->lightingStats();
    result.shadowCacheHits = stats.shadowCacheHits;
    result.shadowMisses = stats.shadowMisses;
    result.peakShadowMisses = std::max(stats.shadowMisses, state.peakShadowMisses);
    result.peakShadowFaces = std::max(stats.shadowFaces, state.peakShadowFaces);
    result.shadowFaces = stats.shadowFaces;
    result.shadowSlots = stats.shadowSlots;
    result.batches = stats.batches;
    result.tileOverflows = stats.tileOverflows;
    result.shadowBytes = stats.shadowBytes;
    double minExpected = std::numeric_limits<double>::max(), maxExpected = 0;
    bool shadowContrast{};
    for (const auto& probe : state.probes) {
        const auto x = std::min(static_cast<uint32_t>(probe.u * float(extent.width)), extent.width - 1);
        const auto y = std::min(static_cast<uint32_t>(probe.v * float(extent.height)), extent.height - 1);
        const size_t at = (size_t(y) * extent.width + x) * 4;
        const glm::vec3 actual(pixels[at], pixels[at + 1], pixels[at + 2]);
        const auto difference = glm::abs(actual - probe.expected);
        const float error = std::max({difference.r, difference.g, difference.b});
        result.maxError = std::max(result.maxError, error);
        if (error <= probeTolerance)
            ++result.probePasses;
        const double expectedLight = luminance(probe.expected);
        minExpected = std::min(minExpected, expectedLight);
        maxExpected = std::max(maxExpected, expectedLight);
        shadowContrast |= probe.shadowed;
    }
    result.expectedContrast = maxExpected - minExpected > 12.0;
    result.expectedShadowContrast = shadowContrast;

    const bool pixelPass = result.probePasses == result.probeCount;
    bool metricPass = true;
    if (phaseNeedsContrast(state.phase))
        metricPass &= result.expectedContrast && result.expectedShadowContrast;
    const bool mutationBaseline = (state.phase == LightingSmokeState::Phase::GeometryRevision ||
                                   state.phase == LightingSmokeState::Phase::MaskMaterial ||
                                   state.phase == LightingSmokeState::Phase::NativeMeshReplacement ||
                                   state.phase == LightingSmokeState::Phase::MaskSampler) &&
                                  state.captureIndex == 0;
    metricPass &= stats.shadowBytes <= uint64_t(document_.scene.lighting.shadowPoolMiB) * 1024 * 1024;
    if (phaseNeedsShadowMiss(state.phase) && !mutationBaseline)
        metricPass &= result.peakShadowMisses > 0;
    if (state.phase == LightingSmokeState::Phase::StaticWarm && state.captureIndex == 1)
        metricPass &= result.shadowMisses == 0 && result.shadowCacheHits > 0;
    if (state.phase == LightingSmokeState::Phase::ColorIntensity)
        metricPass &= result.peakShadowMisses == 0;
    if (state.phase == LightingSmokeState::Phase::CacheRefresh && state.captureIndex == 1)
        metricPass &= result.shadowMisses > 0;
    if (state.phase == LightingSmokeState::Phase::TinyPool && state.captureIndex % 2 == 1)
        metricPass &= result.shadowMisses > 0;
    if (state.phase == LightingSmokeState::Phase::Overflow)
        metricPass &= result.tileOverflows > 0 && stats.lights == 137;
    if (state.phase == LightingSmokeState::Phase::TinyPool && state.captureIndex % 2 == 1)
        metricPass &= result.shadowSlots == 1 && result.batches >= 2;
    if (comparesImages(state.phase) && state.captureIndex == comparisonStart(state.phase, state.captureIndex) + 1) {
        if (state.comparisonWidth != extent.width || state.comparisonHeight != extent.height ||
            state.comparisonPixels.size() != expectedBytes)
            metricPass = false;
        else {
            result.fullImageCompared = true;
            double totalDifference{};
            for (size_t i = 0; i < expectedBytes; i += 4)
                for (size_t channel = 0; channel < 3; ++channel) {
                    const float difference = static_cast<float>(
                        std::abs(int(pixels[i + channel]) - int(state.comparisonPixels[i + channel])));
                    result.fullImageMaxDifference = std::max(result.fullImageMaxDifference, difference);
                    totalDifference += difference;
                }
            result.fullImageMeanDifference =
                static_cast<float>(totalDifference / static_cast<double>(expectedBytes / 4 * 3));
            const float maxAllowed = 2.0f;
            const float meanAllowed = .1f;
            metricPass &= result.fullImageMaxDifference <= maxAllowed && result.fullImageMeanDifference <= meanAllowed;
        }
    }
    result.passed = pixelPass && metricPass;
    state.phases.push_back(result);
    if (!result.passed) {
        std::ostringstream message;
        message << "M3 lighting phase " << result.name << " failed: probes=" << result.probePasses << '/'
                << result.probeCount << " maxError=" << result.maxError << " shadowMisses=" << result.shadowMisses
                << " cacheHits=" << result.shadowCacheHits << " overflows=" << result.tileOverflows;
        state.failure = message.str();
        state.finished = true;
        state.passed = false;
        state.captureArmed = false;
        writeManifest(state);
        throw std::runtime_error(state.failure);
    }

    if (comparesImages(state.phase) && state.captureIndex == comparisonStart(state.phase, state.captureIndex)) {
        state.comparisonPixels.assign(pixels.begin(), pixels.begin() + expectedBytes);
        state.comparisonWidth = extent.width;
        state.comparisonHeight = extent.height;
    }
    state.captureArmed = false;
    state.peakShadowMisses = state.peakShadowFaces = 0;
    state.trackAfterFrame = renderer_.renderedFrames();
    --state.capturesRemaining;
    ++state.captureIndex;
    advanceMutation(*this, state);
    advanceToggle(*this, state);
    advanceReference(*this, state);
    advanceCacheRefresh(*this, state);
    advanceTinyPool(*this, state);
    if (state.phase == LightingSmokeState::Phase::MotionSequence) {
        auto settings = document_.scene.lighting;
        settings.forceShadowRefresh = state.captureIndex % 2 == 1;
        document_.lighting(settings);
        if (state.captureIndex % 2 == 0) {
            advanceMotion(*this, state);
            // Move the camera between paired poses, crossing the cascade
            // depth ranges while both captures in a pair keep the same VP.
            camera_.distance = 11 + 2.5f * std::sin(float(state.captureIndex / 2) * .28f);
        }
    }
    state.phaseStartFrame = state.lastStepFrame;
    if (state.capturesRemaining)
        return;
    ++state.phaseIndex;
    state.phase = phaseAt(state.phaseIndex);
    state.phaseStartFrame = state.lastStepFrame;
    state.captureIndex = 0;
    state.comparisonPixels.clear();
    state.comparisonWidth = state.comparisonHeight = 0;
    applyPhase(*this, state);
    if (state.phase == LightingSmokeState::Phase::Complete) {
        state.finished = true;
        state.passed = state.failure.empty();
    }
    writeManifest(state);
}

bool Editor::lightingSmokePassed() const {
    return lightingSmoke_ && lightingSmoke_->finished && lightingSmoke_->passed;
}

} // namespace proto
