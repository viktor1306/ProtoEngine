#include "runtime/PlayerGraphics.hpp"
#include "core/Diagnostics.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <yyjson.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <set>
#include <stdexcept>
#include <sstream>
#include <unordered_set>

namespace proto {
namespace {
using Json = yyjson_val;

void fail(std::string_view message) { throw std::runtime_error(std::string(message)); }

void keys(Json* object, std::initializer_list<std::string_view> allowed) {
    if (!object || !yyjson_is_obj(object))
        fail("proto.graphics object expected");
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second || std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error("Unknown or duplicate proto.graphics field: " + std::string(name));
    }
}

Json* field(Json* object, const char* name, bool required = true) {
    auto* result = yyjson_obj_get(object, name);
    if (!result && required)
        throw std::runtime_error(std::string("Missing proto.graphics field: ") + name);
    return result;
}

std::string text(Json* value, const char* context) {
    if (!value || !yyjson_is_str(value))
        throw std::runtime_error(std::string("Expected graphics string: ") + context);
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos || !validUtf8(result))
        throw std::runtime_error(std::string("Invalid graphics UTF-8: ") + context);
    return result;
}

double number(Json* value, const char* context) {
    if (!value || !yyjson_is_num(value))
        throw std::runtime_error(std::string("Expected graphics number: ") + context);
    const double result = yyjson_get_num(value);
    if (!std::isfinite(result))
        throw std::runtime_error(std::string("Non-finite graphics number: ") + context);
    return result;
}

bool boolean(Json* value, const char* context) {
    if (!value || !yyjson_is_bool(value))
        throw std::runtime_error(std::string("Expected graphics boolean: ") + context);
    return yyjson_get_bool(value);
}

uint32_t uint32(Json* value, const char* context) {
    if (!value || !yyjson_is_uint(value) || yyjson_get_uint(value) > UINT32_MAX)
        throw std::runtime_error(std::string("Expected graphics uint32: ") + context);
    return static_cast<uint32_t>(yyjson_get_uint(value));
}

void validateResolution(uint32_t value, const char* context) {
    if (value != 256 && value != 512 && value != 1024 && value != 2048)
        throw std::runtime_error(std::string("Unsupported shadow resolution: ") + context);
}

uint32_t filterTaps(std::string_view filter) {
    if (filter == "point")
        return 1;
    if (filter == "pcf3x3")
        return 9;
    if (filter == "pcf5x5")
        return 25;
    throw std::runtime_error("Unsupported sun shadow filter: " + std::string(filter));
}

void validate(PlayerGraphics& result) {
    if (result.profile != "Low" && result.profile != "Balanced" && result.profile != "High" &&
        result.profile != "Custom")
        throw std::runtime_error("Unsupported graphics profile: " + result.profile);
    if (!std::isfinite(result.renderScale) || result.renderScale < .25f || result.renderScale > 1.f)
        fail("renderScale must be finite and in range 0.25..1");
    if (!std::isfinite(result.viewDistance) || result.viewDistance < 1.f || result.viewDistance > 10000.f)
        fail("viewDistance must be finite and in range 1..10000");
    if (result.sunShadows.cascades < 1 || result.sunShadows.cascades > 4)
        fail("sunShadows.cascades must be in range 1..4");
    validateResolution(result.sunShadows.resolution, "sunShadows");
    if (!std::isfinite(result.sunShadows.distance) || result.sunShadows.distance < 1.f ||
        result.sunShadows.distance > 10000.f)
        fail("sunShadows.distance must be finite and in range 1..10000");
    (void)filterTaps(result.sunShadows.filter);
    validateResolution(result.pointShadows.resolution, "pointShadows");
    if (result.pointShadows.filterTaps != 1 && result.pointShadows.filterTaps != 9 &&
        result.pointShadows.filterTaps != 25)
        fail("pointShadows.filterTaps must be one of 1, 9 or 25");
    const auto root = static_cast<uint64_t>(result.pointShadows.resolution) * result.pointShadows.resolution * 4u * 6u;
    if (result.pointShadows.poolBudgetMiB < 1 || result.pointShadows.poolBudgetMiB > 4096 ||
        uint64_t(result.pointShadows.poolBudgetMiB) * 1024u * 1024u < root)
        fail("pointShadows.poolBudgetMiB cannot fit one shadow cube");
    if (result.pointShadows.overflow != "multiPass")
        fail("Only multiPass point shadow overflow is supported");
}

PlayerGraphics balanced() { return {}; }

} // namespace

void validatePlayerGraphics(const PlayerGraphics& settings) {
    auto copy = settings;
    validate(copy);
}

PlayerGraphics playerGraphicsProfile(std::string_view profile) {
    PlayerGraphics result;
    result.profile = std::string(profile);
    if (profile == "Low") {
        result.renderScale = .75f;
        result.viewDistance = 200;
        result.sunShadows = {true, 2, 1024, 60, "pcf3x3"};
        result.pointShadows = {true, 256, 1, 48, "multiPass"};
        result.textureTopMipDrop = 1;
    } else if (profile == "Balanced") {
        result = balanced();
    } else if (profile == "High") {
        result.renderScale = 1;
        result.viewDistance = 500;
        result.sunShadows = {true, 4, 2048, 100, "pcf3x3"};
        result.pointShadows = {true, 1024, 9, 192, "multiPass"};
    } else if (profile == "Custom") {
        result.profile = "Custom";
    } else {
        throw std::runtime_error("Unsupported graphics profile: " + std::string(profile));
    }
    validate(result);
    return result;
}

PlayerGraphics decodePlayerGraphics(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > 1024 * 1024)
        fail("proto.graphics size is outside the supported bound");
    std::string input(bytes);
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read_opts(input.data(), input.size(), 0, nullptr, &error), yyjson_doc_free);
    if (!document)
        throw std::runtime_error("Invalid proto.graphics JSON at byte " + std::to_string(error.pos) + ": " + error.msg);
    auto* root = yyjson_doc_get_root(document.get());
    keys(root, {"format", "formatVersion", "profile", "renderScale", "vSync", "viewDistance", "sunShadows",
                "pointShadows", "textureTopMipDrop"});
    if (text(field(root, "format"), "format") != "proto.graphics" || uint32(field(root, "formatVersion"), "formatVersion") != 1)
        fail("Unsupported proto.graphics format or version");
    auto result = playerGraphicsProfile(text(field(root, "profile"), "profile"));
    result.profile = text(field(root, "profile"), "profile");
    result.renderScale = static_cast<float>(number(field(root, "renderScale"), "renderScale"));
    result.vSync = boolean(field(root, "vSync"), "vSync");
    result.viewDistance = static_cast<float>(number(field(root, "viewDistance"), "viewDistance"));
    auto* sun = field(root, "sunShadows");
    keys(sun, {"enabled", "cascades", "resolution", "distance", "filter"});
    result.sunShadows.enabled = boolean(field(sun, "enabled"), "sunShadows.enabled");
    result.sunShadows.cascades = uint32(field(sun, "cascades"), "sunShadows.cascades");
    result.sunShadows.resolution = uint32(field(sun, "resolution"), "sunShadows.resolution");
    result.sunShadows.distance = static_cast<float>(number(field(sun, "distance"), "sunShadows.distance"));
    result.sunShadows.filter = text(field(sun, "filter"), "sunShadows.filter");
    auto* point = field(root, "pointShadows");
    keys(point, {"enabled", "resolution", "filterTaps", "poolBudgetMiB", "overflow"});
    result.pointShadows.enabled = boolean(field(point, "enabled"), "pointShadows.enabled");
    result.pointShadows.resolution = uint32(field(point, "resolution"), "pointShadows.resolution");
    result.pointShadows.filterTaps = uint32(field(point, "filterTaps"), "pointShadows.filterTaps");
    result.pointShadows.poolBudgetMiB = uint32(field(point, "poolBudgetMiB"), "pointShadows.poolBudgetMiB");
    result.pointShadows.overflow = text(field(point, "overflow"), "pointShadows.overflow");
    if (auto* mip = field(root, "textureTopMipDrop", false))
        result.textureTopMipDrop = uint32(mip, "textureTopMipDrop");
    validate(result);
    return result;
}

std::string encodePlayerGraphics(const PlayerGraphics& input) {
    auto result = input;
    validate(result);
    std::ostringstream out;
    out << "{\n"
        << "  \"format\":\"proto.graphics\",\n"
        << "  \"formatVersion\":1,\n"
        << "  \"profile\":" << jsonString(result.profile) << ",\n"
        << "  \"renderScale\":" << result.renderScale << ",\n"
        << "  \"vSync\":" << (result.vSync ? "true" : "false") << ",\n"
        << "  \"viewDistance\":" << result.viewDistance << ",\n"
        << "  \"sunShadows\":{\"enabled\":" << (result.sunShadows.enabled ? "true" : "false")
        << ",\"cascades\":" << result.sunShadows.cascades << ",\"resolution\":" << result.sunShadows.resolution
        << ",\"distance\":" << result.sunShadows.distance << ",\"filter\":"
        << jsonString(result.sunShadows.filter) << "},\n"
        << "  \"pointShadows\":{\"enabled\":" << (result.pointShadows.enabled ? "true" : "false")
        << ",\"resolution\":" << result.pointShadows.resolution << ",\"filterTaps\":"
        << result.pointShadows.filterTaps << ",\"poolBudgetMiB\":" << result.pointShadows.poolBudgetMiB
        << ",\"overflow\":" << jsonString(result.pointShadows.overflow) << "},\n"
        << "  \"textureTopMipDrop\":" << result.textureTopMipDrop << "\n}\n";
    return out.str();
}

void applyPlayerGraphics(RenderView& view, VkExtent2D extent, const PlayerGraphics& settings) {
    validatePlayerGraphics(settings);
    view.runtimeLighting.explicitSettings = true;
    view.runtimeLighting.viewDistance = settings.viewDistance;
    view.runtimeLighting.sun = {settings.sunShadows.enabled, settings.sunShadows.resolution,
                                filterTaps(settings.sunShadows.filter), 0};
    view.runtimeLighting.point = {settings.pointShadows.enabled, settings.pointShadows.resolution,
                                  settings.pointShadows.filterTaps, settings.pointShadows.poolBudgetMiB};
    view.runtimeLighting.textureTopMipDrop = settings.textureTopMipDrop;
    view.lighting.shadows = settings.sunShadows.enabled || settings.pointShadows.enabled;
    view.lighting.shadowResolution = settings.sunShadows.resolution;
    view.lighting.shadowCascades = settings.sunShadows.cascades;
    view.lighting.shadowDistance = settings.sunShadows.distance;
    view.lighting.filteredShadows = true;
    if (!std::isfinite(view.cameraNear) || view.cameraNear <= 0)
        throw std::runtime_error("cameraNear must be finite and positive");
    const float minimumFar = std::nextafter(view.cameraNear, std::numeric_limits<float>::infinity());
    if (!std::isfinite(minimumFar) || !(minimumFar > view.cameraNear))
        throw std::runtime_error("cameraNear is too large for a finite projection range");
    const float authoredFar = std::isfinite(view.cameraFar) ? view.cameraFar : settings.viewDistance;
    view.cameraFar = std::max(std::min(authoredFar, settings.viewDistance), minimumFar);
    const float width = static_cast<float>(std::max(extent.width, 1u));
    const float height = static_cast<float>(std::max(extent.height, 1u));
    view.viewProjection = glm::perspectiveRH_ZO(glm::radians(view.verticalFovDegrees), width / height,
                                                 view.cameraNear, view.cameraFar) * view.cameraView;
}

} // namespace proto
