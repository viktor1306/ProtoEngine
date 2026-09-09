#pragma once

#include "assets/AssetTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace proto {

struct LightingSmokeProbe {
    glm::vec3 world{};
    glm::vec3 expected{};
    glm::vec3 unshadowed{};
    float u{}, v{};
    std::string label;
    bool shadowed{};
};

struct LightingSmokePhaseResult {
    std::string name;
    uint64_t startFrame{}, captureFrame{};
    uint32_t captureIndex{}, capturesExpected{}, probeCount{}, probePasses{};
    uint32_t shadowCacheHits{}, shadowMisses{}, shadowFaces{}, shadowSlots{}, batches{}, tileOverflows{};
    uint32_t peakShadowMisses{}, peakShadowFaces{};
    uint64_t shadowBytes{};
    float maxError{}, fullImageMaxDifference{}, fullImageMeanDifference{};
    bool expectedContrast{}, expectedShadowContrast{}, passed{};
    bool fullImageCompared{};
};

// The state is deliberately defined outside Editor so the smoke implementation
// can be compiled as a separate translation unit while Editor keeps only a
// shared_ptr to it. IDs and phase data remain stable across save/reopen.
struct LightingSmokeState {
    enum class Phase {
        StaticWarm,
        ColorIntensity,
        MovingLight,
        MovingCaster,
        SunMask,
        PointMask,
        OffscreenCaster,
        RadiusChange,
        GeometryRevision,
        MaskMaterial,
        MaskSampler,
        NativeMeshReplacement,
        ShadowToggles,
        ReferenceLighting,
        CacheRefresh,
        TinyPool,
        Overflow,
        MotionSequence,
        Complete,
    };

    std::filesystem::path sourcePath;
    std::filesystem::path scenePath;
    std::filesystem::path resultDirectory;
    std::filesystem::path manifestPath;
    std::filesystem::path capturePath;

    std::shared_ptr<const ModelBundle> model;
    AssetId modelId;
    AssetId maskMesh;
    AssetId maskMaterial;

    EntityId groundId;
    EntityId cubeId;
    EntityId maskId;
    EntityId offscreenId;
    EntityId sunId;
    EntityId pointId;
    EntityId parentAId;
    EntityId parentBId;
    std::vector<EntityId> lampIds;

    Phase phase{Phase::StaticWarm};
    size_t phaseIndex{};
    uint64_t phaseStartFrame{};
    uint64_t trackAfterFrame{};
    uint32_t peakShadowMisses{}, peakShadowFaces{};
    uint64_t lastStepFrame{std::numeric_limits<uint64_t>::max()};
    uint32_t capturesRemaining{}, settleFrames{6};
    uint32_t captureIndex{};
    uint32_t motionSamplesRemaining{};
    bool initialized{}, captureArmed{}, sceneReopened{}, finished{}, passed{};
    std::string failure;

    std::vector<LightingSmokeProbe> probes;
    std::vector<uint8_t> comparisonPixels;
    uint32_t comparisonWidth{}, comparisonHeight{};
    std::vector<LightingSmokePhaseResult> phases;
};

const char* lightingSmokePhaseName(LightingSmokeState::Phase phase);

} // namespace proto
