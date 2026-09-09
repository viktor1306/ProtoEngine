#include "editor/Editor.hpp"
#include "editor/EditorPlayState.hpp"

#include "assets/AssetIO.hpp"
#include "project/AssetFilePlanner.hpp"
#include "project/ProjectPackage.hpp"
#include "project/ProjectSession.hpp"
#include "runtime/RuntimePackage.hpp"
#include "scene/SceneIO.hpp"

#include <Proto/Build.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace proto {
namespace fs = std::filesystem;

struct M8AcceptanceState {
    fs::path output, fixtureRoot, root, report, playReport;
    fs::path packageRoot, executable, manifest;
    AssetId projectId, sceneId, copiedModelId;
    std::vector<AssetId> modelIds, materialIds, textureIds;
    std::optional<MaterialValues> editedMaterial;
    std::vector<EntityId> modelRoots;
    std::string firstSource, movedSource, secondSource, copiedSource;
    EntityId hierarchyRoot, ground, sun, warmLight, coolLight, cube, camera;
    std::string beforePlayScene, savedScene;
    uint64_t stageFrame{}, playStartFrame{}, pausedFrames{};
    Clock::time_point playStartedAt{};
    size_t historyBeforePlay{};
    int stage{};
    bool passed{}, importedGltf{}, importedGlb{}, serviceFiles{}, reopened{}, playStarted{}, stopRequested{}, nativeExport{};
};

namespace {

void requireM8(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error("M8 acceptance: " + std::string(message));
}

void replaceOnce(std::string& text, const std::string& from, const std::string& to, std::string_view label) {
    const auto at = text.find(from);
    requireM8(at != std::string::npos, label);
    text.replace(at, from.size(), to);
}

std::string m8BehaviorSource(const fs::path& sampleRoot) {
    auto source = readDocument(sampleRoot / "Code" / "Behaviors.cpp");
    replaceOnce(source, "#include <cmath>\n", "#include <algorithm>\n#include <cmath>\n", "M5 behavior include");

    const auto keyboardAt = source.find("class KeyboardMove final");
    requireM8(keyboardAt != std::string::npos, "M5 KeyboardMove source missing");
    const auto keyboardEnd = source.find("\n};", keyboardAt);
    requireM8(keyboardEnd != std::string::npos, "M5 KeyboardMove class end missing");
    const std::string keyboard = R"CPP(class KeyboardMove final : public Behavior {
    void OnUpdate(BehaviorContext& context, double dt) override {
        auto transform = context.GetTransform(context.Self());
        const float distance = float(context.Property<double>("speed") * dt);
        if (!context.Property<bool>("spaceOnly")) {
            if (context.KeyDown(Key::D) || context.KeyDown(Key::Right))
                transform.position.x += distance;
            if (context.KeyDown(Key::A) || context.KeyDown(Key::Left))
                transform.position.x -= distance;
            if (context.KeyDown(Key::W) || context.KeyDown(Key::Up))
                transform.position.z -= distance;
            if (context.KeyDown(Key::S) || context.KeyDown(Key::Down))
                transform.position.z += distance;
        }
        if (context.KeyPressed(Key::Space)) {
            transform.position.y += .5f;
            context.Log("Space pressed: object moved up by 0.5 m");
        }
        context.SetTransform(context.Self(), transform);
    }
};)CPP";
    source.replace(keyboardAt, keyboardEnd + 3 - keyboardAt, keyboard);

    const std::string freeCamera = R"CPP(
namespace {
sdk::Vec3 add(sdk::Vec3 a, sdk::Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
sdk::Vec3 scale(sdk::Vec3 a, float value) { return {a.x * value, a.y * value, a.z * value}; }
float length(sdk::Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
sdk::Vec3 normalize(sdk::Vec3 value) {
    const auto n = length(value);
    return n > .00001f ? scale(value, 1.0f / n) : sdk::Vec3{0, 0, -1};
}
sdk::Quat normalize(sdk::Quat value) {
    const auto n = std::sqrt(value.w * value.w + value.x * value.x + value.y * value.y + value.z * value.z);
    return n > .00001f ? sdk::Quat{value.w / n, value.x / n, value.y / n, value.z / n} : sdk::Quat{};
}
sdk::Quat multiply(sdk::Quat a, sdk::Quat b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
sdk::Quat axisAngle(sdk::Vec3 axis, float angle) {
    axis = normalize(axis);
    const auto half = angle * .5f;
    const auto s = std::sin(half);
    return normalize({std::cos(half), axis.x * s, axis.y * s, axis.z * s});
}
sdk::Vec3 rotate(sdk::Quat q, sdk::Vec3 value) {
    const sdk::Quat v{0, value.x, value.y, value.z};
    const sdk::Quat inverse{q.w, -q.x, -q.y, -q.z};
    const auto result = multiply(multiply(q, v), inverse);
    return {result.x, result.y, result.z};
}
} // namespace

class FreeCamera final : public Behavior {
    double pitch_{};
    void OnStart(BehaviorContext& context) override {
        const auto transform = context.GetTransform(context.Self());
        const auto forward = normalize(rotate(transform.rotation, {0, 0, -1}));
        pitch_ = std::asin(std::clamp(static_cast<double>(forward.y), -1.0, 1.0));
    }
    void OnUpdate(BehaviorContext& context, double dt) override {
        auto transform = context.GetTransform(context.Self());
        float speed = static_cast<float>(context.Property<double>("speed"));
        if (context.KeyDown(Key::Shift))
            speed *= 3.0f;
        const auto forward = normalize(rotate(transform.rotation, {0, 0, -1}));
        const auto right = normalize(rotate(transform.rotation, {1, 0, 0}));
        const auto up = sdk::Vec3{0, 1, 0};
        sdk::Vec3 movement{};
        if (context.KeyDown(Key::W))
            movement = add(movement, forward);
        if (context.KeyDown(Key::S))
            movement = add(movement, scale(forward, -1));
        if (context.KeyDown(Key::D))
            movement = add(movement, right);
        if (context.KeyDown(Key::A))
            movement = add(movement, scale(right, -1));
        if (context.KeyDown(Key::E))
            movement = add(movement, up);
        if (context.KeyDown(Key::Q))
            movement = add(movement, scale(up, -1));
        const auto distance = speed * static_cast<float>(dt);
        if (length(movement) > .00001f)
            transform.position = add(transform.position, scale(normalize(movement), distance));

        const auto lookSpeed = static_cast<float>(context.Property<double>("lookSpeed") * dt * 3.141592653589793 / 180.0);
        const float yaw = (context.KeyDown(Key::Left) ? 1.0f : 0.0f) * lookSpeed -
                          (context.KeyDown(Key::Right) ? 1.0f : 0.0f) * lookSpeed;
        const float pitch = (context.KeyDown(Key::Up) ? 1.0f : 0.0f) * lookSpeed -
                            (context.KeyDown(Key::Down) ? 1.0f : 0.0f) * lookSpeed;
        if (std::abs(yaw) > .00001f)
            transform.rotation = normalize(multiply(axisAngle({0, 1, 0}, yaw), transform.rotation));
        if (std::abs(pitch) > .00001f) {
            const auto nextPitch = std::max(-1.45, std::min(1.45, pitch_ + static_cast<double>(pitch)));
            const auto pitchDelta = static_cast<float>(nextPitch - pitch_);
            pitch_ = nextPitch;
            if (std::abs(pitchDelta) > .00001f)
                transform.rotation = normalize(multiply(transform.rotation, axisAngle({1, 0, 0}, pitchDelta)));
        }
        context.SetTransform(context.Self(), transform);
    }
};
)CPP";
    const auto registerAt = source.find("void RegisterProjectBehaviors");
    requireM8(registerAt != std::string::npos, "M5 behavior registration missing");
    source.insert(registerAt, freeCamera);
    const std::string oldKeyboard =
        R"CPP(registry.Add<KeyboardMove>(
        {keyboardType, "KeyboardMove", {{"speed", PropertyType::Float, 2.0, 0.0, 30.0, "Швидкість, м/с"}}});)CPP";
    const std::string newKeyboard =
        R"CPP(registry.Add<KeyboardMove>(
        {keyboardType, "KeyboardMove", {{"speed", PropertyType::Float, 2.0, 0.0, 30.0, "Швидкість, м/с"},
                                          {"spaceOnly", PropertyType::Bool, false, {}, {}, "Лише Space"}}});)CPP";
    replaceOnce(source, oldKeyboard, newKeyboard, "M5 KeyboardMove descriptor");
    const std::string freeCameraType =
        R"CPP(
    registry.Add<FreeCamera>({BehaviorTypeId::parse("50000000-0000-4000-8000-000000000004"),
                               "FreeCamera",
                               {{"speed", PropertyType::Float, 4.0, 0.0, 30.0, "Швидкість камери, м/с"},
                                {"lookSpeed", PropertyType::Float, 90.0, 0.0, 360.0, "Огляд, град/с"}}});
)CPP";
    const auto keyboardRegistration = source.find(newKeyboard);
    requireM8(keyboardRegistration != std::string::npos, "M8 KeyboardMove registration insertion point missing");
    source.insert(keyboardRegistration + newKeyboard.size(), freeCameraType);
    return source;
}

const ProjectAsset* findProjectAsset(const std::vector<ProjectAsset>& assets, const std::string& path,
                                     std::string_view kind = {}) {
    for (const auto& asset : assets)
        if (projectPathKey(asset.path) == projectPathKey(path) && (kind.empty() || asset.kind == kind))
            return &asset;
    return nullptr;
}

std::string sceneValueJson(const JsonDoc& document, const char* field) {
    auto* value = get(document.root(), field);
    size_t length{};
    auto* bytes = yyjson_val_write(value, 0, &length);
    if (!bytes)
        throw std::runtime_error("M8 report field serialization failed");
    std::string result(bytes, length);
    free(bytes);
    return result;
}

std::string idsJson(const std::vector<AssetId>& ids) {
    std::string result{"["};
    for (const auto id : ids) {
        if (result.size() > 1)
            result += ',';
        result += jsonString(id.string());
    }
    return result + ']';
}

} // namespace

void Editor::startAcceptanceSmoke(const fs::path& output, const fs::path& fixtureRoot) {
    automatedInput_ = true;
    auto state = std::make_shared<M8AcceptanceState>();
    state->output = fs::absolute(output).lexically_normal();
    state->fixtureRoot = fs::absolute(fixtureRoot).lexically_normal();
    state->root = state->output / ("m8-editor-" + Uuid::create().string());
    state->report = state->output / "m8-acceptance-details.json";
    state->playReport = state->output / "m8-player.json";
    fs::create_directories(state->output);
    requireM8(fs::is_regular_file(state->fixtureRoot / "probes.gltf"), "probes.gltf fixture is missing");
    requireM8(fs::is_regular_file(state->fixtureRoot / "embedded.glb"), "embedded.glb fixture is missing");

    createProject(state->root, "Proto M8 Demo");
    requireM8(project_ && project_->info().name == "Proto M8 Demo", "fresh project creation failed");
    requireM8(document_.scene.modelSources.empty(), "fresh project imported an existing model");
    state->projectId = project_->info().id;
    state->sceneId = document_.scene.id;

    // ProjectSession::create intentionally supplies a usable starter scene.
    // M8 authors its own compact demo, so retain only that scene's camera and
    // remove the starter geometry/lights before the first ImportJob starts.
    const auto activeCamera = document_.scene.activeCamera;
    std::vector<EntityId> starterEntities;
    for (const auto handle : document_.scene.entities())
        if (document_.scene.entity(handle).id != activeCamera)
            starterEntities.push_back(document_.scene.entity(handle).id);
    for (const auto id : starterEntities)
        document_.erase(id);

    const fs::path sample(PROTO_M5_SAMPLE_DIR);
    atomicWrite(state->root / "Code" / "Behaviors.cpp", m8BehaviorSource(sample));
    atomicWrite(state->root / "README.md",
                "# Proto M8 Demo\n\n"
                "This compact scene uses the original deterministic M2 glTF/GLB fixtures.\n"
                "The imported PBR and MASK materials are arranged beside a ground plane,\n"
                "a warm moving lamp, a cool shadowed lamp, and a SunCaster.\n\n"
                "## C++ controls\n\n"
                "The code in `Code/Behaviors.cpp` demonstrates Spin, MoveLight, and KeyboardMove.\n"
                "W/A/S/D move the free camera; Q/E move it vertically; arrow keys look around;\n"
                "Shift increases camera speed. Space raises the green cube by 0.5 m.\n"
                "Play uses the current scene snapshot and Stop restores the editor state.\n");
    const auto notices = sample.parent_path().parent_path() / "third_party" / "NOTICES.md";
    requireM8(fs::is_regular_file(notices), "third-party notice source is missing");
    fs::create_directories(state->root / "Notices");
    fs::copy_file(notices, state->root / "Notices" / "THIRD_PARTY_NOTICES.md",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(state->fixtureRoot / "MANIFEST.md", state->root / "Assets" / "M2-FIXTURE-MANIFEST.md",
                  fs::copy_options::overwrite_existing);

    importScene_ = document_.scene.id;
    importInstance_ = true;
    importJob_.start(project_->assetWorkspace(), state->fixtureRoot / "probes.gltf");
    state->stage = 0;
    acceptanceSmoke_ = std::move(state);
}

bool Editor::acceptanceSmokePassed() const { return acceptanceSmoke_ && acceptanceSmoke_->passed; }

void Editor::acceptanceSmokeStep(uint64_t frame) {
    if (!acceptanceSmoke_ || acceptanceSmoke_->passed || frame < 6)
        return;
    auto& state = *acceptanceSmoke_;
    if (showError_ || !error_.empty())
        throw std::runtime_error("M8 acceptance: " + error_);

    const auto verifyModel = [&](AssetId id, std::string_view label) {
        const auto found = document_.scene.assets->models.find(id);
        requireM8(found != document_.scene.assets->models.end() && found->second, label);
        const auto& model = *found->second;
        requireM8(model.source.starts_with("Assets/"), "imported model escaped Assets");
        requireM8(model.nodes.size() == 10 && model.meshes.size() == 6 && model.materials.size() >= 7,
                  "imported model topology/materials missing");
        requireM8(model.textures.size() >= 5 && model.decodedImages > 0, "imported model textures missing");
        bool pbr{}, mask{}, texture{};
        for (const auto& material : model.materials) {
            pbr |= !material->values.unlit && !material->values.mask;
            mask |= material->values.mask;
            for (const auto& slot : material->textures)
                texture |= bool(slot.texture);
        }
        requireM8(pbr && mask && texture, "imported model lacks PBR/MASK/textured materials");
        return model.source;
    };

    if (state.stage == 0) {
        if (importJob_.busy())
            return;
        requireM8(document_.scene.modelSources.size() == 1, "glTF import did not instantiate one model");
        state.modelIds.push_back(document_.scene.modelSources.front());
        requireM8(static_cast<bool>(document_.selection), "glTF import did not select its new root");
        state.modelRoots.push_back(document_.selection);
        state.firstSource = verifyModel(state.modelIds.front(), "glTF model was not imported");
        state.importedGltf = true;
        importScene_ = document_.scene.id;
        importInstance_ = true;
        importJob_.start(project_->assetWorkspace(), state.fixtureRoot / "embedded.glb");
        state.stage = 1;
        return;
    }
    if (state.stage == 1) {
        if (importJob_.busy())
            return;
        requireM8(document_.scene.modelSources.size() == 2, "GLB import did not instantiate a second model");
        state.modelIds.push_back(document_.scene.modelSources.back());
        requireM8(static_cast<bool>(document_.selection), "GLB import did not select its new root");
        state.modelRoots.push_back(document_.selection);
        requireM8(state.modelIds.front() != state.modelIds.back(), "glTF and GLB reused a model SourceId");
        state.secondSource = verifyModel(state.modelIds.back(), "GLB model was not imported");
        state.importedGlb = true;

        [&] {
            EntityRecord record;
            record.id = EntityId::parse("70000000-0000-4000-8000-000000000001");
            record.name = "M8 Demo · PBR / MASK";
            state.hierarchyRoot = document_.create(record);

            EntityRecord ground;
            ground.id = EntityId::parse("70000000-0000-4000-8000-000000000002");
            ground.name = "Ground · imported materials";
            ground.parent = state.hierarchyRoot;
            ground.transform.scale = {12, 1, 12};
            ground.mesh = MeshRenderer{builtin::plane, {0.12f, 0.16f, 0.22f, 1}};
            state.ground = document_.create(ground);

            EntityRecord sun;
            sun.id = EntityId::parse("f3d05618-ff09-4c42-99e7-85fe2482b5fd");
            sun.name = "SunCaster · shadow cascades";
            sun.parent = state.hierarchyRoot;
            sun.transform.position = {0, 5, -2};
            sun.transform.rotation = glm::normalize(glm::quat{-0.38f, 0.16f, 0.07f, 0.91f});
            sun.directionalLight = DirectionalLight{{1.0f, .95f, .85f}, .85f, true};
            state.sun = document_.create(sun);

            const auto behavior = [](BehaviorTypeId type, sdk::PropertyMap properties) {
                sdk::BehaviorBinding binding;
                binding.id = BehaviorBindingId::create();
                binding.type = type;
                binding.enabled = true;
                binding.properties = std::move(properties);
                return binding;
            };
            EntityRecord warm;
            warm.id = EntityId::parse("004de170-579e-4f2c-8d1f-35903540a74a");
            warm.name = "Warm Point · MoveLight · shadowed";
            warm.parent = state.hierarchyRoot;
            warm.transform.position = {-3.5f, 4.0f, 3.5f};
            warm.pointLight = PointLight{{1.0f, .67f, .32f}, 95.0f, 12.0f, true};
            warm.behaviors.push_back(behavior(BehaviorTypeId::parse("50000000-0000-4000-8000-000000000002"),
                                              {{"radius", 2.0}, {"speed", 1.0}}));
            state.warmLight = document_.create(warm);

            EntityRecord cool;
            cool.id = EntityId::parse("fb6a890e-e681-4c2f-92a6-d2b9d777786e");
            cool.name = "Cool Point · shadowed";
            cool.parent = state.hierarchyRoot;
            cool.transform.position = {3.5f, 3.0f, 2.0f};
            cool.pointLight = PointLight{{.28f, .53f, 1.0f}, 75.0f, 11.0f, true};
            state.coolLight = document_.create(cool);

            EntityRecord cube;
            cube.id = EntityId::parse("50000000-0000-4000-8000-000000000010");
            cube.name = "C++ cube · Space / Spin";
            cube.parent = state.hierarchyRoot;
            cube.transform.position = {0, .65f, 2.5f};
            cube.transform.scale = {1.3f, 1.3f, 1.3f};
            cube.mesh = MeshRenderer{builtin::cube, {0.15f, .8f, .65f, 1}};
            cube.behaviors.push_back(behavior(BehaviorTypeId::parse("50000000-0000-4000-8000-000000000001"),
                                              {{"speedDegreesPerSecond", 30.0}}));
            cube.behaviors.push_back(behavior(BehaviorTypeId::parse("50000000-0000-4000-8000-000000000003"),
                                              {{"speed", 2.0}, {"spaceOnly", true}}));
            state.cube = document_.create(cube);
            state.camera = document_.scene.activeCamera;
            auto camera = document_.scene.record(document_.scene.find(state.camera));
            camera.behaviors.push_back(behavior(BehaviorTypeId::parse("50000000-0000-4000-8000-000000000004"),
                                                {{"speed", 4.0}, {"lookSpeed", 90.0}}));
            document_.edit(camera, "M8 FreeCamera");

            for (size_t i = 0; i < state.modelIds.size(); ++i) {
                const auto id = state.modelIds[i];
                const auto root = state.modelRoots.at(i);
                auto modelRoot = document_.scene.record(document_.scene.find(root));
                modelRoot.parent = state.hierarchyRoot;
                modelRoot.transform.position = id == state.modelIds.front() ? glm::vec3{-2.6f, .9f, 0}
                                                                            : glm::vec3{2.6f, .9f, 0};
                modelRoot.transform.scale = {.72f, .72f, .72f};
                document_.edit(modelRoot, "M8 model layout");
            }
        }();
        bool pbr{}, mask{}, texture{};
        for (const auto& [id, material] : document_.scene.assets->materials) {
            if (material->owner != state.modelIds.front())
                continue;
            if (!material->values.unlit && !material->values.mask && !pbr) {
                state.materialIds.push_back(id);
                pbr = true;
            }
            if (material->values.mask && !mask) {
                state.materialIds.push_back(id);
                mask = true;
            }
            for (const auto& slot : material->textures)
                if (slot.texture && !texture) {
                    state.textureIds.push_back(slot.texture);
                    texture = true;
                }
        }
        requireM8(pbr && mask && texture, "scene material identity probes missing");
        bool pbrNodeWithMaterial{}, maskNodeWithMaterial{};
        for (const auto handle : document_.scene.meshes()) {
            const auto& name = document_.scene.entity(handle).name;
            const auto* mesh = document_.scene.mesh(handle);
            if (!mesh)
                continue;
            const auto foundMesh = document_.scene.assets->meshes.find(mesh->mesh);
            if (foundMesh == document_.scene.assets->meshes.end())
                continue;
            bool nodeMask{}, nodePbr{};
            for (const auto& part : foundMesh->second->parts) {
                const auto foundMaterial = document_.scene.assets->materials.find(part.material);
                if (foundMaterial == document_.scene.assets->materials.end())
                    continue;
                nodeMask |= foundMaterial->second->values.mask;
                nodePbr |= !foundMaterial->second->values.mask && !foundMaterial->second->values.unlit;
            }
            if (name.find("MASK") != std::string::npos)
                maskNodeWithMaterial |= nodeMask;
            if (name.find("Normal") != std::string::npos || name.find("Opaque") != std::string::npos)
                pbrNodeWithMaterial |= nodePbr;
        }
        requireM8(maskNodeWithMaterial && pbrNodeWithMaterial, "imported PBR/MASK hierarchy nodes missing");
        requireM8(document_.scene.directionalLights().size() == 1 && document_.scene.pointLights().size() == 2,
                  "M8 lighting hierarchy has the wrong sun/point-light count");
        const auto lighting = document_.scene.lighting;
        auto configured = lighting;
        configured.shadowResolution = 1024;
        configured.shadowCascades = 3;
        configured.shadowPoolMiB = 80;
        configured.shadowDistance = 35;
        document_.lighting(configured);

        if (!state.materialIds.empty()) {
            const auto id = state.materialIds.front();
            auto values = document_.scene.assets->materials.at(id)->values;
            const auto original = values;
            values.roughness = std::clamp(values.roughness * .65f, .12f, .9f);
            document_.material(id, values);
            state.editedMaterial = values;
            requireM8(document_.scene.assets->materials.at(id)->values == values, "material property edit failed");
            document_.undo();
            requireM8(document_.scene.assets->materials.at(id)->values == original, "material Undo failed");
            document_.redo();
            requireM8(document_.scene.assets->materials.at(id)->values == values, "material Redo failed");
        }

        executeFilePlan({"M8 file operation folders", {{"Assets/Moved", FileKind::Directory},
                                                         {"Assets/Copies", FileKind::Directory}}});
        const auto firstId = state.modelIds.front();
        operateFiles(int(FileOperation::Move), {state.firstSource}, "Assets/Moved", std::string("probes-renamed.gltf"));
        state.movedSource = "Assets/Moved/probes-renamed.gltf";
        auto moved = project_->assetWorkspace()->load(firstId);
        requireM8(moved->id == firstId && moved->source == state.movedSource, "model move changed SourceId");
        requireM8(std::find(document_.scene.modelSources.begin(), document_.scene.modelSources.end(), firstId) !=
                      document_.scene.modelSources.end(),
                  "scene reference changed after model move");
        document_.undo();
        requireM8(project_->assetWorkspace()->load(firstId)->source == state.firstSource,
                  "model move Undo did not restore source");
        document_.redo();
        requireM8(project_->assetWorkspace()->load(firstId)->source == state.movedSource,
                  "model move Redo changed SourceId");
        const auto secondId = state.modelIds.back();
        operateFiles(int(FileOperation::Copy), {state.secondSource}, "Assets/Copies", std::string("embedded-copy.glb"));
        state.copiedSource = "Assets/Copies/embedded-copy.glb";
        const auto copiedAssets = inspectProjectAssets(project_->root());
        const auto* copied = findProjectAsset(copiedAssets, state.copiedSource, "ModelSource");
        requireM8(copied != nullptr, "model copy did not publish a ModelSource");
        const auto copiedId = AssetId::parse(copied->id);
        state.copiedModelId = copiedId;
        requireM8(copiedId != secondId, "model copy reused original SourceId");
        document_.undo();
        requireM8(!fs::exists(projectPath(project_->root(), state.copiedSource)), "copy Undo left destination");
        document_.redo();
        const auto copiedAssetsAgain = inspectProjectAssets(project_->root());
        const auto copiedAgain = findProjectAsset(copiedAssetsAgain, state.copiedSource, "ModelSource");
        requireM8(copiedAgain && copiedAgain->id == copiedId.string(), "copy Redo changed new SourceId");
        state.serviceFiles = true;

        // Keep a compact, deliberately authored scene: the two imported model
        // roots are laid out beside the cube, with the ground and three lights
        // forming one readable hierarchy. The file operations above are the
        // service-path contract; native gesture coverage belongs to M4/M5.
        document_.selection = state.cube;
        document_.saveProject(document_.path(), project_->root());
        state.savedScene = encodeScene(document_.scene);
        requireM8(!document_.dirty(), "final authored scene did not save");

        document_.newScene();
        projectUi_.reset();
        project_.reset();
        openProject(state.root);
        useSceneCamera_ = true;
        requireM8(project_->info().id == state.projectId && document_.scene.id == state.sceneId,
                  "project or scene identity changed after reopen");
        requireM8(encodeScene(document_.scene) == state.savedScene, "full scene changed after close/reopen");
        for (const auto id : state.modelIds)
            requireM8(document_.scene.assets->models.contains(id), "model SourceId changed after reopen");
        for (const auto id : state.materialIds)
            requireM8(document_.scene.assets->materials.contains(id), "material AssetId changed after reopen");
        for (const auto id : state.textureIds)
            requireM8(document_.scene.assets->textures.contains(id), "texture AssetId changed after reopen");
        if (state.editedMaterial)
            requireM8(document_.scene.assets->materials.at(state.materialIds.front())->values == *state.editedMaterial,
                      "material property edit did not persist after reopen");
        state.reopened = true;
        state.stage = 2;
        state.stageFrame = frame;
        return;
    }
    if (state.stage == 2) {
        if (frame < state.stageFrame + 4)
            return;
        // The next edit is intentional and remains unsaved through the Player
        // session; this checks Stop's editor-state isolation.
        auto cube = document_.scene.record(document_.scene.find(state.cube));
        cube.transform.position.x += .25f;
        document_.edit(cube, "M8 unsaved Play edit");
        document_.selection = state.cube;
        state.beforePlayScene = encodeScene(document_.scene);
        state.historyBeforePlay = document_.undoCount();
        state.playStartFrame = frame;
        state.playReport = state.output / "m8-player.json";
        if (!playUi_)
            playUi_ = std::make_shared<PlayUiState>();
        playUi_->extraPlayerArguments = {"--frames", "6000", "--fixed-dt", "0.0166666666666667", "--test-input",
                                         "--report", utf8(state.playReport.wstring())};
        requestBuild(true);
        state.stage = 3;
        return;
    }
    if (state.stage == 3) {
        if (playerActive()) {
            if (!state.playStarted) {
                state.playStarted = true;
                state.pausedFrames = renderer_.sceneRenderFrames();
                state.playStartedAt = Clock::now();
            }
            requireM8(renderer_.sceneRenderFrames() == state.pausedFrames, "Player did not pause editor rendering");
            if (!state.stopRequested && milliseconds(state.playStartedAt) > 5000) {
                state.stopRequested = true;
                stopPlay();
            }
            return;
        }
        if (playBusy())
            return;
        requireM8(state.playStarted && state.stopRequested && playUi_ && playUi_->lastSucceeded, "C++ Player run/Stop failed");
        requireM8(document_.dirty() && document_.selection == state.cube &&
                      document_.undoCount() == state.historyBeforePlay && encodeScene(document_.scene) == state.beforePlayScene,
                  "Stop did not preserve unsaved scene bytes/history/selection");

        requireM8(fs::is_regular_file(state.playReport), "Player report missing");
        const JsonDoc report(readDocument(state.playReport));
        requireM8(yyjson_is_true(get(report.root(), "passed")), "Player report failed");
        requireM8(num(get(report.root(), "frames")) >= 60 && num(get(report.root(), "starts")) >= 4 &&
                      num(get(report.root(), "stops")) >= 4,
                  "Player lifecycle report incomplete");
        const auto finalSceneText = sceneValueJson(report, "finalScene");
        auto finalScene = decodeScene(finalSceneText, document_.scene.assets);
        const auto finalCube = finalScene.record(finalScene.find(state.cube));
        const auto originalCube = document_.scene.record(document_.scene.find(state.cube));
        const auto finalLight = finalScene.transform(finalScene.find(state.warmLight)).local.position;
        const auto originalLight = document_.scene.transform(document_.scene.find(state.warmLight)).local.position;
        const auto finalCamera = finalScene.transform(finalScene.find(state.camera)).local.position;
        const auto originalCamera = document_.scene.transform(document_.scene.find(state.camera)).local.position;
        requireM8(finalCube.transform.position.y > originalCube.transform.position.y + .49f,
                  "Keyboard Space did not move the cube");
        requireM8(finalCube.transform.rotation != originalCube.transform.rotation, "Spin did not rotate the cube");
        requireM8(finalLight != originalLight, "MoveLight did not move the warm lamp");
        requireM8(finalCamera != originalCamera, "FreeCamera D input did not move the camera");
        requireM8(num(get(report.root(), "shadow_faces_total")) > 90 * 6, "Player shadow faces did not refresh");

        document_.saveProject(document_.path(), project_->root());
        requireM8(!document_.dirty(), "final authored scene did not save after Stop");
        state.savedScene = encodeScene(document_.scene);
        state.stage = 4;
        state.stageFrame = frame;
        return;
    }
    if (state.stage == 4) {
        if (exportButtonMax_.x <= exportButtonMin_.x || exportButtonMax_.y <= exportButtonMin_.y)
            return;
        const auto click = [this](bool down) {
            const auto center = ImVec2((exportButtonMin_.x + exportButtonMax_.x) * .5f,
                                       (exportButtonMin_.y + exportButtonMax_.y) * .5f);
            testInput_ = [center, down] {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(center.x, center.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            };
        };
        click(true);
        state.nativeExport = true;
        state.stage = 5;
        return;
    }
    if (state.stage == 5) {
        const auto center = ImVec2((exportButtonMin_.x + exportButtonMax_.x) * .5f,
                                   (exportButtonMin_.y + exportButtonMax_.y) * .5f);
        testInput_ = [center] {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(center.x, center.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        };
        state.stage = 6;
        return;
    }
    if (state.stage == 6) {
        if (playBusy())
            return;
        requireM8(state.nativeExport && playUi_ && playUi_->lastSucceeded && playUi_->lastPackage,
                  "native Export button did not produce a package");
        state.packageRoot = playUi_->lastPackage->packageRoot;
        state.executable = playUi_->lastPackage->executable;
        state.manifest = playUi_->lastPackage->manifest;
        const auto package = loadRuntimePackage(state.manifest, sdk::buildId);
        requireM8(package.projectId == state.projectId && package.scene.id == state.sceneId,
                  "runtime package identity mismatch");
        requireM8(package.scene.modelSources.size() == state.modelIds.size(), "runtime package model list mismatch");
        requireM8(fs::is_regular_file(state.executable) && fs::is_regular_file(state.manifest),
                  "runtime package executable or manifest missing");

        std::ostringstream details;
        details << "{\"format\":\"proto.m8.acceptance\",\"version\":1,\"passed\":true"
                << ",\"project_root\":" << jsonString(utf8(state.root.wstring()))
                << ",\"fixture_root\":" << jsonString(utf8(state.fixtureRoot.wstring()))
                << ",\"project_id\":" << jsonString(state.projectId.string())
                << ",\"scene_id\":" << jsonString(state.sceneId.string())
                << ",\"model_ids\":" << idsJson(state.modelIds)
                << ",\"copied_model_id\":" << jsonString(state.copiedModelId.string())
                << ",\"material_ids\":" << idsJson(state.materialIds)
                << ",\"texture_ids\":" << idsJson(state.textureIds)
                << ",\"fresh_project\":true,\"reopened\":true,\"imports\":{\"gltf\":true,\"glb\":true,\"materials\":true,\"textures\":true,\"stable_ids\":true}"
                << ",\"hierarchy\":{\"pbr\":true,\"mask\":true,\"suncaster\":true,\"shadowed_point_lights\":2}"
                << ",\"service_vs_native\":{\"service_setup\":true,\"property_edit\":true,\"operateFiles\":true,\"undo_redo\":true,\"native_export_button\":true}"
                << ",\"play_report\":" << jsonString(utf8(state.playReport.wstring()))
                << ",\"camera_transform_changed\":true"
                << ",\"stop_requested\":true"
                << ",\"state_restoration\":{\"scene_bytes\":true,\"history\":true,\"selection\":true,\"unsaved_edit\":true}"
                << ",\"package_root\":" << jsonString(utf8(state.packageRoot.wstring()))
                << ",\"executable\":" << jsonString(utf8(state.executable.wstring()))
                << ",\"manifest\":" << jsonString(utf8(state.manifest.wstring())) << "}\n";
        atomicWrite(state.report, details.str());
        status_ = "M8: свіжий проєкт, імпорт, Play і native Export перевірено";
        playUi_->showLog = false;
        state.passed = true;
    }
}

} // namespace proto
