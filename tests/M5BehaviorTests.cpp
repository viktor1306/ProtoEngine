#include "behavior/BehaviorSchema.hpp"
#include "editor/SceneDocument.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    bool failed{};
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "Expected rejection");
}
struct TestBehavior final : sdk::Behavior {};
constexpr auto typeId = "77777777-7777-4777-8777-777777777777";
constexpr auto bindingId = "88888888-8888-4888-8888-888888888888";
constexpr auto entityId = "11111111-1111-4111-8111-111111111111";
constexpr auto assetId = "22222222-2222-4222-8222-222222222222";

sdk::PropertyDescriptor property(std::string name, sdk::PropertyType type, sdk::PropertyValue value,
                                 std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
    sdk::PropertyDescriptor result;
    result.name = std::move(name);
    result.type = type;
    result.defaultValue = std::move(value);
    result.min = minimum;
    result.max = maximum;
    return result;
}
sdk::BehaviorDescriptor descriptor() {
    sdk::BehaviorDescriptor result;
    result.id = BehaviorTypeId::parse(typeId);
    result.name = "AllTypes";
    result.properties = {
        property("bool", sdk::PropertyType::Bool, false),
        property("integer", sdk::PropertyType::Integer, int64_t(2), 0, 10),
        property("float", sdk::PropertyType::Float, 1.0, -10, 10),
        property("string", sdk::PropertyType::String, std::string("default")),
        property("vec3", sdk::PropertyType::Vec3, sdk::Vec3{1, 2, 3}),
        property("color", sdk::PropertyType::Color, sdk::Color{1, 0.5f, 0.25f, 1}),
        property("entity", sdk::PropertyType::EntityRef, sdk::EntityRef{}),
        property("asset", sdk::PropertyType::AssetRef, sdk::AssetRef{}),
    };
    return result;
}
sdk::BehaviorBinding binding() {
    sdk::BehaviorBinding result;
    result.id = BehaviorBindingId::parse(bindingId);
    result.type = BehaviorTypeId::parse(typeId);
    result.properties = {
        {"bool", true},
        {"integer", int64_t(4)},
        {"float", 2.5},
        {"string", std::string("тест")},
        {"vec3", sdk::Vec3{1, 2, 3}},
        {"color", sdk::Color{0.1f, 0.2f, 0.3f, 1}},
        {"entity", sdk::EntityRef{EntityId::parse(entityId)}},
        {"asset", sdk::AssetRef{AssetId::parse(assetId)}},
    };
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto root =
        (argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("test-results")) / L"m5-behaviors";
    std::filesystem::create_directories(root);
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

    test("registry schema roundtrip validates all property types", [&] {
        sdk::BehaviorRegistry registry;
        registry.Add<TestBehavior>(descriptor());
        const auto schema = describeRegistry(registry, "m5-test-build");
        const auto bytes = encodeBehaviorSchema(schema);
        const auto loaded = decodeBehaviorSchema(bytes, "m5-test-build");
        check(loaded.types == schema.types && loaded.sdkBuildId == schema.sdkBuildId, "Schema roundtrip changed data");
        rejects([&] { decodeBehaviorSchema(bytes, "different-build"); });
    });
    test("scene roundtrip preserves all values and missing references", [&] {
        Scene scene;
        EntityRecord record;
        record.id = EntityId::parse(entityId);
        record.name = "Behavior host";
        record.behaviors.push_back(binding());
        scene.create(record);
        const auto bytes = encodeScene(scene);
        const auto loaded = decodeScene(bytes);
        check(loaded.record(loaded.find(record.id)).behaviors == record.behaviors, "Behavior values did not roundtrip");
        sdk::BehaviorRegistry registry;
        registry.Add<TestBehavior>(descriptor());
        const auto schema = describeRegistry(registry, "m5-test-build");
        rejects([&] { validateSceneBehaviors(loaded, schema); });
        auto unknown = loaded.record(loaded.find(record.id));
        unknown.behaviors.front().properties.emplace("future", std::string("kept"));
        scene.edit(unknown);
        const auto preserved = decodeScene(encodeScene(scene));
        check(preserved.record(preserved.find(record.id)).behaviors.front().properties.contains("future"),
              "Unknown property was not preserved");
    });
    test("normalization inserts defaults and clamps numeric ranges", [&] {
        auto type = descriptor();
        sdk::PropertyMap values{{"integer", int64_t(50)}, {"unknown", std::string("kept")}};
        const auto normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("integer")) == 10, "Integer range was not normalized");
        check(std::get<double>(normalized.at("float")) == 1.0, "Float default was not inserted");
        check(normalized.contains("unknown"), "Unknown property was dropped");
    });
    test("integer normalization preserves exact int64 values", [&] {
        auto type = descriptor();
        type.properties = {property("value", sdk::PropertyType::Integer, int64_t(0))};
        const int64_t aboveDoublePrecision = int64_t(9007199254740993LL);
        sdk::PropertyMap values{{"value", aboveDoublePrecision}};
        auto normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == aboveDoublePrecision,
              "Integer above 2^53 was changed without bounds");
        values["value"] = std::numeric_limits<int64_t>::min();
        normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == std::numeric_limits<int64_t>::min(),
              "INT64_MIN was changed without bounds");
        values["value"] = std::numeric_limits<int64_t>::max();
        normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == std::numeric_limits<int64_t>::max(),
              "INT64_MAX was changed without bounds");

        type.properties = {property("value", sdk::PropertyType::Integer, int64_t(0),
                                    static_cast<double>(std::numeric_limits<int64_t>::min()),
                                    static_cast<double>(std::numeric_limits<int64_t>::max()))};
        values["value"] = std::numeric_limits<int64_t>::min();
        normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == std::numeric_limits<int64_t>::min(),
              "INT64_MIN was changed with endpoint bounds");
        values["value"] = std::numeric_limits<int64_t>::max();
        normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == std::numeric_limits<int64_t>::max(),
              "INT64_MAX was changed with endpoint bounds");

        type.properties = {
            property("value", sdk::PropertyType::Integer, int64_t(0), -9007199254740992.0, 9007199254740992.0)};
        values["value"] = aboveDoublePrecision;
        normalized = normalizeBehaviorProperties(type, values);
        check(std::get<int64_t>(normalized.at("value")) == int64_t(9007199254740992LL),
              "Integer upper bound was not clamped exactly");
    });
    test("missing default EntityRef and AssetRef targets block Play", [&] {
        auto type = descriptor();
        const auto missingEntity = EntityId::create();
        const auto missingAsset = AssetId::create();
        for (auto& item : type.properties) {
            if (item.name == "entity")
                item.defaultValue = sdk::EntityRef{missingEntity};
            if (item.name == "asset")
                item.defaultValue = sdk::AssetRef{};
        }
        auto sceneBinding = binding();
        sceneBinding.properties.erase("entity");
        sceneBinding.properties.erase("asset");
        Scene scene;
        EntityRecord record;
        record.id = EntityId::create();
        record.name = "Default entity ref";
        record.behaviors.push_back(sceneBinding);
        scene.create(record);
        BehaviorSchema schema{"m5-test-build", {type}};
        rejects([&] { validateSceneBehaviors(scene, schema); });

        for (auto& item : type.properties) {
            if (item.name == "entity")
                item.defaultValue = sdk::EntityRef{};
            if (item.name == "asset")
                item.defaultValue = sdk::AssetRef{missingAsset};
        }
        Scene assetScene;
        record.id = EntityId::create();
        assetScene.create(record);
        schema.types.front() = type;
        rejects([&] { validateSceneBehaviors(assetScene, schema); });
    });
    test("duplicate binding IDs fail before scene mutation and dense lifecycle retains IDs", [&] {
        Scene scene;
        EntityRecord first;
        first.id = EntityId::create();
        first.name = "First";
        first.behaviors.push_back(binding());
        scene.create(first);
        EntityRecord duplicate;
        duplicate.id = EntityId::create();
        duplicate.name = "Duplicate";
        duplicate.behaviors.push_back(binding());
        rejects([&] { scene.create(duplicate); });
        check(scene.entities().size() == 1 && !scene.find(duplicate.id), "Rejected duplicate mutated scene");
        const auto bindingValue = scene.record(scene.find(first.id)).behaviors.front();
        scene.eraseSubtree(scene.find(first.id));
        scene.restore({first});
        check(scene.record(scene.find(first.id)).behaviors.front().id == bindingValue.id, "Restore changed binding ID");
        SceneDocument document;
        const auto id = document.create(first);
        const auto pathA = root / L"a.scene.json";
        const auto pathB = root / L"b.scene.json";
        document.save(pathA);
        document.erase(id);
        document.undo();
        check(document.scene.record(document.scene.find(id)).behaviors.front().id == bindingValue.id,
              "Undo changed binding ID");
        document.save(pathB);
        check(document.scene.record(document.scene.find(id)).behaviors.front().id == bindingValue.id,
              "Save As changed binding ID");
    });
    test("bulk restore keeps binding uniqueness without scanning every existing entity", [&] {
        for (const bool withBehaviors : {false, true}) {
            Scene scene;
            for (size_t i = 0; i < 10000; ++i) {
                EntityRecord entity;
                entity.id = EntityId::create();
                if (withBehaviors)
                    entity.behaviors.push_back({BehaviorBindingId::create(), BehaviorTypeId::create(), true, {}});
                scene.create(entity);
            }
            auto clone = Scene::fromSnapshot(scene.snapshot());
            check(clone.entities().size() == 10000, "Bulk scene clone lost entities");
            if (withBehaviors) {
                auto record = clone.record(clone.entities().front());
                clone.eraseSubtree(clone.find(record.id));
                clone.restore({record});
                record.id = EntityId::create();
                rejects([&] { clone.create(record); });
            }
        }
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
