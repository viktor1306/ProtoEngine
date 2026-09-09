#include "runtime/BehaviorRuntime.hpp"
#include "runtime/RuntimeSnapshot.hpp"
#include "project/ProjectSession.hpp"
#include "project/PlayAssets.hpp"
#include "project/AssetFilePlanner.hpp"
#include "AssetFixtures.hpp"
#include "scene/SceneIO.hpp"
#include <Proto/Build.hpp>
#include <windows.h>
#include <iostream>
#include <cmath>
#include <limits>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F&& action) {
    bool failed{};
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "Expected runtime rejection");
}
const auto probeType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000020");
const auto deferredType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000021");
const auto manyType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000022");
const auto invalidTransformType = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000023");
std::vector<std::string> events;
std::vector<sdk::Vec3> observedWorld;
class Probe final : public sdk::Behavior {
    void OnStart(sdk::BehaviorContext& c) override {
        events.push_back("start");
        c.Log("ready");
    }
    void OnUpdate(sdk::BehaviorContext& c, double dt) override {
        auto t = c.GetTransform(c.Self());
        t.position.x += float(c.Property<double>("speed") * dt);
        if (c.KeyPressed(sdk::Key::Space))
            t.position.y += 1;
        c.SetTransform(c.Self(), t);
        events.push_back("update");
    }
    void OnStop(sdk::BehaviorContext&) override { events.push_back("stop"); }
};
class ThrowStart final : public sdk::Behavior {
    void OnStart(sdk::BehaviorContext&) override { throw std::runtime_error("start failure"); }
};
class ThrowStop final : public sdk::Behavior {
    void OnStop(sdk::BehaviorContext&) override { throw std::runtime_error("stop failure"); }
};
class DeferredWrites final : public sdk::Behavior {
    void OnUpdate(sdk::BehaviorContext& c, double) override {
        auto transform = c.GetTransform(c.Self());
        transform.position.x += 1;
        c.SetTransform(c.Self(), transform);
        observedWorld.push_back(c.GetWorldPosition(c.Self()));
        transform = c.GetTransform(c.Self());
        transform.position.y = observedWorld.back().x;
        c.SetTransform(c.Self(), transform);
    }
};
class ManyWrites final : public sdk::Behavior {
    void OnUpdate(sdk::BehaviorContext& c, double) override {
        auto transform = c.GetTransform(c.Self());
        transform.position.x += 1;
        c.SetTransform(c.Self(), transform);
        transform = c.GetTransform(c.Self());
        transform.position.x += 1;
        c.SetTransform(c.Self(), transform);
    }
};
class InvalidTransform final : public sdk::Behavior {
    void OnUpdate(sdk::BehaviorContext& c, double) override {
        auto transform = c.GetTransform(c.Self());
        transform.position.x = std::numeric_limits<float>::infinity();
        c.SetTransform(c.Self(), transform);
    }
};
sdk::BehaviorBinding bind(BehaviorTypeId type) {
    return {BehaviorBindingId::create(), type, true, {}};
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2)
            throw std::runtime_error("Need output root");
        const auto root = std::filesystem::absolute(argv[1]) / ("m5-runtime-" + Uuid::create().string());
        auto project = ProjectSession::create(root, "Runtime contract");
        Scene scene;
        EntityRecord record;
        record.id = EntityId::create();
        record.name = "Runtime cube";
        record.mesh = MeshRenderer{};
        record.behaviors.push_back(bind(probeType));
        record.behaviors[0].properties["speed"] = int64_t(4);
        const auto h = scene.create(record);
        auto disabled = record;
        disabled.id = EntityId::create();
        disabled.behaviors[0].id = BehaviorBindingId::create();
        disabled.enabled = false;
        scene.create(disabled);
        sdk::BehaviorRegistry registry;
        registry.Add<Probe>({probeType, "Probe", {{"speed", sdk::PropertyType::Float, 2.0, 0.0, 10.0}}});
        std::string logged;
        BehaviorRuntime runtime(scene, registry, [&](std::string_view s) { logged += s; });
        check(events.empty(), "Editor construction ran lifecycle");
        runtime.start();
        RuntimeInput input;
        input.pressed[size_t(sdk::Key::Space)] = true;
        runtime.update(.25, input);
        input.pressed.fill(false);
        runtime.update(.25, input);
        runtime.stop();
        runtime.stop();
        check(runtime.metrics().starts == 1 && runtime.metrics().updates == 2 && runtime.metrics().stops == 1,
              "Lifecycle count");
        check(events == std::vector<std::string>{"start", "update", "update", "stop"}, "Lifecycle order");
        check(scene.transform(h).local.position == glm::vec3(2, 1, 0), "SDK transform/input/normalized property");
        check(!logged.empty(), "Behavior log missing");
        rejects([&] { runtime.update(.1, {}); });

        registry.Add<DeferredWrites>({deferredType, "DeferredWrites", {}});
        observedWorld.clear();
        Scene deferredScene;
        EntityRecord deferredRecord;
        deferredRecord.id = EntityId::create();
        deferredRecord.name = "Deferred transform";
        deferredRecord.behaviors = {bind(deferredType)};
        const auto deferredHandle = deferredScene.create(deferredRecord);
        BehaviorRuntime deferredRuntime(deferredScene, registry, [](auto) {});
        deferredRuntime.start();
        deferredRuntime.update(.1, {});
        deferredRuntime.stop();
        check(observedWorld.size() == 1 && observedWorld.front() == sdk::Vec3{1, 0, 0},
              "World query did not observe the first deferred transform");
        check(deferredScene.transform(deferredHandle).local.position == glm::vec3(1, 1, 0),
              "Multiple deferred transform writes did not publish their final local state");

        registry.Add<ManyWrites>({manyType, "ManyWrites", {}});
        Scene many;
        constexpr size_t movingCount = 64;
        for (size_t i = 0; i < movingCount; ++i) {
            EntityRecord moving;
            moving.id = EntityId::create();
            moving.name = "Moving " + std::to_string(i);
            moving.behaviors = {bind(manyType)};
            many.create(moving);
        }
        BehaviorRuntime manyRuntime(many, registry, [](auto) {});
        manyRuntime.start();
        const auto evaluationsBeforeFrame = many.transformEvaluations();
        manyRuntime.update(.1, {});
        check(many.transformEvaluations() - evaluationsBeforeFrame == movingCount,
              "Deferred writes did not collapse repeated per-entity evaluation");
        for (const auto handle : many.entities())
            check(many.transform(handle).local.position.x == 2.0f, "Many deferred writers lost a transform update");
        manyRuntime.stop();

        registry.Add<InvalidTransform>({invalidTransformType, "InvalidTransform", {}});
        Scene invalidTransformScene;
        EntityRecord invalidTransformRecord;
        invalidTransformRecord.id = EntityId::create();
        invalidTransformRecord.name = "Invalid deferred transform";
        invalidTransformRecord.transform.position.x = 7;
        invalidTransformRecord.behaviors = {bind(invalidTransformType)};
        const auto invalidTransformHandle = invalidTransformScene.create(invalidTransformRecord);
        BehaviorRuntime invalidTransformRuntime(invalidTransformScene, registry, [](auto) {});
        invalidTransformRuntime.start();
        const auto beforeInvalid = invalidTransformScene.transform(invalidTransformHandle).local;
        rejects([&] { invalidTransformRuntime.update(.1, {}); });
        check(invalidTransformScene.transform(invalidTransformHandle).local == beforeInvalid,
              "Invalid deferred transform partially mutated local state");
        invalidTransformRuntime.stop();

        const auto original = encodeScene(scene);
        auto snapshot = stagePlaySnapshot(root, scene, sdk::buildId);
        check(encodeScene(scene) == original, "Staging mutated authored scene");
        auto loaded = loadRuntimeSnapshot(snapshot.manifest, sdk::buildId);
        check(encodeScene(loaded) == original, "Pinned snapshot changed scene state");
        rejects([&] { loadRuntimeSnapshot(snapshot.manifest, "wrong-build-id"); });
        HANDLE writer =
            CreateFileW(snapshot.manifest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(writer == INVALID_HANDLE_VALUE, "Pinned snapshot allowed mutation");
        check(!DeleteFileW(snapshot.manifest.c_str()), "Pinned snapshot allowed deletion");
        auto invalid = record;
        invalid.id = EntityId::create();
        invalid.behaviors[0].id = BehaviorBindingId::create();
        invalid.behaviors[0].type = BehaviorTypeId::create();
        scene.create(invalid);
        rejects([&] { BehaviorRuntime invalidRuntime(scene, registry, [](auto) {}); });
        scene.eraseSubtree(scene.find(invalid.id));
        // A failed OnStart stops all earlier successful instances, once.
        const auto failType = BehaviorTypeId::create();
        registry.Add<ThrowStart>({failType, "ThrowStart", {}});
        auto first = record;
        first.id = EntityId::parse("00000000-0000-4000-8000-000000000100");
        first.behaviors[0].id = BehaviorBindingId::create();
        auto last = first;
        last.id = EntityId::parse("ffffffff-ffff-4fff-8fff-ffffffffffff");
        last.behaviors = {bind(failType)};
        Scene failing;
        failing.create(first);
        failing.create(last);
        events.clear();
        {
            BehaviorRuntime failed(failing, registry, [](auto) {});
            rejects([&] { failed.start(); });
        }
        check(events == std::vector<std::string>{"start", "stop"}, "Failed start did not unwind");
        const auto stopType = BehaviorTypeId::create();
        registry.Add<ThrowStop>({stopType, "ThrowStop", {}});
        last.behaviors = {bind(stopType)};
        Scene stopFailure;
        stopFailure.create(first);
        stopFailure.create(last);
        events.clear();
        {
            BehaviorRuntime failed(stopFailure, registry, [](auto) {});
            failed.start();
            rejects([&] { failed.stop(); });
            failed.stop();
            check(failed.metrics().stops == 2, "OnStop failure skipped another instance");
        }
        check(events == std::vector<std::string>{"start", "stop"}, "OnStop failure repeated or skipped lifecycle");
        // A dependency referenced only by a behavior must survive reopen and cooking.
        const auto fixtures = root.parent_path() / ("m5-assets-" + Uuid::create().string());
        fixtures::create(fixtures);
        const auto model = project->assetWorkspace()->importFile(fixtures / "probes.gltf");
        const auto refType = BehaviorTypeId::create();
        sdk::BehaviorRegistry refRegistry;
        refRegistry.Add<Probe>(
            {refType,
             "References",
             {{"model", sdk::PropertyType::AssetRef, sdk::AssetRef{model->id}},
              {"material", sdk::PropertyType::AssetRef, sdk::AssetRef{model->materials.front()->id}}}});
        Scene references;
        EntityRecord refEntity;
        refEntity.id = EntityId::create();
        refEntity.behaviors = {bind(refType)};
        references.create(refEntity);
        auto reopened = decodeScene(encodeScene(references));
        const auto refSchema = describeRegistry(refRegistry, sdk::buildId);
        rejects([&] { validateSceneBehaviors(reopened, refSchema); });
        preparePlayAssets(reopened, root, refSchema);
        validateSceneBehaviors(reopened, refSchema);
        check(reopened.modelSources.empty() && references.assets->models.empty(),
              "Asset closure changed editor model instances/catalog");
        auto refSnapshot = stagePlaySnapshot(root, reopened, sdk::buildId);
        auto refPlayer = loadRuntimeSnapshot(refSnapshot.manifest, sdk::buildId);
        check(refPlayer.assets->materials.contains(model->materials.front()->id),
              "Behavior-only resource missing in Player");
        validateSceneBehaviors(refPlayer, refSchema);
        atomicWrite(std::filesystem::path(argv[1]) / "m5-runtime.json", "{\"passed\":true,\"contracts\":16}");
        std::cout << "M5 runtime: 16 contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
