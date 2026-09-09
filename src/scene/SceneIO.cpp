#include "scene/SceneIO.hpp"
#include "behavior/BehaviorSchema.hpp"
#include "core/Diagnostics.hpp"
#include <yyjson.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <windows.h>
#include <cmath>
#include <fstream>
#include <memory>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace proto {
namespace {
constexpr size_t maxDocumentBytes = 32 * 1024 * 1024;
using Value = yyjson_val;
Value* field(Value* object, const char* name) {
    auto* result = yyjson_obj_get(object, name);
    if (!result)
        throw std::runtime_error(std::string("Відсутнє поле: ") + name);
    return result;
}
void keys(Value* value, std::initializer_list<std::string_view> allowed) {
    if (!yyjson_is_obj(value))
        throw std::runtime_error("Очікувався JSON object");
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter it = yyjson_obj_iter_with(value);
    while (auto* key = yyjson_obj_iter_next(&it)) {
        std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second)
            throw std::runtime_error("Дублікат JSON-поля");
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error("Непідтримуване поле M1: " + std::string(name));
    }
}
std::string text(Value* value) {
    if (!yyjson_is_str(value))
        throw std::runtime_error("Очікувався JSON string");
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos)
        throw std::runtime_error("NUL у текстовому полі");
    return result;
}
float number(Value* value) {
    if (!yyjson_is_num(value))
        throw std::runtime_error("Очікувалося число");
    const double n = yyjson_get_num(value);
    const auto result = static_cast<float>(n);
    if (!std::isfinite(n) || !std::isfinite(result))
        throw std::runtime_error("Число поза скінченним діапазоном float");
    return result;
}
bool boolean(Value* value) {
    if (!yyjson_is_bool(value))
        throw std::runtime_error("Очікувався JSON boolean");
    return yyjson_get_bool(value);
}
uint32_t uint32(Value* value) {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > UINT32_MAX)
        throw std::runtime_error("Очікувалося невід’ємне ціле число uint32");
    return static_cast<uint32_t>(yyjson_get_uint(value));
}
template <int N> glm::vec<N, float> vector(Value* value) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) != N)
        throw std::runtime_error("Некоректний розмір вектора");
    glm::vec<N, float> result;
    for (int i = 0; i < N; ++i)
        result[i] = number(yyjson_arr_get(value, static_cast<size_t>(i)));
    return result;
}
EntityId reference(Value* value) {
    return yyjson_is_null(value) ? EntityId{} : EntityId::parse(text(value));
}
DirectionalLight directionalLight(Value* value) {
    keys(value, {"color", "intensity", "shadows"});
    DirectionalLight result;
    result.color = vector<3>(field(value, "color"));
    result.intensity = number(field(value, "intensity"));
    result.shadows = boolean(field(value, "shadows"));
    return result;
}
PointLight pointLight(Value* value) {
    keys(value, {"color", "intensity", "radius", "shadows"});
    PointLight result;
    result.color = vector<3>(field(value, "color"));
    result.intensity = number(field(value, "intensity"));
    result.radius = number(field(value, "radius"));
    result.shadows = boolean(field(value, "shadows"));
    return result;
}
double behaviorNumber(Value* value) {
    if (!yyjson_is_num(value))
        throw std::runtime_error("Behavior property expected a number");
    const auto result = yyjson_get_num(value);
    if (!std::isfinite(result))
        throw std::runtime_error("Behavior property number must be finite");
    return result;
}
std::string behaviorText(Value* value, const char* context, bool allowEmpty = false) {
    if (!yyjson_is_str(value))
        throw std::runtime_error(std::string(context) + " expected a string");
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if ((!allowEmpty && result.empty()) || result.size() > 1024 * 1024 || result.find('\0') != std::string::npos ||
        !validUtf8(result))
        throw std::runtime_error(std::string(context) + " must be bounded valid UTF-8");
    return result;
}
template <int N> std::array<float, N> behaviorVector(Value* value, const char* context) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) != N)
        throw std::runtime_error(std::string(context) + " has an invalid vector size");
    std::array<float, N> result{};
    for (int i = 0; i < N; ++i) {
        const auto number = behaviorNumber(yyjson_arr_get(value, static_cast<size_t>(i)));
        result[i] = static_cast<float>(number);
        if (!std::isfinite(result[i]))
            throw std::runtime_error(std::string(context) + " contains a non-finite float");
    }
    return result;
}
sdk::PropertyValue behaviorValue(Value* value) {
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
        return behaviorNumber(value);
    if (yyjson_is_str(value))
        return behaviorText(value, "Behavior string", true);
    if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) == 3) {
            const auto vector = behaviorVector<3>(value, "Behavior Vec3");
            return sdk::Vec3{vector[0], vector[1], vector[2]};
        }
        if (yyjson_arr_size(value) == 4) {
            const auto vector = behaviorVector<4>(value, "Behavior Color");
            return sdk::Color{vector[0], vector[1], vector[2], vector[3]};
        }
        throw std::runtime_error("Behavior vectors have three or four elements");
    }
    if (yyjson_is_obj(value)) {
        yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
        auto* key = yyjson_obj_iter_next(&iterator);
        if (!key || yyjson_obj_iter_next(&iterator))
            throw std::runtime_error("Behavior reference wrapper must contain exactly one field");
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        auto* wrapped = yyjson_obj_iter_get_val(key);
        if (name == "entityRef")
            return sdk::EntityRef{yyjson_is_null(wrapped) ? EntityId{}
                                                          : EntityId::parse(behaviorText(wrapped, "entityRef"))};
        if (name == "assetRef")
            return sdk::AssetRef{yyjson_is_null(wrapped) ? AssetId{}
                                                         : AssetId::parse(behaviorText(wrapped, "assetRef"))};
        throw std::runtime_error("Unknown behavior reference wrapper");
    }
    throw std::runtime_error("Unsupported behavior property value");
}
void behaviorObjectKeys(Value* value) {
    if (!yyjson_is_obj(value))
        throw std::runtime_error("Behavior properties expected an object");
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second || name.empty() || name.size() > 1024 ||
            name.find('\0') != std::string_view::npos || !validUtf8(name))
            throw std::runtime_error("Invalid or duplicate behavior property name");
    }
}
std::vector<sdk::BehaviorBinding> behaviorBindings(Value* value) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 4096)
        throw std::runtime_error("Behavior bindings must be a bounded array");
    std::vector<sdk::BehaviorBinding> result;
    size_t index{}, count{};
    Value* item{};
    yyjson_arr_foreach(value, index, count, item) {
        keys(item, {"id", "type", "enabled", "properties"});
        sdk::BehaviorBinding binding;
        binding.id = BehaviorBindingId::parse(text(field(item, "id")));
        binding.type = BehaviorTypeId::parse(text(field(item, "type")));
        binding.enabled = boolean(field(item, "enabled"));
        auto* properties = field(item, "properties");
        behaviorObjectKeys(properties);
        yyjson_obj_iter iterator = yyjson_obj_iter_with(properties);
        while (auto* key = yyjson_obj_iter_next(&iterator)) {
            const std::string name(yyjson_get_str(key), yyjson_get_len(key));
            binding.properties.emplace(name, behaviorValue(yyjson_obj_iter_get_val(key)));
        }
        validateBehaviorBinding(binding);
        result.push_back(std::move(binding));
    }
    return result;
}
yyjson_mut_val* encodeBehaviorValue(yyjson_mut_doc* doc, const sdk::PropertyValue& value) {
    validateBehaviorValue(value);
    return std::visit(
        [doc](const auto& item) -> yyjson_mut_val* {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return yyjson_mut_bool(doc, item);
            else if constexpr (std::is_same_v<T, int64_t>)
                return yyjson_mut_sint(doc, item);
            else if constexpr (std::is_same_v<T, double>)
                return yyjson_mut_real(doc, item);
            else if constexpr (std::is_same_v<T, std::string>)
                return yyjson_mut_strncpy(doc, item.data(), item.size());
            else if constexpr (std::is_same_v<T, sdk::Vec3>) {
                auto* array = yyjson_mut_arr(doc);
                yyjson_mut_arr_add_real(doc, array, item.x);
                yyjson_mut_arr_add_real(doc, array, item.y);
                yyjson_mut_arr_add_real(doc, array, item.z);
                return array;
            } else if constexpr (std::is_same_v<T, sdk::Color>) {
                auto* array = yyjson_mut_arr(doc);
                yyjson_mut_arr_add_real(doc, array, item.r);
                yyjson_mut_arr_add_real(doc, array, item.g);
                yyjson_mut_arr_add_real(doc, array, item.b);
                yyjson_mut_arr_add_real(doc, array, item.a);
                return array;
            } else if constexpr (std::is_same_v<T, sdk::EntityRef>) {
                auto* object = yyjson_mut_obj(doc);
                if (item.id)
                    yyjson_mut_obj_add_strcpy(doc, object, "entityRef", item.id.string().c_str());
                else
                    yyjson_mut_obj_add_null(doc, object, "entityRef");
                return object;
            } else if constexpr (std::is_same_v<T, sdk::AssetRef>) {
                auto* object = yyjson_mut_obj(doc);
                if (item.id)
                    yyjson_mut_obj_add_strcpy(doc, object, "assetRef", item.id.string().c_str());
                else
                    yyjson_mut_obj_add_null(doc, object, "assetRef");
                return object;
            }
        },
        value);
}
void encodeBehaviors(yyjson_mut_doc* doc, yyjson_mut_val* entity, const std::vector<sdk::BehaviorBinding>& values) {
    auto* array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, entity, "behaviors", array);
    for (const auto& binding : values) {
        validateBehaviorBinding(binding);
        auto* object = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(array, object);
        yyjson_mut_obj_add_strcpy(doc, object, "id", binding.id.string().c_str());
        yyjson_mut_obj_add_strcpy(doc, object, "type", binding.type.string().c_str());
        yyjson_mut_obj_add_bool(doc, object, "enabled", binding.enabled);
        auto* properties = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, object, "properties", properties);
        for (const auto& [name, value] : binding.properties)
            yyjson_mut_obj_add_val(doc, properties, name.c_str(), encodeBehaviorValue(doc, value));
    }
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void close() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
        }
    }
};
void fail(const char* action) {
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ')');
}
void writeFlushed(const std::filesystem::path& path, std::string_view bytes) {
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        fail("Cannot create staged document");
    size_t at{};
    while (at < bytes.size()) {
        DWORD written{};
        const auto chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 1024 * 1024));
        if (!WriteFile(file.value, bytes.data() + at, chunk, &written, nullptr) || !written)
            fail("Cannot write staged document");
        at += written;
    }
    if (!FlushFileBuffers(file.value))
        fail("Cannot flush staged document");
}
} // namespace
std::string readDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("Не вдалося відкрити файл: " + utf8(path.wstring()));
    const auto length = stream.tellg();
    if (length < 0 || length > static_cast<std::streamoff>(maxDocumentBytes))
        throw std::runtime_error("Файл не читається або перевищує 32 MiB");
    std::string bytes(static_cast<size_t>(length), '\0');
    stream.seekg(0);
    stream.read(bytes.data(), length);
    if (!stream)
        throw std::runtime_error("Не вдалося повністю прочитати документ");
    return bytes;
}
Scene decodeScene(std::string_view bytes, std::shared_ptr<AssetCatalog> assets) {
    if (bytes.empty() || bytes.size() > maxDocumentBytes)
        throw std::runtime_error("Некоректний розмір сцени");
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(
        yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &error), yyjson_doc_free);
    if (!doc)
        throw std::runtime_error("Пошкоджений JSON, byte " + std::to_string(error.pos) + ": " + error.msg);
    auto* root = yyjson_doc_get_root(doc.get());
    keys(root, {"format", "formatVersion", "sceneId", "name", "activeCamera", "entities", "assetRoot", "modelSources",
                "lighting"});
    if (text(field(root, "format")) != "proto.scene")
        throw std::runtime_error("Це не файл proto.scene");
    auto* version = field(root, "formatVersion");
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1)
        throw std::runtime_error("Непідтримувана версія сцени; файл не буде перезаписано");
    SceneSnapshot snapshot;
    snapshot.id = AssetId::parse(text(field(root, "sceneId")));
    snapshot.name = text(field(root, "name"));
    snapshot.activeCamera = reference(field(root, "activeCamera"));
    if (auto* value = yyjson_obj_get(root, "assetRoot"))
        snapshot.assetRoot = text(value);
    if (auto* value = yyjson_obj_get(root, "modelSources")) {
        if (!yyjson_is_arr(value))
            throw std::runtime_error("Expected model source array");
        size_t i, n;
        Value* v;
        yyjson_arr_foreach(value, i, n, v) snapshot.modelSources.push_back(AssetId::parse(text(v)));
    }
    if (auto* lighting = yyjson_obj_get(root, "lighting")) {
        keys(lighting,
             {"shadowResolution", "shadowCascades", "shadowPoolMiB", "shadowDistance", "depthBias", "normalBias",
              "ambient", "shadows", "filteredShadows", "referenceLighting", "forceShadowRefresh"});
        snapshot.lighting.shadowResolution = uint32(field(lighting, "shadowResolution"));
        snapshot.lighting.shadowCascades = uint32(field(lighting, "shadowCascades"));
        snapshot.lighting.shadowPoolMiB = uint32(field(lighting, "shadowPoolMiB"));
        snapshot.lighting.shadowDistance = number(field(lighting, "shadowDistance"));
        snapshot.lighting.depthBias = number(field(lighting, "depthBias"));
        snapshot.lighting.normalBias = number(field(lighting, "normalBias"));
        snapshot.lighting.ambient = number(field(lighting, "ambient"));
        snapshot.lighting.shadows = boolean(field(lighting, "shadows"));
        snapshot.lighting.filteredShadows = boolean(field(lighting, "filteredShadows"));
        snapshot.lighting.referenceLighting = boolean(field(lighting, "referenceLighting"));
        snapshot.lighting.forceShadowRefresh = boolean(field(lighting, "forceShadowRefresh"));
    }
    auto* entities = field(root, "entities");
    if (!yyjson_is_arr(entities) || yyjson_arr_size(entities) > 100000)
        throw std::runtime_error("Некоректний масив об’єктів або перевищено ліміт M1");
    size_t index{}, count{};
    Value* value{};
    yyjson_arr_foreach(entities, index, count, value) {
        keys(value, {"id", "name", "parent", "enabled", "transform", "components", "behaviors"});
        EntityRecord r;
        r.id = EntityId::parse(text(field(value, "id")));
        r.name = text(field(value, "name"));
        r.parent = reference(field(value, "parent"));
        r.enabled = boolean(field(value, "enabled"));
        auto* transform = field(value, "transform");
        keys(transform, {"position", "rotation", "scale"});
        r.transform.position = vector<3>(field(transform, "position"));
        r.transform.scale = vector<3>(field(transform, "scale"));
        const auto q = vector<4>(field(transform, "rotation"));
        r.transform.rotation = {q.w, q.x, q.y, q.z};
        auto* components = field(value, "components");
        keys(components, {"meshRenderer", "camera", "directionalLight", "pointLight"});
        if (auto* mesh = yyjson_obj_get(components, "meshRenderer")) {
            keys(mesh, {"mesh", "materials", "debugColor", "castShadows", "receiveShadows"});
            MeshRenderer m;
            m.mesh = AssetId::parse(text(field(mesh, "mesh")));
            auto* slots = field(mesh, "materials");
            if (!yyjson_is_arr(slots))
                throw std::runtime_error("Expected material slots");
            size_t i, n;
            Value* v;
            yyjson_arr_foreach(slots, i, n, v) m.materials.push_back(AssetId::parse(text(v)));
            if (auto* color = yyjson_obj_get(mesh, "debugColor"))
                m.color = vector<4>(color);
            m.castShadows = boolean(field(mesh, "castShadows"));
            m.receiveShadows = boolean(field(mesh, "receiveShadows"));
            r.mesh = m;
        }
        if (auto* camera = yyjson_obj_get(components, "camera")) {
            keys(camera, {"projection", "verticalFovDegrees", "near", "far", "exposure"});
            if (text(field(camera, "projection")) != "perspective")
                throw std::runtime_error("M1 підтримує perspective camera");
            r.camera = Camera{number(field(camera, "verticalFovDegrees")), number(field(camera, "near")),
                              number(field(camera, "far")), number(field(camera, "exposure"))};
        }
        if (auto* light = yyjson_obj_get(components, "directionalLight"))
            r.directionalLight = directionalLight(light);
        if (auto* light = yyjson_obj_get(components, "pointLight"))
            r.pointLight = pointLight(light);
        r.behaviors = behaviorBindings(field(value, "behaviors"));
        snapshot.entities.push_back(std::move(r));
    }
    return Scene::fromSnapshot(snapshot, std::move(assets));
}
std::string encodeScene(const Scene& scene) {
    const auto snapshot = scene.snapshot();
    auto validated = Scene::fromSnapshot(snapshot, scene.assets); // Validate references before any disk write.
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owner(yyjson_mut_doc_new(nullptr),
                                                                          yyjson_mut_doc_free);
    if (!owner)
        throw std::bad_alloc();
    auto* doc = owner.get();
    auto* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    const auto string = [&](yyjson_mut_val* object, const char* key, const std::string& value) {
        yyjson_mut_obj_add_strcpy(doc, object, key, value.c_str());
    };
    const auto ref = [&](yyjson_mut_val* object, const char* key, EntityId id) {
        if (id)
            string(object, key, id.string());
        else
            yyjson_mut_obj_add_null(doc, object, key);
    };
    const auto vec = [&](yyjson_mut_val* object, const char* key, auto value) {
        auto* array = yyjson_mut_arr(doc);
        for (glm::length_t i = 0; i < value.length(); ++i)
            yyjson_mut_arr_add_real(doc, array, value[i]);
        yyjson_mut_obj_add_val(doc, object, key, array);
    };
    string(root, "format", "proto.scene");
    yyjson_mut_obj_add_uint(doc, root, "formatVersion", 1);
    string(root, "sceneId", snapshot.id.string());
    string(root, "name", snapshot.name);
    ref(root, "activeCamera", snapshot.activeCamera);
    if (snapshot.lighting != LightingSettings{}) {
        auto* lighting = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "lighting", lighting);
        yyjson_mut_obj_add_uint(doc, lighting, "shadowResolution", snapshot.lighting.shadowResolution);
        yyjson_mut_obj_add_uint(doc, lighting, "shadowCascades", snapshot.lighting.shadowCascades);
        yyjson_mut_obj_add_uint(doc, lighting, "shadowPoolMiB", snapshot.lighting.shadowPoolMiB);
        yyjson_mut_obj_add_real(doc, lighting, "shadowDistance", snapshot.lighting.shadowDistance);
        yyjson_mut_obj_add_real(doc, lighting, "depthBias", snapshot.lighting.depthBias);
        yyjson_mut_obj_add_real(doc, lighting, "normalBias", snapshot.lighting.normalBias);
        yyjson_mut_obj_add_real(doc, lighting, "ambient", snapshot.lighting.ambient);
        yyjson_mut_obj_add_bool(doc, lighting, "shadows", snapshot.lighting.shadows);
        yyjson_mut_obj_add_bool(doc, lighting, "filteredShadows", snapshot.lighting.filteredShadows);
        yyjson_mut_obj_add_bool(doc, lighting, "referenceLighting", snapshot.lighting.referenceLighting);
        yyjson_mut_obj_add_bool(doc, lighting, "forceShadowRefresh", snapshot.lighting.forceShadowRefresh);
    }
    if (!snapshot.modelSources.empty()) {
        string(root, "assetRoot", snapshot.assetRoot);
        auto* a = yyjson_mut_arr(doc);
        for (auto id : snapshot.modelSources)
            yyjson_mut_arr_add_strcpy(doc, a, id.string().c_str());
        yyjson_mut_obj_add_val(doc, root, "modelSources", a);
    }
    auto* entities = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "entities", entities);
    for (const auto& r : snapshot.entities) {
        auto* e = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(entities, e);
        string(e, "id", r.id.string());
        string(e, "name", r.name);
        ref(e, "parent", r.parent);
        yyjson_mut_obj_add_bool(doc, e, "enabled", r.enabled);
        auto* t = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, e, "transform", t);
        vec(t, "position", r.transform.position);
        vec(t, "scale", r.transform.scale);
        vec(t, "rotation",
            glm::vec4(r.transform.rotation.x, r.transform.rotation.y, r.transform.rotation.z, r.transform.rotation.w));
        auto* components = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, e, "components", components);
        if (r.mesh) {
            auto* m = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, components, "meshRenderer", m);
            string(m, "mesh", r.mesh->mesh.string());
            auto* slots = yyjson_mut_arr(doc);
            for (auto id : r.mesh->materials)
                yyjson_mut_arr_add_strcpy(doc, slots, id.string().c_str());
            yyjson_mut_obj_add_val(doc, m, "materials", slots);
            vec(m, "debugColor", r.mesh->color);
            yyjson_mut_obj_add_bool(doc, m, "castShadows", r.mesh->castShadows);
            yyjson_mut_obj_add_bool(doc, m, "receiveShadows", r.mesh->receiveShadows);
        }
        if (r.camera) {
            auto* c = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, components, "camera", c);
            string(c, "projection", "perspective");
            yyjson_mut_obj_add_real(doc, c, "verticalFovDegrees", r.camera->verticalFovDegrees);
            yyjson_mut_obj_add_real(doc, c, "near", r.camera->nearPlane);
            yyjson_mut_obj_add_real(doc, c, "far", r.camera->farPlane);
            yyjson_mut_obj_add_real(doc, c, "exposure", r.camera->exposure);
        }
        if (r.directionalLight) {
            auto* l = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, components, "directionalLight", l);
            vec(l, "color", r.directionalLight->color);
            yyjson_mut_obj_add_real(doc, l, "intensity", r.directionalLight->intensity);
            yyjson_mut_obj_add_bool(doc, l, "shadows", r.directionalLight->shadows);
        }
        if (r.pointLight) {
            auto* l = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, components, "pointLight", l);
            vec(l, "color", r.pointLight->color);
            yyjson_mut_obj_add_real(doc, l, "intensity", r.pointLight->intensity);
            yyjson_mut_obj_add_real(doc, l, "radius", r.pointLight->radius);
            yyjson_mut_obj_add_bool(doc, l, "shadows", r.pointLight->shadows);
        }
        encodeBehaviors(doc, e, r.behaviors);
    }
    size_t length{};
    std::unique_ptr<char, decltype(&free)> data(
        yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length), free);
    if (!data || length > maxDocumentBytes)
        throw std::runtime_error("Не вдалося серіалізувати сцену в межах 32 MiB");
    return {data.get(), length};
}
void atomicWrite(const std::filesystem::path& target, std::string_view bytes,
                 std::optional<std::string_view> expected) {
    const auto path = nativeFilePath(target);
    ensureParent(path);
    auto lockPath = path;
    lockPath += L".lock";
    Handle lock{CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
    if (lock.value == INVALID_HANDLE_VALUE)
        fail("Another save owns the document, or the folder is not writable");
    const bool exists = std::filesystem::exists(path);
    const std::string previous = exists ? readDocument(path) : std::string{};
    if (expected && (!exists || previous != *expected))
        throw std::runtime_error(
            "Файл змінено або видалено іншою програмою. Відкрийте його заново або використайте «Зберегти як».");
    const auto suffix = L".tmp-" + std::filesystem::path(Uuid::create().string()).wstring();
    // Keep temporary names independent of the document basename. A cooked
    // cache hash is already 64 characters; appending another UUID exceeded
    // Win32's path limit in otherwise valid project folders.
    const auto staged = path.parent_path() / suffix;
    const auto backupStage = path.parent_path() / (suffix + L"-backup");
    struct Staging {
        std::filesystem::path a, b;
        ~Staging() {
            DeleteFileW(a.c_str());
            DeleteFileW(b.c_str());
        }
    } cleanup{staged, backupStage};
    writeFlushed(staged, bytes);
    if (exists) {
        writeFlushed(backupStage, previous);
        auto backup = path;
        backup += L".bak";
        if (!MoveFileExW(backupStage.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            fail("Cannot preserve previous document");
    }
    // Both paths are in the same directory/volume; no copy/delete fallback.
    // A denied rename leaves the original document and in-memory dirty state intact.
    if (!MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail("Cannot publish staged document");
}
} // namespace proto
