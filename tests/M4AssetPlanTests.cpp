#include "AssetFixtures.hpp"
#include "WinJunction.hpp"
#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "editor/SceneDocument.hpp"
#include "project/AssetFilePlanner.hpp"
#include "project/FileTransactions.hpp"
#include "scene/SceneIO.hpp"

#include <windows.h>
#include <yyjson.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace proto;
namespace {
namespace fs = std::filesystem;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void writeText(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(output), "Test fixture write failed");
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(bool(input), "Test fixture read failed");
    const auto size = input.tellg();
    check(size >= 0, "Test fixture size failed");
    std::string bytes(static_cast<size_t>(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    check(bool(input), "Test fixture read was incomplete");
    return bytes;
}

void expectThrow(const std::function<void()>& action, const char* message) {
    bool thrown = false;
    try {
        action();
    } catch (const std::exception&) {
        thrown = true;
    }
    check(thrown, message);
}

std::string relativePath(const fs::path& root, const fs::path& path) {
    return utf8(fs::absolute(path)
                    .lexically_normal()
                    .lexically_relative(fs::absolute(root).lexically_normal())
                    .generic_wstring());
}

std::set<std::string> authoredTree(const fs::path& root) {
    std::set<std::string> result;
    for (const auto& top : {root / "Assets", root / "Scenes", root / "Code", root / "Config"}) {
        if (!fs::exists(top))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(top))
            result.insert(relativePath(root, entry.path()));
    }
    return result;
}

std::shared_ptr<FileTransaction> applyPlan(const fs::path& root, const FilePlan& plan) {
    auto transaction = FileTransaction::prepare(root, plan);
    transaction->redo();
    return transaction;
}

struct Project {
    fs::path area;
    fs::path root;
    fs::path fixtures;
    std::shared_ptr<AssetWorkspace> workspace;
    std::shared_ptr<const ModelBundle> model;
    std::string source;
    std::string package;
};

Project makeProject(const fs::path& output, std::string_view fixtureName = "probes.gltf") {
    Project project;
    project.area = output / ("m4-plan-" + Uuid::create().string());
    project.root = project.area / L"Project з URI";
    project.fixtures = project.area / L"fixtures";
    fs::create_directories(project.root);
    fixtures::create(project.fixtures);
    project.workspace = std::make_shared<AssetWorkspace>(project.root);
    project.model = project.workspace->importFile(project.fixtures / fs::path(std::string(fixtureName)));
    project.source = project.model->source;
    project.package = utf8(fs::path(project.source).parent_path().generic_wstring());
    return project;
}

struct SceneState {
    fs::path path;
    AssetId id;
    std::set<std::string> entities;
    MaterialValues material;
};

SceneState makeProjectScene(Project& project) {
    SceneDocument document;
    document.saveProject(project.root / L"Scenes/Main.scene.json", project.root);
    document.instantiate(project.model);
    const auto materialId = project.model->materials.front()->id;
    auto material = document.scene.assets->materials.at(materialId)->values;
    material.roughness = .27f;
    document.material(materialId, material);
    document.saveProject(project.root / L"Scenes/Main.scene.json", project.root);
    SceneState state;
    state.path = project.root / L"Scenes/Main.scene.json";
    state.id = document.scene.id;
    state.material = material;
    for (const auto handle : document.scene.entities())
        state.entities.insert(document.scene.entity(handle).id.string());
    return state;
}

std::string jsonStringField(Json* object, const char* key) {
    auto* value = yyjson_obj_get(object, key);
    check(value && yyjson_is_str(value), "Expected JSON string field");
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

std::map<std::string, std::string> dependencyIds(const fs::path& metadataPath) {
    const auto bytes = readText(metadataPath);
    JsonDoc document(bytes);
    std::map<std::string, std::string> result;
    auto* dependencies = yyjson_obj_get(document.root(), "dependencies");
    if (!dependencies)
        return result;
    size_t i, n;
    Json* value;
    yyjson_arr_foreach(dependencies, i, n, value) {
        if (yyjson_obj_get(value, "assetId"))
            result[jsonStringField(value, "uri")] = jsonStringField(value, "assetId");
    }
    return result;
}

std::set<std::string> materialOverrideKeys(const fs::path& metadataPath) {
    const auto bytes = readText(metadataPath);
    JsonDoc document(bytes);
    std::set<std::string> result;
    auto* overrides = yyjson_obj_get(document.root(), "materialOverrides");
    if (!overrides || !yyjson_is_obj(overrides))
        return result;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(overrides);
    while (auto* key = yyjson_obj_iter_next(&iterator))
        result.emplace(yyjson_get_str(key), yyjson_get_len(key));
    return result;
}

std::string sceneIdFrom(const fs::path& scenePath) {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    return jsonStringField(document.root(), "sceneId");
}

std::set<std::string> sceneEntityIds(const fs::path& scenePath) {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    std::set<std::string> result;
    auto* entities = yyjson_obj_get(document.root(), "entities");
    check(entities && yyjson_is_arr(entities), "Scene entities missing");
    size_t i, n;
    Json* value;
    yyjson_arr_foreach(entities, i, n, value) result.insert(jsonStringField(value, "id"));
    return result;
}

std::string sceneMeshId(const fs::path& scenePath) {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    auto* entities = yyjson_obj_get(document.root(), "entities");
    size_t i, n;
    Json* value;
    yyjson_arr_foreach(entities, i, n, value) {
        auto* components = yyjson_obj_get(value, "components");
        auto* mesh = components ? yyjson_obj_get(components, "meshRenderer") : nullptr;
        if (mesh)
            return jsonStringField(mesh, "mesh");
    }
    return {};
}

std::string sceneBehaviorAssetRef(const fs::path& scenePath, const char* propertyName = "asset") {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    auto* entities = yyjson_obj_get(document.root(), "entities");
    check(entities && yyjson_is_arr(entities) && yyjson_arr_size(entities), "Scene entities missing for behavior ref");
    auto* entity = yyjson_arr_get(entities, 0);
    auto* behaviors = entity ? yyjson_obj_get(entity, "behaviors") : nullptr;
    check(behaviors && yyjson_is_arr(behaviors) && yyjson_arr_size(behaviors), "Behavior binding missing");
    auto* binding = yyjson_arr_get(behaviors, 0);
    auto* properties = binding ? yyjson_obj_get(binding, "properties") : nullptr;
    auto* value = properties ? yyjson_obj_get(properties, propertyName) : nullptr;
    auto* wrapper = value ? yyjson_obj_get(value, "assetRef") : nullptr;
    check(wrapper && yyjson_is_str(wrapper), "Typed behavior AssetRef missing");
    return {yyjson_get_str(wrapper), yyjson_get_len(wrapper)};
}

std::string sceneBehaviorOpaqueString(const fs::path& scenePath) {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    auto* entities = yyjson_obj_get(document.root(), "entities");
    auto* entity = entities && yyjson_is_arr(entities) ? yyjson_arr_get(entities, 0) : nullptr;
    auto* behaviors = entity ? yyjson_obj_get(entity, "behaviors") : nullptr;
    auto* binding = behaviors && yyjson_is_arr(behaviors) ? yyjson_arr_get(behaviors, 0) : nullptr;
    auto* properties = binding ? yyjson_obj_get(binding, "properties") : nullptr;
    auto* value = properties ? yyjson_obj_get(properties, "opaque") : nullptr;
    check(value && yyjson_is_str(value), "Opaque behavior string missing");
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

void makeBehaviorAssetScene(Project& project, const fs::path& path) {
    Scene scene;
    scene.assets->publish(project.model);
    EntityRecord entity;
    entity.id = EntityId::create();
    entity.name = "Behavior AssetRef";
    sdk::BehaviorBinding behavior;
    behavior.id = BehaviorBindingId::create();
    behavior.type = BehaviorTypeId::create();
    behavior.properties.emplace("asset", sdk::AssetRef{project.model->meshes.front()->id});
    behavior.properties.emplace("asset2", sdk::AssetRef{project.model->meshes.at(1)->id});
    behavior.properties.emplace("opaque", project.model->meshes.front()->id.string());
    entity.behaviors.push_back(std::move(behavior));
    scene.create(entity);
    writeText(path, encodeScene(scene));
}

std::vector<std::string> sceneSources(const fs::path& scenePath) {
    const auto bytes = readText(scenePath);
    JsonDoc document(bytes);
    std::vector<std::string> result;
    auto* sources = yyjson_obj_get(document.root(), "modelSources");
    if (!sources)
        return result;
    size_t i, n;
    Json* value;
    yyjson_arr_foreach(sources, i, n, value) {
        check(value && yyjson_is_str(value), "Expected model source UUID string");
        result.emplace_back(yyjson_get_str(value), yyjson_get_len(value));
    }
    return result;
}

void makeAssetScene(Project& project, const fs::path& path) {
    Scene scene;
    scene.assetRoot = ".";
    scene.modelSources = {project.model->id};
    scene.assets->publish(project.model);
    EntityRecord entity;
    entity.id = EntityId::create();
    entity.name = "Copied model";
    entity.mesh = MeshRenderer{project.model->meshes.front()->id, glm::vec4(1)};
    scene.create(entity);
    scene.update();
    writeText(path, encodeScene(scene));
}

std::string findModelSourceAt(const fs::path& root, const std::string& path) {
    for (const auto& asset : inspectProjectAssets(root))
        if (asset.path == path && asset.kind == "ModelSource")
            return asset.id;
    return {};
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto output = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("test-results"));
    fs::create_directories(output);
    unsigned passed{};
    unsigned failed{};
    const auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("asset index, enrollment and project URI bounds", [&] {
        auto project = makeProject(output);
        const auto modelDirectory = resourcePath(project.root, project.source).parent_path();
        check(assetUriWithin(modelDirectory, "../Shared/x.bin", project.root / "Assets") ==
                  project.root / "Assets/Shared/x.bin",
              "Sibling Assets URI was not resolved");
        expectThrow([&] { (void)assetUriWithin(modelDirectory, "../../outside.bin", project.root / "Assets"); },
                    "Project URI escape was accepted");
        const auto enrollment = enrollProjectFiles(project.root);
        check(!enrollment.edits.empty(), "Enrollment produced no sidecars");
        applyPlan(project.root, enrollment);
        const auto assets = inspectProjectAssets(project.root);
        bool sourceFound = false;
        bool meshFound = false;
        bool dependencyFound = false;
        for (const auto& asset : assets) {
            sourceFound |= asset.id == project.model->id.string() && asset.kind == "ModelSource";
            meshFound |= asset.kind == "Mesh" && asset.owner == project.model->id.string();
            dependencyFound |= asset.kind == "RawDependency";
        }
        check(sourceFound && meshFound && dependencyFound, "Enrollment index is incomplete");
        check(!dependencyIds(resourcePath(project.root, project.source).parent_path() /
                             (fs::path(project.source).filename().wstring() + L".meta"))
                   .empty(),
              "Model dependency IDs were not enrolled");
    });

    test("read-only reload rejects a new texture locator without rewriting UUID metadata", [&] {
        auto project = makeProject(output);
        const auto sourcePath = resourcePath(project.root, project.source);
        const auto metadataPath = fs::path(sourcePath.wstring() + L".meta");
        const auto metadataBefore = readText(metadataPath);
        auto sourceBytes = readText(sourcePath);
        const std::string marker = R"("normalTexture":{"index":1})";
        const auto markerPosition = sourceBytes.find(marker);
        check(markerPosition != std::string::npos, "Fixture material did not contain the texture slot marker");
        sourceBytes.insert(markerPosition + marker.size(), R"(,"occlusionTexture":{"index":1})");
        writeText(sourcePath, sourceBytes);
        expectThrow([&] { (void)project.workspace->load(project.model->id); },
                    "Read-only load accepted a new subresource locator");
        expectThrow([&] { (void)project.workspace->load(project.model->id); },
                    "Read-only reload accepted a new subresource locator");
        check(readText(metadataPath) == metadataBefore, "Read-only reload rewrote authored UUID metadata");
    });

    test("move glTF transaction preserves material, model and scene UUIDs across reload and redo", [&] {
        auto project = makeProject(output);
        const auto scene = makeProjectScene(project);
        const auto originalTree = authoredTree(project.root);
        const auto plan = planProjectFiles(project.root, FileOperation::Move, std::vector<std::string>{project.source},
                                           "Assets/Moved");
        const auto target = "Assets/Moved/" + utf8(fs::path(project.source).filename().wstring());
        auto transaction = applyPlan(project.root, plan);
        check(authoredTree(project.root) != originalTree && fs::exists(project.root / target),
              "Move was not published");
        auto moved = project.workspace->load(project.model->id);
        check(moved->source == target && moved->id == project.model->id, "Moved model identity changed");
        check(moved->materials.front()->values == scene.material, "Material override did not survive move");
        SceneDocument reopened;
        reopened.load(scene.path);
        check(reopened.scene.id == scene.id, "Scene UUID changed after model move");
        std::set<std::string> movedEntities;
        for (const auto handle : reopened.scene.entities())
            movedEntities.insert(reopened.scene.entity(handle).id.string());
        check(movedEntities == scene.entities, "Scene EntityIds changed after model move");
        transaction->undo();
        check(fs::exists(project.root / project.source) && !fs::exists(project.root / target), "Move Undo failed");
        transaction->redo();
        check(project.workspace->load(project.model->id)->source == target, "Move Redo changed source identity");
    });

    test("group copy transaction remaps internal resources and preserves external dependency IDs", [&] {
        auto project = makeProject(output);
        auto editedMaterial = *project.model->materials.front();
        editedMaterial.values.roughness = .23f;
        const auto originalMaterialId = editedMaterial.id.string();
        project.workspace->saveMaterial(editedMaterial);
        project.model = project.workspace->load(project.model->id);
        const auto sourceModelPath = resourcePath(project.root, project.source);
        const auto scenePath = project.root / "Assets/Source.scene.json";
        makeAssetScene(project, scenePath);
        applyPlan(project.root, enrollProjectFiles(project.root));
        const auto originalDependencyIds = dependencyIds(fs::path(sourceModelPath.wstring() + L".meta"));
        const auto packagePrefix = project.source.substr(0, project.source.find_last_of('/'));
        const auto internalPath = packagePrefix + "/mesh data.bin";
        const auto internalImage = packagePrefix + "/текстури/маска 100%.png";
        const std::vector<std::string> selected{relativePath(project.root, scenePath), project.source, internalPath,
                                                internalImage};
        const auto originalTree = authoredTree(project.root);
        const auto plan = planProjectFiles(project.root, FileOperation::Copy, selected, "Assets/Copied");
        auto transaction = applyPlan(project.root, plan);
        const auto copiedScene = project.root / "Assets/Copied/Source.scene.json";
        const auto copiedModelPath = "Assets/Copied/" + utf8(sourceModelPath.filename().wstring());
        const auto copiedModelId = findModelSourceAt(project.root, copiedModelPath);
        check(!copiedModelId.empty() && copiedModelId != project.model->id.string(),
              "Copied ModelSource UUID was reused");
        const auto copied = project.workspace->load(AssetId::parse(copiedModelId));
        check(copied->meshes.front()->id != project.model->meshes.front()->id, "Copied subasset UUID was reused");
        check(copied->materials.front()->id.string() != originalMaterialId, "Copied material UUID was reused");
        check(std::abs(copied->materials.front()->values.roughness - .23f) < .0001f,
              "Copied material override value did not reload");
        const auto copiedOverrideKeys = materialOverrideKeys(project.root / (copiedModelPath + ".meta"));
        check(copiedOverrideKeys.contains(copied->materials.front()->id.string()) &&
                  !copiedOverrideKeys.contains(originalMaterialId),
              "Copied material override UUID key was not remapped");
        check(sceneIdFrom(copiedScene) != sceneIdFrom(scenePath), "Copied scene UUID was reused");
        check(sceneEntityIds(copiedScene) == sceneEntityIds(scenePath), "Copied scene EntityIds changed");
        check(sceneSources(copiedScene).size() == 1 && sceneSources(copiedScene).front() == copiedModelId,
              "Copied scene modelSources were not remapped");
        check(sceneMeshId(copiedScene) == copied->meshes.front()->id.string(),
              "Copied mesh reference was not remapped");
        const auto copiedDependencyIds = dependencyIds(project.root / (copiedModelPath + ".meta"));
        check(copiedDependencyIds.at("mesh%20data.bin") != originalDependencyIds.at("mesh%20data.bin"),
              "Internal dependency UUID was not copied");
        const auto copiedPackage = fs::path(packagePrefix).lexically_relative(fs::path("Assets/Copied"));
        const auto copiedNormalUri = utf8((copiedPackage / "normal.png").generic_wstring());
        const auto copiedCheckerUri = utf8((copiedPackage / "checker.jpg").generic_wstring());
        check(copiedDependencyIds.at(copiedNormalUri) == originalDependencyIds.at("normal.png"),
              "External dependency UUID was unexpectedly remapped");
        check(copiedDependencyIds.at(copiedCheckerUri) == originalDependencyIds.at("checker.jpg"),
              "Second external dependency UUID was unexpectedly remapped");
        const auto copiedTree = authoredTree(project.root);
        transaction->undo();
        check(authoredTree(project.root) == originalTree, "Group copy Undo did not restore exact tree");
        transaction->redo();
        check(authoredTree(project.root) == copiedTree, "Group copy Redo did not restore exact tree");
        check(dependencyIds(project.root / (copiedModelPath + ".meta")) == copiedDependencyIds,
              "Redo issued different copied dependency IDs");
        const auto copiedAfterRedo = project.workspace->load(AssetId::parse(copiedModelId));
        check(std::abs(copiedAfterRedo->materials.front()->values.roughness - .23f) < .0001f,
              "Redo copied material override did not reload");
    });

    test("copy destination collision is rejected before any authored write", [&] {
        auto project = makeProject(output);
        const auto before = authoredTree(project.root);
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Copy, std::vector<std::string>{project.source},
                                       project.package);
            },
            "Copy destination collision was accepted");
        check(authoredTree(project.root) == before, "Collision preflight changed authored files");
        const auto scene = makeProjectScene(project);
        const auto sceneTree = authoredTree(project.root);
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Move, {relativePath(project.root, scene.path)},
                                       "Scenes", std::string("Main.json"));
            },
            "Scene rename dropped its required compound suffix");
        check(authoredTree(project.root) == sceneTree, "Invalid scene suffix preflight changed authored files");
    });

    test("external-reference GLB URI rewrite reloads after journal move", [&] {
        auto project = makeProject(output, "embedded.glb");
        const auto oldSource = project.source;
        const auto plan =
            planProjectFiles(project.root, FileOperation::Move, std::vector<std::string>{oldSource}, "Assets/GlbMoved");
        auto transaction = applyPlan(project.root, plan);
        const auto target = "Assets/GlbMoved/" + utf8(fs::path(oldSource).filename().wstring());
        const auto loaded = project.workspace->load(project.model->id);
        check(loaded->source == target && loaded->id == project.model->id, "Moved GLB did not reload by stable ID");
        transaction->undo();
        check(project.workspace->load(project.model->id)->source == oldSource, "GLB Undo did not restore source");
        transaction->redo();
        check(project.workspace->load(project.model->id)->source == target, "GLB Redo did not restore URI package");
    });

    test("case-only UTF-8 folder and file moves support Undo", [&] {
        auto project = makeProject(output);
        const auto folder = project.root / L"Assets/Україна";
        const auto file = folder / L"дані.txt";
        writeText(file, "before");
        auto folderTransaction =
            applyPlan(project.root, planProjectFiles(project.root, FileOperation::Move,
                                                     std::vector<std::string>{"Assets/Україна"}, "Assets", "україна"));
        check(fs::exists(project.root / L"Assets/україна/дані.txt"), "Case-only UTF-8 folder move failed");
        folderTransaction->undo();
        check(fs::exists(file), "Case-only folder Undo failed");
        folderTransaction->redo();
        auto fileTransaction =
            applyPlan(project.root, planProjectFiles(project.root, FileOperation::Move,
                                                     std::vector<std::string>{"Assets/україна/дані.txt"},
                                                     "Assets/україна", "ДАНІ.TXT"));
        check(fs::exists(project.root / L"Assets/україна/ДАНІ.TXT"), "Case-only UTF-8 file move failed");
        fileTransaction->undo();
        check(fs::exists(project.root / L"Assets/україна/дані.txt"), "Case-only file Undo failed");
    });

    test("nested Code and Config files are indexed and round-trip through Undo Redo", [&] {
        auto project = makeProject(output);
        const auto codeSource = project.root / L"Code/Nested/Behavior.cpp";
        const auto configSource = project.root / L"Config/Nested/renderer.json";
        writeText(codeSource, "// behavior bytes\n");
        writeText(configSource, "{\"quality\":\"high\"}\n");
        const auto codeBytes = readText(codeSource);
        const auto configBytes = readText(configSource);

        auto codeTransaction =
            applyPlan(project.root, planProjectFiles(project.root, FileOperation::Move,
                                                     std::vector<std::string>{"Code/Nested"}, "Code/Relocated"));
        const auto movedCode = project.root / L"Code/Relocated/Nested/Behavior.cpp";
        check(readText(movedCode) == codeBytes && !fs::exists(codeSource), "Nested Code move omitted authored bytes");
        codeTransaction->undo();
        check(readText(codeSource) == codeBytes && !fs::exists(movedCode), "Nested Code Undo did not restore bytes");
        codeTransaction->redo();
        check(readText(movedCode) == codeBytes, "Nested Code Redo did not restore bytes");

        auto configTransaction =
            applyPlan(project.root, planProjectFiles(project.root, FileOperation::Copy,
                                                     std::vector<std::string>{"Config/Nested"}, "Config/Relocated"));
        const auto copiedConfig = project.root / L"Config/Relocated/Nested/renderer.json";
        check(readText(configSource) == configBytes && readText(copiedConfig) == configBytes,
              "Nested Config copy omitted authored bytes");
        configTransaction->undo();
        check(readText(configSource) == configBytes && !fs::exists(copiedConfig),
              "Nested Config Undo did not restore exact bytes");
        configTransaction->redo();
        check(readText(copiedConfig) == configBytes, "Nested Config Redo did not restore bytes");
    });

    test("remove rejects referenced model source and dependency", [&] {
        auto project = makeProject(output);
        makeProjectScene(project);
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Remove, std::vector<std::string>{project.source},
                                       "");
            },
            "Referenced model source removal was accepted");
        applyPlan(project.root, enrollProjectFiles(project.root));
        const auto dependency = project.source.substr(0, project.source.find_last_of('/')) + "/mesh data.bin";
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Remove, std::vector<std::string>{dependency}, "");
            },
            "Referenced dependency removal was accepted");
    });

    test("group copy remaps typed behavior AssetRef and preserves opaque strings", [&] {
        auto project = makeProject(output);
        const auto scenePath = project.root / "Assets/Behavior.scene.json";
        makeBehaviorAssetScene(project, scenePath);
        applyPlan(project.root, enrollProjectFiles(project.root));
        const auto originalMesh = project.model->meshes.front()->id.string();
        const auto packagePrefix = project.source.substr(0, project.source.find_last_of('/'));
        const auto internalPath = packagePrefix + "/mesh data.bin";
        const auto internalImage = packagePrefix + "/текстури/маска 100%.png";
        const std::vector<std::string> selected{relativePath(project.root, scenePath), project.source, internalPath,
                                                internalImage};
        applyPlan(project.root, planProjectFiles(project.root, FileOperation::Copy, selected, "Assets/Copied"));
        const auto copiedScene = project.root / "Assets/Copied/Behavior.scene.json";
        const auto copiedModelPath = "Assets/Copied/" + utf8(fs::path(project.source).filename().wstring());
        const auto copiedModelId = findModelSourceAt(project.root, copiedModelPath);
        check(!copiedModelId.empty(), "Copied model source was not indexed");
        std::set<std::string> copiedMeshes;
        for (const auto& asset : inspectProjectAssets(project.root))
            if (asset.path == copiedModelPath && asset.kind == "Mesh")
                copiedMeshes.insert(asset.id);
        check(copiedMeshes.size() >= 2, "Copied mesh UUIDs were not indexed");
        const auto copiedMesh = sceneBehaviorAssetRef(copiedScene);
        const auto copiedMesh2 = sceneBehaviorAssetRef(copiedScene, "asset2");
        check(copiedMeshes.contains(copiedMesh) && copiedMesh != originalMesh,
              "Typed behavior AssetRef was not remapped");
        check(copiedMeshes.contains(copiedMesh2) && copiedMesh2 != project.model->meshes.at(1)->id.string(),
              "Second typed behavior AssetRef was not remapped");
        check(sceneBehaviorOpaqueString(copiedScene) == originalMesh,
              "Opaque behavior string was unexpectedly remapped");
    });

    test("remove protects assets referenced only by a behavior AssetRef", [&] {
        auto project = makeProject(output);
        makeBehaviorAssetScene(project, project.root / "Scenes/Behavior.scene.json");
        applyPlan(project.root, enrollProjectFiles(project.root));
        const auto before = authoredTree(project.root);
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Remove, std::vector<std::string>{project.source},
                                       "");
            },
            "Behavior AssetRef target removal was accepted");
        check(authoredTree(project.root) == before, "Behavior AssetRef removal preflight changed authored files");
    });

    test("duplicate UUID metadata is rejected by project index", [&] {
        auto project = makeProject(output);
        const auto sourcePath = resourcePath(project.root, project.source);
        const auto duplicate = sourcePath.parent_path() / "duplicate.gltf";
        fs::copy_file(sourcePath, duplicate);
        fs::copy_file(fs::path(sourcePath.wstring() + L".meta"), fs::path(duplicate.wstring() + L".meta"));
        expectThrow([&] { (void)inspectProjectAssets(project.root); }, "Duplicate UUID metadata was accepted");
    });

    test("duplicate UUID within one ModelSource is rejected without a planner write", [&] {
        auto project = makeProject(output);
        const auto metadataPath = fs::path(resourcePath(project.root, project.source).wstring() + L".meta");
        const auto before = readText(metadataPath);
        const auto firstSubasset = project.model->meshes.front()->id.string();
        const auto secondSubasset = project.model->meshes.at(1)->id.string();
        auto corrupted = before;
        const auto position = corrupted.find(secondSubasset);
        check(position != std::string::npos, "Test metadata did not contain the second subasset UUID");
        corrupted.replace(position, secondSubasset.size(), firstSubasset);
        writeText(metadataPath, corrupted);
        expectThrow([&] { (void)inspectProjectAssets(project.root); }, "Same-model duplicate UUID was accepted");
        check(readText(metadataPath) == corrupted, "Duplicate UUID inspection mutated authored metadata");
    });

    test("traversal, control roots and reparse boundaries are rejected", [&] {
        auto project = makeProject(output);
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Move, std::vector<std::string>{"../outside"},
                                       "Assets");
            },
            "Traversal source was accepted");
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Move,
                                       std::vector<std::string>{project.source + ".meta"}, "Assets");
            },
            "Direct metadata selection was accepted");
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Remove, std::vector<std::string>{"Assets"}, "");
            },
            "Protected Assets root removal was accepted");
        const auto hiddenFolder = project.root / "Assets/HiddenFolder";
        writeText(hiddenFolder / "keep.txt", "keep");
        writeText(hiddenFolder / "keep.bak", "backup");
        expectThrow(
            [&] {
                (void)planProjectFiles(project.root, FileOperation::Move,
                                       std::vector<std::string>{"Assets/HiddenFolder"}, "Assets/MovedHidden");
            },
            "Folder with a filtered backup entry was accepted");
        check(fs::exists(hiddenFolder / "keep.bak"), "Hidden backup was changed during preflight");
        const auto outside = project.area / "outside";
        fs::create_directories(outside);
        const auto junction = project.root / "Assets/Reparse";
        testHelpers::makeJunction(junction, outside);
        expectThrow([&] { (void)inspectProjectAssets(project.root); }, "Reparse directory was accepted");
        RemoveDirectoryW(junction.c_str());
    });

    test("external edit before Undo is detected without overwriting it", [&] {
        auto project = makeProject(output);
        const auto source = "Assets/Conflict.txt";
        writeText(project.root / source, "original");
        auto transaction =
            applyPlan(project.root, planProjectFiles(project.root, FileOperation::Move,
                                                     std::vector<std::string>{source}, "Assets/ConflictMoved"));
        const auto target = project.root / "Assets/ConflictMoved/Conflict.txt";
        writeText(target, "external edit");
        expectThrow([&] { transaction->undo(); }, "External edit before Undo was not rejected");
        check(readText(target) == "external edit", "Undo overwrote an external edit");
    });

    test("top-level Scenes topology guard rejects a new referenced scene before asset mutation", [&] {
        auto project = makeProject(output);
        fs::create_directories(project.root / "Scenes");
        const auto plan = planProjectFiles(project.root, FileOperation::Move, std::vector<std::string>{project.source},
                                           "Assets/Guarded");
        auto transaction = FileTransaction::prepare(project.root, plan);
        makeAssetScene(project, project.root / L"Scenes/New.scene.json");
        expectThrow([&] { transaction->redo(); }, "New top-level scene bypassed the topology guard");
        check(fs::exists(resourcePath(project.root, project.source)) &&
                  !fs::exists(project.root / L"Assets/Guarded" / fs::path(project.source).filename()),
              "Topology guard failure mutated the selected asset");
    });

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
