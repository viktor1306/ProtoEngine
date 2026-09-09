#include "behavior/BehaviorSchema.hpp"

#include "core/Diagnostics.hpp"
#include "scene/Scene.hpp"
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace proto {
namespace {
using Json = yyjson_val;

constexpr size_t maxBehaviorTypes = 4096;
constexpr size_t maxBehaviorProperties = 4096;
constexpr size_t maxBehaviorNameBytes = 1024;
constexpr size_t maxBehaviorStringBytes = 1024 * 1024;
constexpr size_t maxBehaviorSchemaBytes = 8 * 1024 * 1024;

bool finite(double value) {
    return std::isfinite(value);
}

void validText(std::string_view value, const char* context, size_t limit = maxBehaviorStringBytes) {
    if (value.empty() && std::string_view(context) != "behavior string")
        throw std::runtime_error(std::string(context) + " must not be empty");
    if (value.size() > limit || value.find('\0') != std::string_view::npos || !validUtf8(value))
        throw std::runtime_error(std::string(context) + " must be valid bounded UTF-8");
}

const char* propertyTypeName(sdk::PropertyType type) {
    switch (type) {
    case sdk::PropertyType::Bool:
        return "bool";
    case sdk::PropertyType::Integer:
        return "integer";
    case sdk::PropertyType::Float:
        return "float";
    case sdk::PropertyType::String:
        return "string";
    case sdk::PropertyType::Vec3:
        return "vec3";
    case sdk::PropertyType::Color:
        return "color";
    case sdk::PropertyType::EntityRef:
        return "entityRef";
    case sdk::PropertyType::AssetRef:
        return "assetRef";
    }
    throw std::runtime_error("Unknown behavior property type");
}

sdk::PropertyType parsePropertyType(std::string_view value) {
    if (value == "bool")
        return sdk::PropertyType::Bool;
    if (value == "integer")
        return sdk::PropertyType::Integer;
    if (value == "float")
        return sdk::PropertyType::Float;
    if (value == "string")
        return sdk::PropertyType::String;
    if (value == "vec3")
        return sdk::PropertyType::Vec3;
    if (value == "color")
        return sdk::PropertyType::Color;
    if (value == "entityRef")
        return sdk::PropertyType::EntityRef;
    if (value == "assetRef")
        return sdk::PropertyType::AssetRef;
    throw std::runtime_error("Unknown behavior property type: " + std::string(value));
}

bool isNumeric(sdk::PropertyType type) {
    return type == sdk::PropertyType::Integer || type == sdk::PropertyType::Float;
}

bool valueMatches(sdk::PropertyType type, const sdk::PropertyValue& value) {
    switch (type) {
    case sdk::PropertyType::Bool:
        return std::holds_alternative<bool>(value);
    case sdk::PropertyType::Integer:
        return std::holds_alternative<int64_t>(value);
    case sdk::PropertyType::Float:
        // Scene JSON may contain a JSON integer for a float property. Keep this
        // accepted at the schema boundary; normalization converts it to double.
        return std::holds_alternative<double>(value) || std::holds_alternative<int64_t>(value);
    case sdk::PropertyType::String:
        return std::holds_alternative<std::string>(value);
    case sdk::PropertyType::Vec3:
        return std::holds_alternative<sdk::Vec3>(value);
    case sdk::PropertyType::Color:
        return std::holds_alternative<sdk::Color>(value);
    case sdk::PropertyType::EntityRef:
        return std::holds_alternative<sdk::EntityRef>(value);
    case sdk::PropertyType::AssetRef:
        return std::holds_alternative<sdk::AssetRef>(value);
    }
    return false;
}

double numericValue(const sdk::PropertyValue& value) {
    if (const auto* integer = std::get_if<int64_t>(&value))
        return static_cast<double>(*integer);
    if (const auto* real = std::get_if<double>(&value))
        return *real;
    throw std::runtime_error("Behavior range requires a numeric value");
}

int64_t normalizeInteger(int64_t value, const sdk::PropertyDescriptor& property) {
    // Keep integer normalization in the integer domain. Converting through
    // double loses values above 2^53 and converting an out-of-range double to
    // int64_t is undefined, even when the descriptor range is malformed.
    constexpr double minBoundary = -9223372036854775808.0;
    constexpr double maxBoundary = 9223372036854775808.0; // one past INT64_MAX
    if (property.min && *property.min > minBoundary) {
        const auto minimum =
            *property.min >= maxBoundary ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(*property.min);
        if (value < minimum)
            value = minimum;
    }
    if (property.max && *property.max < maxBoundary) {
        const auto maximum =
            *property.max <= minBoundary ? std::numeric_limits<int64_t>::min() : static_cast<int64_t>(*property.max);
        if (value > maximum)
            value = maximum;
    }
    return value;
}

void validatePropertyDescriptor(const sdk::PropertyDescriptor& property) {
    validText(property.name, "Behavior property name", maxBehaviorNameBytes);
    validateBehaviorValue(property.defaultValue);
    if (!valueMatches(property.type, property.defaultValue))
        throw std::runtime_error("Behavior property default does not match its type: " + property.name);
    if ((property.min || property.max) && !isNumeric(property.type))
        throw std::runtime_error("Behavior property ranges require integer or float type: " + property.name);
    if (property.min && !finite(*property.min))
        throw std::runtime_error("Behavior property minimum is not finite: " + property.name);
    if (property.max && !finite(*property.max))
        throw std::runtime_error("Behavior property maximum is not finite: " + property.name);
    if (property.min && property.max && *property.min > *property.max)
        throw std::runtime_error("Behavior property range is reversed: " + property.name);
    if (property.type == sdk::PropertyType::Integer && ((property.min && std::trunc(*property.min) != *property.min) ||
                                                        (property.max && std::trunc(*property.max) != *property.max)))
        throw std::runtime_error("Integer behavior property ranges must be integral: " + property.name);
    if (isNumeric(property.type)) {
        const auto value = numericValue(property.defaultValue);
        if (property.min && value < *property.min)
            throw std::runtime_error("Behavior property default is below its minimum: " + property.name);
        if (property.max && value > *property.max)
            throw std::runtime_error("Behavior property default is above its maximum: " + property.name);
    }
    if (property.label.size() > maxBehaviorNameBytes || property.label.find('\0') != std::string::npos ||
        !validUtf8(property.label))
        throw std::runtime_error("Behavior property label must be valid bounded UTF-8: " + property.name);
}

void validateDescriptor(const sdk::BehaviorDescriptor& descriptor) {
    if (!descriptor.id)
        throw std::runtime_error("Behavior type has no UUID");
    validText(descriptor.name, "Behavior type name", maxBehaviorNameBytes);
    if (descriptor.properties.size() > maxBehaviorProperties)
        throw std::runtime_error("Behavior has too many properties");
    std::set<std::string, std::less<>> names;
    for (const auto& property : descriptor.properties) {
        if (!names.insert(property.name).second)
            throw std::runtime_error("Duplicate behavior property name: " + property.name);
        validatePropertyDescriptor(property);
    }
}

void validateSchema(const BehaviorSchema& schema) {
    if (schema.sdkBuildId.empty() || schema.sdkBuildId.size() > maxBehaviorNameBytes ||
        schema.sdkBuildId.find('\0') != std::string::npos || !validUtf8(schema.sdkBuildId))
        throw std::runtime_error("Behavior schema has invalid sdkBuildId");
    if (schema.types.size() > maxBehaviorTypes)
        throw std::runtime_error("Behavior schema has too many types");
    std::set<BehaviorTypeId> ids;
    std::set<std::string, std::less<>> names;
    for (const auto& descriptor : schema.types) {
        validateDescriptor(descriptor);
        if (!ids.insert(descriptor.id).second)
            throw std::runtime_error("Duplicate behavior type UUID");
        if (!names.insert(descriptor.name).second)
            throw std::runtime_error("Duplicate behavior type name: " + descriptor.name);
    }
}

void objectKeys(Json* value, std::initializer_list<std::string_view> allowed, const char* context) {
    if (!value || !yyjson_is_obj(value))
        throw std::runtime_error(std::string(context) + ": expected JSON object");
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second || std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error(std::string(context) + ": unsupported or duplicate field");
    }
}

Json* required(Json* object, const char* name, const char* context) {
    auto* value = yyjson_obj_get(object, name);
    if (!value)
        throw std::runtime_error(std::string(context) + ": missing field " + name);
    return value;
}

std::string jsonText(Json* value, const char* context, bool allowEmpty = false) {
    if (!value || !yyjson_is_str(value))
        throw std::runtime_error(std::string(context) + ": expected string");
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (!allowEmpty && result.empty())
        throw std::runtime_error(std::string(context) + ": must not be empty");
    validText(result, allowEmpty ? "behavior string" : context);
    return result;
}

double jsonNumber(Json* value, const char* context) {
    if (!value || !yyjson_is_num(value))
        throw std::runtime_error(std::string(context) + ": expected number");
    const double result = yyjson_get_num(value);
    if (!finite(result))
        throw std::runtime_error(std::string(context) + ": number is not finite");
    return result;
}

template <class T> T jsonUuid(Json* value, const char* context) {
    if (!value || yyjson_is_null(value))
        return {};
    return T::parse(jsonText(value, context));
}

yyjson_mut_val* encodeValue(yyjson_mut_doc* document, const sdk::PropertyValue& value);

sdk::PropertyValue decodeValue(Json* value) {
    if (!value)
        throw std::runtime_error("Behavior property value is missing");
    if (yyjson_is_bool(value))
        return yyjson_get_bool(value);
    if (yyjson_is_sint(value))
        return yyjson_get_sint(value);
    if (yyjson_is_uint(value)) {
        const auto integer = yyjson_get_uint(value);
        if (integer > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("Behavior integer exceeds signed int64 range");
        return static_cast<int64_t>(integer);
    }
    if (yyjson_is_real(value))
        return jsonNumber(value, "Behavior property value");
    if (yyjson_is_str(value))
        return jsonText(value, "Behavior string", true);
    if (yyjson_is_arr(value)) {
        const auto size = yyjson_arr_size(value);
        if (size != 3 && size != 4)
            throw std::runtime_error("Behavior vector must contain three or four finite numbers");
        if (size == 3)
            return sdk::Vec3{static_cast<float>(jsonNumber(yyjson_arr_get(value, 0), "Behavior Vec3")),
                             static_cast<float>(jsonNumber(yyjson_arr_get(value, 1), "Behavior Vec3")),
                             static_cast<float>(jsonNumber(yyjson_arr_get(value, 2), "Behavior Vec3"))};
        return sdk::Color{static_cast<float>(jsonNumber(yyjson_arr_get(value, 0), "Behavior Color")),
                          static_cast<float>(jsonNumber(yyjson_arr_get(value, 1), "Behavior Color")),
                          static_cast<float>(jsonNumber(yyjson_arr_get(value, 2), "Behavior Color")),
                          static_cast<float>(jsonNumber(yyjson_arr_get(value, 3), "Behavior Color"))};
    }
    if (yyjson_is_obj(value)) {
        std::unordered_set<std::string_view> names;
        yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
        auto* key = yyjson_obj_iter_next(&iterator);
        if (!key || yyjson_obj_iter_next(&iterator))
            throw std::runtime_error("Behavior reference wrapper must contain exactly one field");
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!names.insert(name).second)
            throw std::runtime_error("Duplicate behavior reference field");
        auto* ref = yyjson_obj_iter_get_val(key);
        if (name == "entityRef")
            return sdk::EntityRef{jsonUuid<EntityId>(ref, "Behavior entityRef")};
        if (name == "assetRef")
            return sdk::AssetRef{jsonUuid<AssetId>(ref, "Behavior assetRef")};
        throw std::runtime_error("Unknown behavior reference wrapper");
    }
    throw std::runtime_error("Unsupported behavior property value");
}

yyjson_mut_val* encodeValue(yyjson_mut_doc* document, const sdk::PropertyValue& value) {
    return std::visit(
        [document](const auto& item) -> yyjson_mut_val* {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return yyjson_mut_bool(document, item);
            else if constexpr (std::is_same_v<T, int64_t>)
                return yyjson_mut_sint(document, item);
            else if constexpr (std::is_same_v<T, double>) {
                if (!finite(item))
                    throw std::runtime_error("Behavior property number is not finite");
                return yyjson_mut_real(document, item);
            } else if constexpr (std::is_same_v<T, std::string>)
                return yyjson_mut_strncpy(document, item.data(), item.size());
            else if constexpr (std::is_same_v<T, sdk::Vec3>) {
                auto* array = yyjson_mut_arr(document);
                yyjson_mut_arr_add_real(document, array, item.x);
                yyjson_mut_arr_add_real(document, array, item.y);
                yyjson_mut_arr_add_real(document, array, item.z);
                return array;
            } else if constexpr (std::is_same_v<T, sdk::Color>) {
                auto* array = yyjson_mut_arr(document);
                yyjson_mut_arr_add_real(document, array, item.r);
                yyjson_mut_arr_add_real(document, array, item.g);
                yyjson_mut_arr_add_real(document, array, item.b);
                yyjson_mut_arr_add_real(document, array, item.a);
                return array;
            } else if constexpr (std::is_same_v<T, sdk::EntityRef>) {
                auto* object = yyjson_mut_obj(document);
                if (item.id)
                    yyjson_mut_obj_add_strcpy(document, object, "entityRef", item.id.string().c_str());
                else
                    yyjson_mut_obj_add_null(document, object, "entityRef");
                return object;
            } else if constexpr (std::is_same_v<T, sdk::AssetRef>) {
                auto* object = yyjson_mut_obj(document);
                if (item.id)
                    yyjson_mut_obj_add_strcpy(document, object, "assetRef", item.id.string().c_str());
                else
                    yyjson_mut_obj_add_null(document, object, "assetRef");
                return object;
            }
        },
        value);
}

void appendProperty(yyjson_mut_doc* document, yyjson_mut_val* object, const sdk::PropertyDescriptor& property) {
    yyjson_mut_obj_add_strcpy(document, object, "name", property.name.c_str());
    yyjson_mut_obj_add_strcpy(document, object, "type", propertyTypeName(property.type));
    yyjson_mut_obj_add_val(document, object, "default", encodeValue(document, property.defaultValue));
    if (property.min)
        yyjson_mut_obj_add_real(document, object, "min", *property.min);
    if (property.max)
        yyjson_mut_obj_add_real(document, object, "max", *property.max);
    if (!property.label.empty())
        yyjson_mut_obj_add_strcpy(document, object, "label", property.label.c_str());
}

sdk::PropertyDescriptor decodeProperty(Json* value) {
    objectKeys(value, {"name", "type", "default", "min", "max", "label"}, "Behavior property");
    sdk::PropertyDescriptor result;
    result.name = jsonText(required(value, "name", "Behavior property"), "Behavior property name");
    result.type = parsePropertyType(jsonText(required(value, "type", "Behavior property"), "Behavior property type"));
    result.defaultValue = decodeValue(required(value, "default", "Behavior property"));
    if (auto* minimum = yyjson_obj_get(value, "min"))
        result.min = jsonNumber(minimum, "Behavior property min");
    if (auto* maximum = yyjson_obj_get(value, "max"))
        result.max = jsonNumber(maximum, "Behavior property max");
    if (auto* label = yyjson_obj_get(value, "label"))
        result.label = jsonText(label, "Behavior property label", true);
    validatePropertyDescriptor(result);
    return result;
}

sdk::BehaviorDescriptor decodeDescriptor(Json* value) {
    objectKeys(value, {"id", "name", "properties"}, "Behavior descriptor");
    sdk::BehaviorDescriptor result;
    result.id = BehaviorTypeId::parse(jsonText(required(value, "id", "Behavior descriptor"), "Behavior type ID"));
    result.name = jsonText(required(value, "name", "Behavior descriptor"), "Behavior type name");
    auto* properties = required(value, "properties", "Behavior descriptor");
    if (!yyjson_is_arr(properties) || yyjson_arr_size(properties) > maxBehaviorProperties)
        throw std::runtime_error("Behavior descriptor properties must be a bounded array");
    size_t index{}, count{};
    Json* property{};
    yyjson_arr_foreach(properties, index, count, property) result.properties.push_back(decodeProperty(property));
    validateDescriptor(result);
    return result;
}

void ensureValueType(const sdk::PropertyDescriptor& descriptor, const sdk::PropertyValue& value) {
    if (!valueMatches(descriptor.type, value))
        throw std::runtime_error("Behavior property type mismatch: " + descriptor.name);
}

bool assetExists(const Scene& scene, AssetId id) {
    if (id == builtin::cube || id == builtin::plane)
        return true;
    return scene.assets && (scene.assets->models.contains(id) || scene.assets->meshes.contains(id) ||
                            scene.assets->materials.contains(id) || scene.assets->textures.contains(id) ||
                            scene.assets->dataFiles.contains(id));
}

} // namespace

void sdk::BehaviorRegistry::Add(BehaviorDescriptor descriptor, Factory factory) {
    if (!factory)
        throw std::runtime_error("Behavior registration requires a factory");
    validateDescriptor(descriptor);
    if (Find(descriptor.id))
        throw std::runtime_error("Duplicate behavior type UUID");
    for (const auto& entry : entries_)
        if (entry.descriptor.name == descriptor.name)
            throw std::runtime_error("Duplicate behavior type name: " + descriptor.name);
    entries_.push_back({std::move(descriptor), factory});
}

const sdk::BehaviorRegistry::Entry* sdk::BehaviorRegistry::Find(BehaviorTypeId id) const {
    for (const auto& entry : entries_)
        if (entry.descriptor.id == id)
            return &entry;
    return nullptr;
}

const sdk::BehaviorDescriptor* BehaviorSchema::find(BehaviorTypeId id) const {
    for (const auto& descriptor : types)
        if (descriptor.id == id)
            return &descriptor;
    return nullptr;
}

BehaviorSchema describeRegistry(const sdk::BehaviorRegistry& registry, std::string buildId) {
    BehaviorSchema result;
    result.sdkBuildId = std::move(buildId);
    for (const auto& entry : registry.Entries())
        result.types.push_back(entry.descriptor);
    validateSchema(result);
    return result;
}

std::string encodeBehaviorSchema(const BehaviorSchema& schema) {
    validateSchema(schema);
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owner(yyjson_mut_doc_new(nullptr),
                                                                          yyjson_mut_doc_free);
    if (!owner)
        throw std::bad_alloc();
    auto* document = owner.get();
    auto* root = yyjson_mut_obj(document);
    yyjson_mut_doc_set_root(document, root);
    yyjson_mut_obj_add_strcpy(document, root, "format", "proto.behaviors");
    yyjson_mut_obj_add_uint(document, root, "formatVersion", 1);
    yyjson_mut_obj_add_uint(document, root, "behaviorApiVersion", sdk::behaviorApiVersion);
    yyjson_mut_obj_add_strcpy(document, root, "sdkBuildId", schema.sdkBuildId.c_str());
    auto* types = yyjson_mut_arr(document);
    yyjson_mut_obj_add_val(document, root, "types", types);
    for (const auto& descriptor : schema.types) {
        auto* type = yyjson_mut_obj(document);
        yyjson_mut_arr_append(types, type);
        yyjson_mut_obj_add_strcpy(document, type, "id", descriptor.id.string().c_str());
        yyjson_mut_obj_add_strcpy(document, type, "name", descriptor.name.c_str());
        auto* properties = yyjson_mut_arr(document);
        yyjson_mut_obj_add_val(document, type, "properties", properties);
        for (const auto& property : descriptor.properties) {
            auto* encoded = yyjson_mut_obj(document);
            yyjson_mut_arr_append(properties, encoded);
            appendProperty(document, encoded, property);
        }
    }
    size_t length{};
    std::unique_ptr<char, decltype(&free)> bytes(
        yyjson_mut_write(document, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length), free);
    if (!bytes || length > maxBehaviorSchemaBytes)
        throw std::runtime_error("Behavior schema exceeds size limit");
    return {bytes.get(), length};
}

BehaviorSchema decodeBehaviorSchema(std::string_view json, std::string_view expectedBuildId) {
    if (json.empty() || json.size() > maxBehaviorSchemaBytes)
        throw std::runtime_error("Behavior schema has invalid size");
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &error), yyjson_doc_free);
    if (!document)
        throw std::runtime_error("Invalid behavior schema JSON at byte " + std::to_string(error.pos) + ": " +
                                 error.msg);
    auto* root = yyjson_doc_get_root(document.get());
    objectKeys(root, {"format", "formatVersion", "behaviorApiVersion", "sdkBuildId", "types"}, "Behavior schema");
    if (jsonText(required(root, "format", "Behavior schema"), "Behavior schema format") != "proto.behaviors")
        throw std::runtime_error("Not a proto.behaviors schema");
    auto* version = required(root, "formatVersion", "Behavior schema");
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1)
        throw std::runtime_error("Unsupported behavior schema formatVersion");
    auto* api = required(root, "behaviorApiVersion", "Behavior schema");
    if (!yyjson_is_uint(api) || yyjson_get_uint(api) != sdk::behaviorApiVersion)
        throw std::runtime_error("Unsupported behavior API version");
    BehaviorSchema result;
    result.sdkBuildId = jsonText(required(root, "sdkBuildId", "Behavior schema"), "Behavior schema sdkBuildId");
    if (!expectedBuildId.empty() && result.sdkBuildId != expectedBuildId)
        throw std::runtime_error("Behavior schema sdkBuildId does not match this build");
    auto* types = required(root, "types", "Behavior schema");
    if (!yyjson_is_arr(types) || yyjson_arr_size(types) > maxBehaviorTypes)
        throw std::runtime_error("Behavior schema types must be a bounded array");
    size_t index{}, count{};
    Json* type{};
    yyjson_arr_foreach(types, index, count, type) result.types.push_back(decodeDescriptor(type));
    validateSchema(result);
    return result;
}

sdk::PropertyMap normalizeBehaviorProperties(const sdk::BehaviorDescriptor& descriptor,
                                             const sdk::PropertyMap& values) {
    validateDescriptor(descriptor);
    sdk::PropertyMap result;
    for (const auto& [name, value] : values) {
        validText(name, "Behavior property name", maxBehaviorNameBytes);
        validateBehaviorValue(value);
        result.emplace(name, value);
    }
    for (const auto& property : descriptor.properties) {
        auto it = result.find(property.name);
        if (it == result.end()) {
            auto defaultValue = property.defaultValue;
            if (property.type == sdk::PropertyType::Float && std::holds_alternative<int64_t>(defaultValue))
                defaultValue = static_cast<double>(std::get<int64_t>(defaultValue));
            result.emplace(property.name, std::move(defaultValue));
            continue;
        }
        if (!valueMatches(property.type, it->second))
            continue; // Preserve incompatible data for diagnostics and roundtrip.
        if (property.type == sdk::PropertyType::Float && std::holds_alternative<int64_t>(it->second))
            it->second = static_cast<double>(std::get<int64_t>(it->second));
        if (property.type == sdk::PropertyType::Integer) {
            it->second = normalizeInteger(std::get<int64_t>(it->second), property);
        } else if (property.type == sdk::PropertyType::Float) {
            auto number = numericValue(it->second);
            if (property.min)
                number = std::max(number, *property.min);
            if (property.max)
                number = std::min(number, *property.max);
            it->second = number;
        }
    }
    return result;
}

void validateBehaviorValue(const sdk::PropertyValue& value) {
    std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, double>) {
                if (!finite(item))
                    throw std::runtime_error("Behavior number must be finite");
            } else if constexpr (std::is_same_v<T, std::string>) {
                validText(item, "behavior string");
            } else if constexpr (std::is_same_v<T, sdk::Vec3>) {
                if (!finite(item.x) || !finite(item.y) || !finite(item.z))
                    throw std::runtime_error("Behavior Vec3 must be finite");
            } else if constexpr (std::is_same_v<T, sdk::Color>) {
                if (!finite(item.r) || !finite(item.g) || !finite(item.b) || !finite(item.a))
                    throw std::runtime_error("Behavior Color must be finite");
            }
        },
        value);
}

void validateBehaviorBinding(const sdk::BehaviorBinding& binding) {
    if (!binding.id)
        throw std::runtime_error("Behavior binding has no UUID");
    if (!binding.type)
        throw std::runtime_error("Behavior binding has no type UUID");
    if (binding.properties.size() > maxBehaviorProperties)
        throw std::runtime_error("Behavior binding has too many properties");
    for (const auto& [name, value] : binding.properties) {
        validText(name, "Behavior property name", maxBehaviorNameBytes);
        validateBehaviorValue(value);
    }
}

void validateSceneBehaviors(const Scene& scene, const BehaviorSchema& schema) {
    validateSchema(schema);
    std::set<BehaviorBindingId> bindingIds;
    for (const auto handle : scene.entities()) {
        const auto& entity = scene.entity(handle);
        for (const auto& binding : scene.behaviors(handle)) {
            validateBehaviorBinding(binding);
            if (!bindingIds.insert(binding.id).second)
                throw std::runtime_error("Duplicate behavior binding UUID in scene");
            const auto* descriptor = schema.find(binding.type);
            if (!descriptor)
                throw std::runtime_error("Unknown behavior type UUID: " + binding.type.string());
            const auto effectiveProperties = normalizeBehaviorProperties(*descriptor, binding.properties);
            std::unordered_set<std::string_view> known;
            for (const auto& property : descriptor->properties)
                known.insert(property.name);
            for (const auto& [name, value] : effectiveProperties) {
                if (!known.contains(name))
                    throw std::runtime_error("Unknown behavior property: " + name);
                const auto& descriptorProperty =
                    *std::find_if(descriptor->properties.begin(), descriptor->properties.end(),
                                  [&](const auto& property) { return property.name == name; });
                ensureValueType(descriptorProperty, value);
                if (const auto* reference = std::get_if<sdk::EntityRef>(&value);
                    reference && reference->id && !scene.find(reference->id))
                    throw std::runtime_error("Behavior references a missing entity");
                if (const auto* reference = std::get_if<sdk::AssetRef>(&value);
                    reference && reference->id && !assetExists(scene, reference->id))
                    throw std::runtime_error("Behavior references a missing asset");
            }
            (void)entity;
        }
    }
}

size_t behaviorBytes(const sdk::BehaviorBinding& binding) {
    size_t total = sizeof(binding) + binding.properties.size() * sizeof(std::pair<std::string, sdk::PropertyValue>);
    for (const auto& [name, value] : binding.properties) {
        total += name.size();
        std::visit(
            [&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::string>)
                    total += item.size();
            },
            value);
    }
    return total;
}

} // namespace proto
