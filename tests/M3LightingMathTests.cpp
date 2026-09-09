#include "renderer/LightingMath.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace proto;
namespace {
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}

void closeEnough(float actual, float expected, float tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        std::ostringstream text;
        text << message << " actual=" << actual << " expected=" << expected << " tolerance=" << tolerance;
        throw std::runtime_error(text.str());
    }
}

void closeEnough(const glm::vec3& actual, const glm::vec3& expected, float tolerance, const std::string& message) {
    for (int i = 0; i < 3; ++i)
        closeEnough(actual[i], expected[i], tolerance, message);
}

std::string vectorText(glm::vec3 value) {
    std::ostringstream text;
    text << '(' << value.x << ',' << value.y << ',' << value.z << ')';
    return text.str();
}

glm::vec3 point(const glm::mat4& matrix, glm::vec3 value) {
    return glm::vec3(matrix * glm::vec4(value, 1.0f));
}

float ndcDepth(const glm::mat4& matrix, glm::vec3 value) {
    const auto clip = matrix * glm::vec4(value, 1.0f);
    check(std::abs(clip.w) > 1e-7f, "Projection produced a zero clip w");
    return clip.z / clip.w;
}

std::array<glm::vec3, 4> cameraSliceCorners(float depth, float tangent, float aspect) {
    std::array<glm::vec3, 4> corners{};
    size_t at{};
    for (float y : {-1.0f, 1.0f})
        for (float x : {-1.0f, 1.0f})
            corners[at++] = glm::vec3(x * depth * tangent * aspect, y * depth * tangent, -depth);
    return corners;
}

void cubeFaceMapping() {
    const glm::vec3 light{3.25f, -1.5f, 7.0f};
    const auto matrices = pointShadowMatrices(light, 12.0f);
    const std::array<glm::vec3, 6> axes{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};

    // The array order is the Vulkan cube order. Exact face-axis samples make a
    // wrong order visible before the asymmetric edge samples exercise sc/tc.
    for (size_t face = 0; face < axes.size(); ++face) {
        const auto clip = matrices[face] * glm::vec4(light + axes[face] * 4.0f, 1.0f);
        check(clip.w > 0.0f, "Cube face has the wrong forward direction");
        closeEnough(clip.x / clip.w, 0.0f, 0.00001f, "Cube face center sc is not zero");
        closeEnough(clip.y / clip.w, 0.0f, 0.00001f, "Cube face center tc is not zero");
        for (size_t other = 0; other < axes.size(); ++other) {
            if (other == face)
                continue;
            const auto otherClip = matrices[other] * glm::vec4(light + axes[face] * 4.0f, 1.0f);
            check(otherClip.w <= 0.00001f, "Cube face order accepts a perpendicular or opposite center");
        }
    }

    struct Sample {
        glm::vec3 direction;
        size_t face;
        float sc;
        float tc;
    };
    const std::array<Sample, 10> samples{{
        {{1.0f, -0.997f, 0.371f}, 0, -0.371f, 0.997f},
        {{-1.0f, -0.997f, -0.371f}, 1, -0.371f, 0.997f},
        {{0.371f, 1.0f, 0.997f}, 2, 0.371f, 0.997f},
        {{0.371f, -1.0f, -0.997f}, 3, 0.371f, 0.997f},
        {{0.997f, -0.371f, 1.0f}, 4, 0.997f, 0.371f},
        {{-0.997f, -0.371f, -1.0f}, 5, 0.997f, 0.371f},
        // Samples just across four cube edges keep the two components
        // asymmetric, so a sign or face-selection error cannot hide at a seam.
        {{1.001f, 1.0f, 0.231f}, 0, -0.231f / 1.001f, -1.0f / 1.001f},
        {{1.0f, 1.001f, 0.231f}, 2, 1.0f / 1.001f, 0.231f / 1.001f},
        {{-1.001f, -1.0f, 0.231f}, 1, 0.231f / 1.001f, 1.0f / 1.001f},
        {{-1.0f, -1.001f, 0.231f}, 3, -1.0f / 1.001f, -0.231f / 1.001f},
    }};
    for (const auto& sample : samples) {
        const auto direction = glm::normalize(sample.direction);
        const auto clip = matrices[sample.face] * glm::vec4(light + direction * 5.0f, 1.0f);
        check(clip.w > 0.0f, "Asymmetric cube sample is behind its expected face");
        const glm::vec2 st{0.5f * (clip.x / clip.w + 1.0f), 0.5f * (clip.y / clip.w + 1.0f)};
        closeEnough(st.x, 0.5f * (sample.sc + 1.0f), 0.00015f,
                    "Positive-viewport cube sc mapping failed for direction " + vectorText(sample.direction));
        closeEnough(st.y, 0.5f * (sample.tc + 1.0f), 0.00015f,
                    "Positive-viewport cube tc mapping failed for direction " + vectorText(sample.direction));
    }
}

void pointRadiusDepth() {
    const glm::vec3 light{-2.0f, 4.0f, 1.0f};
    const float radius = 12.0f;
    const float nearPlane = 0.005f;
    const auto matrices = pointShadowMatrices(light, radius);
    const std::array<glm::vec3, 6> axes{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};

    // RH ZO projection depth is a function of the face-forward distance. The
    // final point-shadow shader's radial depth is deliberately checked below
    // as a separate quantity.
    for (size_t face = 0; face < axes.size(); ++face) {
        for (float distance : {nearPlane, radius * 0.5f, radius * 0.95f, radius}) {
            const float expected = radius * (distance - nearPlane) / ((radius - nearPlane) * distance);
            const float actual = ndcDepth(matrices[face], light + axes[face] * distance);
            // At the near plane, subtracting two translated world-space
            // positions leaves only a few float ulps before the division by
            // the tiny distance. Keep the oracle exact and allow that
            // representation error only for this endpoint.
            const float tolerance = distance == nearPlane ? 0.00005f : 0.00002f;
            closeEnough(actual, expected, tolerance, "RH ZO point depth disagrees with near/far oracle");
        }
    }

    const float clampedNear = 0.005f;
    const auto clamped = pointShadowMatrices(light, 1.0f);
    closeEnough(ndcDepth(clamped[0], light + glm::vec3(1, 0, 0) * clampedNear), 0.0f, 0.00002f,
                "Small point radius did not use the minimum near clip");
    closeEnough(ndcDepth(clamped[0], light + glm::vec3(1, 0, 0)), 1.0f, 0.00002f,
                "Small point radius far clip is not the radius");

    const glm::vec3 offset{4.0f, 1.0f, 2.0f};
    const float faceDistance = offset.x;
    const float radialDepth = glm::length(offset) / radius;
    const float matrixDepth = ndcDepth(matrices[0], light + offset);
    const float expectedFaceDepth = radius * (faceDistance - nearPlane) / ((radius - nearPlane) * faceDistance);
    closeEnough(matrixDepth, expectedFaceDepth, 0.00002f, "Off-axis point clip depth is not face-forward RH ZO depth");
    check(std::abs(matrixDepth - radialDepth) > 0.1f,
          "Point projection test accidentally treats clip depth as final radial shadow depth");

    // A fixed near clip keeps close casters in a large point-light map. This
    // one-metre cube starts at 0.5m while its receiver is 10m away; the
    // independent RH-ZO oracle must keep both in the +X face clip volume even
    // when the light radius is 10km.
    const float longRadius = 10000.0f;
    const auto longMatrices = pointShadowMatrices(light, longRadius);
    for (float x : {0.5f, 1.5f})
        for (float y : {-0.5f, 0.5f})
            for (float z : {-0.5f, 0.5f}) {
                const glm::vec3 caster = light + glm::vec3(x, y, z);
                const auto clip = longMatrices[0] * glm::vec4(caster, 1.0f);
                check(clip.w > 0.0f, "Long-radius one-metre caster is behind the +X face");
                const auto ndc = glm::vec3(clip) / clip.w;
                check(ndc.x >= -1.001f && ndc.x <= 1.001f && ndc.y >= -1.001f && ndc.y <= 1.001f && ndc.z >= -0.001f &&
                          ndc.z <= 1.001f,
                      "Long-radius one-metre caster was clipped before radial shadow depth");
                const float expected = longRadius * (x - 0.005f) / ((longRadius - 0.005f) * x);
                closeEnough(ndc.z, expected, 0.00002f, "Long-radius caster depth disagrees with fixed-near oracle");
            }
    const float receiverDepth = ndcDepth(longMatrices[0], light + glm::vec3(10.0f, 0, 0));
    const float expectedReceiver = longRadius * (10.0f - 0.005f) / ((longRadius - 0.005f) * 10.0f);
    closeEnough(receiverDepth, expectedReceiver, 0.00002f,
                "Long-radius 10m receiver depth disagrees with fixed-near oracle");
}

void frustumBoundaries() {
    const glm::mat4 identity(1.0f);
    const Bounds inside{{-0.25f, -0.25f, 0.2f}, {0.25f, 0.25f, 0.4f}};
    check(shadowFrustumContains(inside, identity), "Interior AABB was rejected");
    const std::array<std::pair<Bounds, bool>, 12> cases{{
        {{{-1.0f, -0.2f, 0.2f}, {-0.8f, 0.2f, 0.4f}}, true},
        {{{0.8f, -0.2f, 0.2f}, {1.0f, 0.2f, 0.4f}}, true},
        {{{-0.2f, -1.0f, 0.2f}, {0.2f, -0.8f, 0.4f}}, true},
        {{{-0.2f, 0.8f, 0.2f}, {0.2f, 1.0f, 0.4f}}, true},
        {{{-0.2f, -0.2f, 0.0f}, {0.2f, 0.2f, 0.2f}}, true},
        {{{-0.2f, -0.2f, 0.8f}, {0.2f, 0.2f, 1.0f}}, true},
        {{{-1.2f, -0.2f, 0.2f}, {-1.0001f, 0.2f, 0.4f}}, false},
        {{{1.0001f, -0.2f, 0.2f}, {1.2f, 0.2f, 0.4f}}, false},
        {{{-0.2f, -1.2f, 0.2f}, {0.2f, -1.0001f, 0.4f}}, false},
        {{{-0.2f, 1.0001f, 0.2f}, {0.2f, 1.2f, 0.4f}}, false},
        {{{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, -0.0001f}}, false},
        {{{-0.2f, -0.2f, 1.0001f}, {0.2f, 0.2f, 1.2f}}, false},
    }};
    const auto prepared = makeShadowFrustum(identity);
    for (const auto& [bounds, expected] : cases) {
        check(shadowFrustumContains(bounds, identity) == expected,
              "AABB frustum boundary case returned the wrong visibility result");
        check(shadowFrustumContains(bounds, prepared) == expected,
              "Prepared frustum changed a boundary visibility result");
    }

    // Crossing one clip plane remains conservatively visible.
    check(shadowFrustumContains(Bounds{{-1.2f, -0.1f, 0.2f}, {-0.8f, 0.1f, 0.4f}}, identity),
          "AABB crossing a clip plane was culled");
}

void sphereContacts() {
    const Bounds box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    check(sphereTouches(box, {0, 0, 0}, 0.0f), "Zero-radius sphere inside AABB did not touch");
    check(sphereTouches(box, {2, 0, 0}, 1.0f), "Sphere face contact was rejected");
    check(!sphereTouches(box, {2.0001f, 0, 0}, 1.0f), "Sphere just beyond face contact was accepted");
    const float edge = std::sqrt(2.0f);
    check(sphereTouches(box, {2, 2, 0}, edge), "Sphere edge contact was rejected");
    check(!sphereTouches(box, {2, 2, 0}, edge - 0.0001f), "Sphere just inside edge radius was accepted");
    const float corner = std::sqrt(3.0f);
    check(sphereTouches(box, {2, 2, 2}, corner), "Sphere corner contact was rejected");
    check(!sphereTouches(box, {2, 2, 2}, corner - 0.0001f), "Sphere just inside corner radius was accepted");
}

void transformedBounds() {
    const Bounds local{{-2.0f, 0.5f, -1.25f}, {1.5f, 3.0f, 2.25f}};
    const auto world = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, -3.0f, 2.0f)) *
                       glm::rotate(glm::mat4(1.0f), glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, -1))) *
                       glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 3.0f, 0.25f));
    Bounds expected{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
    for (int z : {0, 1})
        for (int y : {0, 1})
            for (int x : {0, 1}) {
                const glm::vec3 localPoint{x ? local.max.x : local.min.x, y ? local.max.y : local.min.y,
                                           z ? local.max.z : local.min.z};
                const auto transformed = point(world, localPoint);
                expected.min = glm::min(expected.min, transformed);
                expected.max = glm::max(expected.max, transformed);
            }
    const auto actual = transformBounds(local, world);
    closeEnough(actual.min, expected.min, 0.0001f, "Negative/nonuniform transformed min bound is incorrect");
    closeEnough(actual.max, expected.max, 0.0001f, "Negative/nonuniform transformed max bound is incorrect");
}

RenderView cascadeView() {
    RenderView view;
    view.cameraView = glm::mat4(1.0f);
    view.verticalFovDegrees = 60.0f;
    view.cameraNear = 0.5f;
    view.cameraFar = 100.0f;
    view.lighting.shadowDistance = 40.0f;
    view.lighting.shadowCascades = 3;
    view.lighting.shadowResolution = 1024;
    return view;
}

void cascadesAndStability() {
    const glm::vec3 direction = glm::normalize(glm::vec3(-0.35f, -1.0f, -0.2f));
    const float aspect = 16.0f / 9.0f;
    const auto view = cascadeView();
    const auto cascades = directionalCascades(view, direction, aspect, {});
    const std::array<float, 3> expectedSplits{{6.7593275f, 16.3032399f, 40.0f}};
    for (size_t i = 0; i < expectedSplits.size(); ++i)
        closeEnough(cascades.splits[static_cast<int>(i)], expectedSplits[i], 0.0002f,
                    "CSM split does not match the independent coverage oracle");
    check(cascades.splits[0] > view.cameraNear && cascades.splits[0] < cascades.splits[1] &&
              cascades.splits[1] < cascades.splits[2] && cascades.splits[2] <= view.lighting.shadowDistance,
          "CSM split ordering or shadow-distance coverage is invalid");
    closeEnough(cascades.splits[3], view.lighting.shadowDistance, 0.00001f, "Unused CSM split was not filled with far");

    const float tangent = std::tan(glm::radians(view.verticalFovDegrees) * 0.5f);
    float previous = view.cameraNear;
    for (size_t cascade = 0; cascade < 3; ++cascade) {
        const float split = cascades.splits[static_cast<int>(cascade)];
        const float begin =
            cascade ? std::max(view.cameraNear, previous - (previous - view.cameraNear) * 0.12f) : view.cameraNear;
        for (const float depth : {begin, split}) {
            for (const auto corner : cameraSliceCorners(depth, tangent, aspect)) {
                const auto clip = cascades.matrices[cascade] * glm::vec4(corner, 1.0f);
                check(std::abs(clip.w) > 1e-7f, "CSM coverage corner has zero clip w");
                const auto ndc = glm::vec3(clip) / clip.w;
                check(ndc.x >= -1.0005f && ndc.x <= 1.0005f && ndc.y >= -1.0005f && ndc.y <= 1.0005f &&
                          ndc.z >= -0.0005f && ndc.z <= 1.0005f,
                      "CSM matrix does not cover its independently built slice corners");
            }
        }
        previous = split;
    }

    // This caster is outside the camera's first slice (it sits above the near
    // plane) but overlaps that cascade in light XY. Its light-space Z must
    // expand the orthographic depth range.
    auto vertical = cascadeView();
    const std::vector<Bounds> offscreen{{{-0.25f, 19.5f, -0.25f}, {0.25f, 20.5f, 0.25f}}};
    const auto withoutCaster = directionalCascades(vertical, glm::vec3(0, -1, 0), 1.0f, {});
    const auto withCaster = directionalCascades(vertical, glm::vec3(0, -1, 0), 1.0f, offscreen);
    const glm::vec3 casterCenter{0, 20, 0};
    const float withoutDepth = ndcDepth(withoutCaster.matrices[0], casterCenter);
    const float withDepth = ndcDepth(withCaster.matrices[0], casterCenter);
    check(withoutDepth < -0.05f || withoutDepth > 1.05f,
          "Offscreen caster unexpectedly fit the unexpanded cascade depth range");
    check(withDepth >= -0.0005f && withDepth <= 1.0005f,
          "Offscreen caster Z was not included in the cascade depth range");

    auto moved = view;
    moved.cameraView = glm::translate(glm::mat4(1.0f), glm::vec3(-0.001f, 0, 0));
    const auto movedCascades = directionalCascades(moved, direction, aspect, {});
    for (size_t cascade = 0; cascade < 3; ++cascade)
        for (int column = 0; column < 4; ++column) {
            closeEnough(movedCascades.matrices[cascade][column][0], cascades.matrices[cascade][column][0], 0.000001f,
                        "Small camera move changed stabilized cascade X mapping");
            closeEnough(movedCascades.matrices[cascade][column][1], cascades.matrices[cascade][column][1], 0.000001f,
                        "Small camera move changed stabilized cascade Y mapping");
        }

    // Low-resolution, asymmetric light/camera placement that used to leave
    // the far upper-right corner just outside the snapped first cascade.
    auto adversarial = cascadeView();
    adversarial.lighting.shadowResolution = 256;
    const glm::vec3 cameraTranslation{-0.0197442203f, -0.0111061239f, 0.00890671042f};
    adversarial.cameraView = glm::translate(glm::mat4(1.0f), -cameraTranslation);
    const glm::vec3 adversarialDirection{-0.415903478f, 0.889845228f, 0.187616010f};
    const auto adversarialCascades = directionalCascades(adversarial, adversarialDirection, aspect, {});
    const float farSplit = adversarialCascades.splits[0];
    const float adversarialTangent = std::tan(glm::radians(adversarial.verticalFovDegrees) * 0.5f);
    const glm::vec3 upperRight =
        cameraTranslation + glm::vec3(farSplit * adversarialTangent * aspect, farSplit * adversarialTangent, -farSplit);
    const auto projected = adversarialCascades.matrices[0] * glm::vec4(upperRight, 1.0f);
    const float projectedY = projected.y / projected.w;
    check(projectedY <= 1.0005f,
          "Adversarial low-resolution cascade lost its far upper-right corner after texel snapping");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    unsigned passed{}, failed{};
    auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("positive-viewport cube face sc/tc mapping", cubeFaceMapping);
    test("point radius RH ZO clip depth and radial-depth separation", pointRadiusDepth);
    test("shadow frustum boundary cases", frustumBoundaries);
    test("sphere and AABB contact", sphereContacts);
    test("transformed bounds with negative nonuniform scale", transformedBounds);
    test("cascade coverage, offscreen caster depth, and texel stability", cascadesAndStability);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
