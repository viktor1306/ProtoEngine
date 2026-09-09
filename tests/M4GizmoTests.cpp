#include "editor/GizmoMath.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

bool close(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= tolerance;
}

bool closeVec(glm::vec3 actual, glm::vec3 expected, float tolerance = 1.0e-4f) {
    return close(actual.x, expected.x, tolerance) && close(actual.y, expected.y, tolerance) &&
           close(actual.z, expected.z, tolerance);
}

bool closeMat3(const glm::mat3& actual, const glm::mat3& expected, float tolerance = 2.0e-4f) {
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            if (!close(actual[column][row], expected[column][row], tolerance))
                return false;
    return true;
}

Transform childTransform() {
    Transform result;
    result.position = {1.2f, 0.7f, -1.4f};
    result.rotation =
        glm::normalize(glm::angleAxis(-0.28f, glm::vec3(0, 1, 0)) * glm::angleAxis(0.17f, glm::vec3(1, 0, 0)));
    result.scale = {-1.4f, 0.75f, 0.45f};
    return result;
}

glm::mat4 compose(const Transform& transform) {
    return glm::translate(glm::mat4(1), transform.position) * glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4(1), transform.scale);
}

glm::mat4 parentMatrix() {
    Transform parent;
    parent.position = {2.0f, 1.0f, -3.0f};
    parent.rotation =
        glm::normalize(glm::angleAxis(0.41f, glm::vec3(0, 1, 0)) * glm::angleAxis(-0.22f, glm::vec3(1, 0, 0)));
    parent.scale = {-2.0f, 1.5f, 0.6f};
    return compose(parent);
}
} // namespace

int main() {
    unsigned passed{};
    unsigned failed{};
    const auto test = [&](const char* name, const auto& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("world translation preserves linear transform through nonuniform negative parent", [] {
        const glm::mat4 parent = parentMatrix();
        const Transform start = childTransform();
        const glm::mat4 before = parent * compose(start);
        const glm::vec3 desired = glm::vec3(before[3]) + glm::vec3(1.25f, 0, 0);
        const auto translated = applyWorldTranslation(start, parent, desired);
        check(translated.has_value(), "world translation unexpectedly rejected");
        const glm::mat4 after = parent * compose(*translated);
        check(closeVec(glm::vec3(after[3]), desired), "world pivot did not reach target");
        check(closeMat3(glm::mat3(after), glm::mat3(before)), "world translation changed linear matrix");
        check(translated->rotation == start.rotation && translated->scale == start.scale,
              "world translation changed local rotation or scale");
    });

    test("local rotation stays representable beneath a sheared world parent", [] {
        const glm::mat4 parent = parentMatrix();
        const Transform start = childTransform();
        const glm::vec3 pivot = glm::vec3((parent * compose(start))[3]);
        const auto basis = makeGizmoBasis(start, parent, pivot);
        check(invertibleMatrix(parent), "fixture parent is singular");
        const auto rotated = applyLocalRotation(start, GizmoAxis::Y, 0.63f);
        check(rotated.has_value(), "local rotation unexpectedly rejected");
        const auto expectedRotation = glm::normalize(start.rotation * glm::angleAxis(0.63f, glm::vec3(0, 1, 0)));
        check(close(glm::dot(rotated->rotation, expectedRotation), 1.0f, 2.0e-4f), "wrong local rotation quaternion");
        const glm::mat4 before = parent * compose(start), after = parent * compose(*rotated);
        check(closeVec(glm::vec3(after[3]), pivot), "local rotation moved pivot");
        check(!closeMat3(glm::mat3(after), glm::mat3(before)), "local rotation did not change linear transform");
        check(closeVec(basis.pivotWorld, pivot), "basis pivot mismatch");
    });

    test("ring angle uses an elliptical parent-transformed local frame", [] {
        const glm::mat4 parent = parentMatrix();
        const Transform start = childTransform();
        const glm::vec3 pivot = glm::vec3((parent * compose(start))[3]);
        const GizmoBasis basis = makeGizmoBasis(start, parent, pivot);
        const glm::vec3 u = basis.worldBasis[2];
        const glm::vec3 v = basis.worldBasis[0];
        const glm::vec3 normal = glm::normalize(glm::cross(u, v));
        const glm::vec3 point = pivot + (u * std::cos(0.72f) + v * std::sin(0.72f)) * 1.8f;
        const Ray ray{point + normal * 5.0f, -normal};
        const auto angle = ringAngle(ray, basis, GizmoAxis::Y);
        check(angle.has_value(), "ring ray did not intersect transformed plane");
        check(close(wrapAngle(*angle), 0.72f, 2.0e-4f), "ring angle did not map through parent shear");
    });

    test("local scale preserves sign and clamps positive factor", [] {
        const Transform start = childTransform();
        const auto scaled = applyLocalScale(start, GizmoAxis::X, 0.5f);
        check(scaled.has_value(), "local scale unexpectedly rejected");
        check(close(scaled->scale.x, -0.7f), "local scale value mismatch");
        check(close(scaled->scale.y, start.scale.y) && close(scaled->scale.z, start.scale.z),
              "axis scale changed unrelated components");
        check(std::signbit(scaled->scale.x) == std::signbit(start.scale.x), "axis scale lost negative sign");
        const auto clamped = applyLocalScale(start, GizmoAxis::X, 0.0f);
        check(!clamped.has_value(), "non-positive scale factor was accepted");
        const auto uniform = applyLocalScale(start, GizmoAxis::All, 0.25f);
        check(uniform.has_value() && close(uniform->scale.x, -0.35f) && close(uniform->scale.y, 0.1875f) &&
                  close(uniform->scale.z, 0.1125f),
              "uniform local scale mismatch");
    });

    test("singular parent and nonfinite inputs are rejected without mutation", [] {
        const Transform start = childTransform();
        const glm::mat4 singular = glm::scale(glm::mat4(1), glm::vec3(0, 1, 1));
        check(!invertibleMatrix(singular), "singular parent reported invertible");
        check(!applyWorldTranslation(start, singular, {1, 2, 3}), "singular parent accepted world translation");
        check(!closestAxisParameter({{0, 0, 0}, {0, 0, 0}}, {}, {1, 0, 0}), "zero ray accepted");
        auto nonfinite = singular;
        nonfinite[0][0] = std::numeric_limits<float>::infinity();
        check(!finiteMatrix(nonfinite), "nonfinite matrix reported finite");
        check(!applyLocalScale(start, GizmoAxis::X, std::numeric_limits<float>::infinity()),
              "nonfinite scale factor accepted");
    });

    test("axis parameter and angle wrapping are stable", [] {
        const Ray ray{{0, 2, 5}, {0, -0.2f, -1}};
        const auto parameter = closestAxisParameter(ray, {0, 0, 0}, {1, 0, 0});
        check(parameter && close(*parameter, 0.0f), "axis closest-point parameter mismatch");
        check(close(wrapAngle(4.0f), 4.0f - 2.0f * 3.14159265358979323846f) &&
                  close(wrapAngle(-4.0f), -4.0f + 2.0f * 3.14159265358979323846f),
              "angle wrapping mismatch");
    });

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
