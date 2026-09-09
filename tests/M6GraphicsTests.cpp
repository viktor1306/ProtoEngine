#include "runtime/PlayerGraphics.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <cmath>
#include <stdexcept>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
constexpr std::string_view balanced = R"json({
  "format":"proto.graphics",
  "formatVersion":1,
  "profile":"Balanced",
  "renderScale":1,
  "vSync":true,
  "viewDistance":300,
  "sunShadows":{"enabled":true,"cascades":3,"resolution":2048,"distance":80,"filter":"pcf3x3"},
  "pointShadows":{"enabled":true,"resolution":512,"filterTaps":9,"poolBudgetMiB":96,"overflow":"multiPass"},
  "textureTopMipDrop":1
})json";
void roundtrip() {
    const auto settings = decodePlayerGraphics(balanced);
    check(settings.profile == "Balanced" && settings.sunShadows.resolution == 2048,
          "Balanced graphics decode lost shadow fields");
    check(settings.pointShadows.filterTaps == 9 && settings.textureTopMipDrop == 1,
          "Balanced graphics decode lost point/mip fields");
    const auto encoded = encodePlayerGraphics(settings);
    const auto decoded = decodePlayerGraphics(encoded);
    check(decoded.profile == settings.profile && decoded.renderScale == settings.renderScale &&
              decoded.pointShadows.poolBudgetMiB == settings.pointShadows.poolBudgetMiB,
          "Graphics encode/decode roundtrip changed values");
}
void profiles() {
    for (const auto profile : {"Low", "Balanced", "High"}) {
        const auto settings = playerGraphicsProfile(profile);
        check(settings.profile == profile && settings.renderScale > 0 && settings.renderScale <= 1,
              "Graphics profile returned invalid scale");
    }
}
void strictRejection() {
    bool rejected{};
    try {
        decodePlayerGraphics(R"json({"format":"proto.graphics","formatVersion":1,"profile":"Balanced","renderScale":1,"vSync":true,"viewDistance":300,"sunShadows":{"enabled":true,"cascades":3,"resolution":2048,"distance":80,"filter":"pcf3x3"},"pointShadows":{"enabled":true,"resolution":512,"filterTaps":9,"poolBudgetMiB":96,"overflow":"discard"}})json");
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Unsupported point shadow overflow was accepted");
    auto invalidTaps = decodePlayerGraphics(balanced);
    invalidTaps.pointShadows.filterTaps = 4;
    rejected = false;
    try {
        encodePlayerGraphics(invalidTaps);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Non-square point filter taps were accepted");
}
void runtimeApply() {
    RenderView view;
    view.cameraView = glm::mat4(1);
    view.verticalFovDegrees = 60;
    view.cameraNear = 1;
    view.cameraFar = 500;
    applyPlayerGraphics(view, {960, 540}, decodePlayerGraphics(balanced));
    check(view.runtimeLighting.explicitSettings && view.cameraFar > view.cameraNear &&
              view.runtimeLighting.point.poolBudgetMiB == 96,
          "Runtime graphics override did not reach RenderView");
    auto nearDistance = decodePlayerGraphics(balanced);
    nearDistance.viewDistance = 1;
    applyPlayerGraphics(view, {960, 540}, nearDistance);
    check(std::isfinite(view.cameraFar) && view.cameraFar > view.cameraNear,
          "viewDistance equal to cameraNear produced an invalid projection range");
    view.cameraNear = 500000;
    view.cameraFar = 500000;
    applyPlayerGraphics(view, {960, 540}, nearDistance);
    check(std::isfinite(view.cameraFar) && view.cameraFar > view.cameraNear,
          "large cameraNear collapsed the projection range");
}
} // namespace

int main() {
    try {
        roundtrip();
        profiles();
        strictRejection();
        runtimeApply();
        std::cout << "4 passed, 0 failed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "0 passed, 1 failed: " << error.what() << '\n';
        return 1;
    }
}
