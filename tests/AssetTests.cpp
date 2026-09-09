#include "AssetFixtures.hpp"
#include "editor/SceneDocument.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <functional>
#include <cmath>
#include <cstring>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    bool rejected{};
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Expected rejection");
}
void put(const std::filesystem::path& file, std::span<const uint8_t> bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(bool(out), "Fixture write");
}
void put(const std::filesystem::path& file, const std::string& bytes) {
    put(file, {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
}
std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "Replacement token missing");
    text.replace(at, from.size(), to);
    return text;
}
std::filesystem::path cached(const std::filesystem::path& root, const ModelBundle& model) {
    return root / ".proto/cache" / (sha256(model.revision + model.id.string()) + ".bin");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--example") {
        const auto root = std::filesystem::absolute(argv[2]);
        const auto scene = root / "MaterialLab.scene.json";
        if (std::filesystem::exists(scene)) {
            std::cerr << "Example already exists; refusing to overwrite it\n";
            return 1;
        }
        const auto fixturesPath = executableDirectory() / "test-results/m2-fixtures";
        fixtures::create(fixturesPath);
        SceneDocument document;
        document.save(scene);
        const auto cube = document.scene.meshes()[0];
        document.erase(document.scene.entity(cube).id);
        const auto model = document.workspace()->importFile(fixturesPath / "MaterialSpheres.glb");
        document.instantiate(model);
        for (const auto handle : document.scene.meshes()) {
            auto record = document.scene.record(handle);
            if (record.name == "Золото") {
                record.mesh->materials = {model->materials[1]->id};
                document.edit(record);
            }
            if (record.name == "Шорстка мідь") {
                record.mesh->materials = {model->materials[2]->id};
                document.edit(record);
            }
        }
        document.scene.name = "Proto Engine · Матеріали";
        document.save(scene);
        std::filesystem::copy_file(fixturesPath / "MANIFEST.md", root / "SOURCE_MANIFEST.md");
        std::cout << "Created " << utf8(scene.wstring()) << '\n';
        return 0;
    }
    const auto output =
        std::filesystem::absolute(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("test-results"));
    const auto source = output / "m2-fixtures";
    fixtures::create(source);
    const auto root = output / L"Ресурси з пробілами 100%" / Uuid::create().string();
    std::filesystem::create_directories(root);
    auto workspace = std::make_shared<AssetWorkspace>(root);
    std::shared_ptr<const ModelBundle> model;
    unsigned passed{}, failed{};
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
    test("SHA256 and scoped UTF-8/percent-encoded URIs", [&] {
        check(sha256(std::string("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA256 oracle");
        check(assetUri(source, "текстури/маска%20100%25.png").filename() == L"маска 100%.png", "UTF8/percent URI");
        for (const std::string uri : {"../escape.bin", "%2e%2e/escape.bin", "C:/file.bin", "//host/file", "a%00b",
                                      "https://example.org/a", "bad%2", "file%3ab"})
            rejects([&] { assetUri(source, uri); });
        check(dataUri("data:application/octet-stream;base64,AAECAw==") == std::vector<uint8_t>({0, 1, 2, 3}),
              "Data URI bytes");
        rejects([] { dataUri("data:image/png;base64,=AAA"); });
    });
    test("glTF external bin, PNG/JPEG/data URI, hierarchy and material slots", [&] {
        model = workspace->importFile(source / "probes.gltf");
        check(model->meshes.size() == 6 && model->nodes.size() == 10 && model->materials.size() == 7, "Model topology");
        check(model->meshes[3]->parts.size() == 2 && model->textures.size() == 5 && model->decodedImages == 4,
              "Material slots/decode dedup");
        const auto installed = resourcePath(root, model->source);
        check(sha256(assetBytes(installed.parent_path() / "mesh data.bin")) ==
                  sha256(assetBytes(source / "mesh data.bin")),
              "Bin copied");
        check(std::filesystem::exists(installed.parent_path() / L"текстури/маска 100%.png"),
              "Texture dependency copied");
        check(model->materials[4]->textures[0].texCoord == 1 && model->materials[4]->textures[0].offset.x == 1,
              "UV transform/override");
        check(model->materials[2]->values.mask && model->materials[2]->values.doubleSided &&
                  !model->materials[5]->values.mask,
              "Alpha and culling modes");
    });
    test("generated normals and Mikk tangents with normalized attributes", [&] {
        for (const auto& mesh : model->meshes)
            for (const auto& vertex : mesh->vertices) {
                check(std::abs(glm::length(vertex.normal) - 1) < .0001f && vertex.normal.z > .99f, "Generated normal");
                check(std::abs(glm::dot(vertex.normal, glm::vec3(vertex.tangent))) < .001f, "Orthogonal tangent");
                check(std::abs(glm::length(glm::vec3(vertex.tangent)) - 1) < .001f && std::abs(vertex.tangent.w) == 1,
                      "Tangent frame");
                check(vertex.color == glm::vec4(1) && vertex.uv1.x >= 0 && vertex.uv1.x <= 1, "Normalized accessors");
            }
        check(model->meshes[0]->vertices[0].tangent.w == -1, "glTF top-left UV handedness");
    });
    test("color/data spaces and normalized normal mip chain", [&] {
        const auto& normal = model->textures[0]->pixels;
        check(!normal->srgb && normal->mips.size() == 11, "Normal mip levels");
        const auto& last = normal->mips.back().rgba;
        const glm::vec3 n = glm::vec3(last[0], last[1], last[2]) / 127.5f - 1.0f;
        check(std::abs(glm::length(n) - 1) < .015f, "Mip normal normalization");
        check(model->textures[1]->pixels->srgb && !model->textures[2]->pixels->srgb, "Color versus MR texture role");
        const auto linearMip = makeMips(2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, true, false);
        check(linearMip.back().rgba[0] >= 187 && linearMip.back().rgba[0] <= 188, "sRGB linear filtering oracle");
        const auto odd = makeMips(3, 1, {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255}, false, false);
        check(odd.back().rgba[0] == 85, "Odd texture dimensions retain the last column");
        const auto solid = makeMips(2, 2, std::vector<uint8_t>(16, 255), false, false, .5f);
        check(solid.back().rgba[3] >= 128, "MASK mip remains above its quantized alpha cutoff");
        rejects([] { makeMips(0, 1, {}, false, false); });
    });
    test("cache hit decodes zero images and duplicate import reuses UUIDs", [&] {
        const auto copy = workspace->load(model->id), duplicate = workspace->importFile(source / "probes.gltf");
        check(copy->fromCache && copy->decodedImages == 0 && duplicate->id == model->id, "Prepared cache reuse");
        check(copy->meshes[0]->id == model->meshes[0]->id && copy->materials[0]->id == model->materials[0]->id,
              "Stable subresource IDs");
        check(workspace->rebuildRegistry().size() == 1, "Duplicate physical import");
    });
    test("GLB bufferView embedded PNG and mixed dependencies", [&] {
        auto embedded = workspace->importFile(source / "embedded.glb");
        check(embedded->meshes.size() == 6 && embedded->decodedImages == 4, "GLB imported");
        check(embedded->textures[1]->pixels->hash == model->textures[1]->pixels->hash,
              "Embedded image bytes match PNG");
        check(workspace->load(embedded->id)->decodedImages == 0, "GLB cached");
    });
    test("Windows uppercase model extensions import and reopen", [&] {
        const auto uppercase = source / "UPPER.GLB";
        std::filesystem::copy_file(source / "embedded.glb", uppercase,
                                   std::filesystem::copy_options::overwrite_existing);
        AssetWorkspace capitalized(root / "CaseTest");
        const auto imported = capitalized.importFile(uppercase);
        check(capitalized.load(imported->id)->fromCache, "Uppercase .GLB source missing from registry");
        std::filesystem::remove(uppercase);
    });
    test("deleted and corrupt cooked cache rebuild without changing IDs", [&] {
        const auto path = cached(root, *model);
        put(path, "broken cache");
        auto repaired = workspace->load(model->id);
        check(!repaired->fromCache && repaired->meshes[0]->id == model->meshes[0]->id, "Corrupt cache repair");
        std::filesystem::remove(path);
        repaired = workspace->load(model->id);
        check(!repaired->fromCache && repaired->decodedImages == 4, "Deleted cache rebuild");
    });
    test("scene instances share meshes; imported hierarchy Undo/Redo is atomic", [&] {
        SceneDocument document;
        document.scene = Scene{};
        document.save(root / "instances.scene.json");
        const auto before = encodeScene(document.scene);
        document.instantiate(model);
        document.undo();
        check(encodeScene(document.scene) == before && !document.dirty(), "Import undo/savepoint");
        document.redo();
        const auto geometry = document.scene.assets->meshes.at(model->meshes[0]->id);
        document.instantiate(model);
        check(document.scene.meshes().size() == 18 && document.scene.assets->meshes.size() == 6 &&
                  document.scene.assets->meshes.at(geometry->id) == geometry,
              "Instances share CPU geometry");
        document.save(document.path());
        SceneDocument reopened;
        reopened.load(document.path());
        check(encodeScene(reopened.scene) == encodeScene(document.scene), "Scene UUID/material reference roundtrip");
        for (const auto& [id, sourceModel] : reopened.scene.assets->models)
            check(sourceModel->decodedImages == 0, "Scene load uses cache");
        std::filesystem::create_directories(root / "Scenes");
        reopened.save(root / "Scenes/copy.scene.json");
        SceneDocument copied;
        copied.load(reopened.path());
        check(copied.scene.meshes().size() == 18 && copied.scene.assetRoot == "..", "Save As retains resource root");
    });
    test("material gesture persists once, shares across instances and undoes", [&] {
        SceneDocument document;
        document.load(root / "instances.scene.json");
        const auto id = model->materials[0]->id;
        const auto original = document.scene.assets->materials.at(id)->values;
        const auto history = document.undoCount();
        document.beginMaterialGesture(id);
        for (int i = 0; i < 20; ++i) {
            auto values = original;
            values.roughness = .1f + float(i) * .02f;
            document.previewMaterial(id, values);
        }
        document.endGesture();
        check(document.undoCount() == history + 1, "One material gesture = one Undo");
        const auto modified = document.scene.assets->materials.at(id)->values;
        document.instantiate(document.scene.assets->models.at(model->id));
        check(document.scene.assets->materials.at(id)->values == modified,
              "New instance does not reset material edits");
        document.undo();
        document.undo();
        check(document.scene.assets->materials.at(id)->values == original, "Material Undo");
        document.redo();
        check(workspace->load(model->id)->materials[0]->values == modified, "Meta overrides reopen");
        check(sha256(assetBytes(resourcePath(root, model->source))) == sha256(assetBytes(source / "probes.gltf")),
              "Authored glTF untouched");
        document.undo();
    });
    test("BVH picking respects MASK holes, mirrored objects and slot overrides", [&] {
        Scene scene;
        scene.assets->publish(model);
        EntityRecord item;
        item.id = EntityId::create();
        item.name = "MASK";
        item.mesh = MeshRenderer{model->meshes[2]->id, glm::vec4(1)};
        const auto handle = scene.create(item);
        scene.update();
        check(!scene.pick({{-.4f, 0, 2}, {0, 0, -1}}) && scene.pick({{.4f, 0, 2}, {0, 0, -1}}).has_value(),
              "MASK alpha-aware picking");
        item.transform.scale.x = -1;
        scene.edit(item);
        check(scene.pick({{-.4f, 0, 2}, {0, 0, -1}}).has_value() && !scene.pick({{.4f, 0, 2}, {0, 0, -1}}),
              "Mirrored MASK");
        item.mesh->materials = {model->materials[1]->id};
        scene.edit(item);
        check(scene.pick({{.4f, 0, 2}, {0, 0, -1}}).has_value(), "Slot material overrides");
        item.mesh->materials = {AssetId::create()};
        rejects([&] { scene.edit(item); });
        check(scene.mesh(handle)->materials[0] == model->materials[1]->id, "Failed slot edit preserved scene");
    });
    test("model plus .meta move rebuilds registry and preserves saved scenes", [&] {
        auto oldFolder = resourcePath(root, model->source).parent_path();
        const auto destination = root / "Assets" / L"Переміщена модель 100%";
        check(std::filesystem::weakly_canonical(oldFolder).parent_path() ==
                      std::filesystem::weakly_canonical(root / "Assets") &&
                  destination.parent_path() == root / "Assets",
              "Scoped move");
        std::filesystem::rename(oldFolder, destination);
        model = workspace->load(model->id);
        check(model->source.find("Переміщена") != std::string::npos, "Registry rebuilt after move");
        SceneDocument document;
        document.load(root / "instances.scene.json");
        check(document.scene.meshes().size() == 18, "References survived move");
    });
    test("geometry reimport retains IDs; structural remapping is rejected", [&] {
        const auto file = resourcePath(root, model->source);
        const auto bin = file.parent_path() / "mesh data.bin";
        auto original = assetBytes(bin), changed = original;
        float value;
        std::memcpy(&value, changed.data(), 4);
        value -= .03f;
        std::memcpy(changed.data(), &value, 4);
        put(bin, changed);
        const auto reimport = workspace->load(model->id);
        check(reimport->revision != model->revision && reimport->meshes[0]->id == model->meshes[0]->id,
              "Reimport UUID identity");
        put(bin, original);
        model = workspace->load(model->id);
        const auto authored = readDocument(file);
        put(file, replaced(authored, "Mesh 0", "Different mesh"));
        rejects([&] { workspace->load(model->id); });
        put(file, authored);
        check(workspace->load(model->id)->id == model->id, "Valid source recovers");
    });
    test("duplicate detection verifies current source bytes, not stale metadata", [&] {
        const auto bin = resourcePath(root, model->source).parent_path() / "mesh data.bin";
        const auto original = assetBytes(bin);
        auto changed = original;
        float value = -.91f;
        std::memcpy(changed.data(), &value, sizeof(value));
        put(bin, changed);
        const auto imported = workspace->importFile(source / "probes.gltf");
        check(imported->id != model->id && imported->revision == model->revision,
              "Stale metadata returned a different model");
        put(bin, original);
        model = workspace->load(model->id);
    });
    test("duplicate UUID metadata detected instead of rebinding", [&] {
        const auto file = resourcePath(root, model->source);
        auto meta = file;
        meta += L".meta";
        const auto copy = file.parent_path() / "duplicate.gltf";
        auto copyMeta = copy;
        copyMeta += L".meta";
        std::filesystem::copy_file(file, copy);
        std::filesystem::copy_file(meta, copyMeta);
        rejects([&] { workspace->rebuildRegistry(); });
        std::filesystem::remove(copyMeta);
        std::filesystem::remove(copy);
        check(workspace->load(model->id)->id == model->id, "Registry recovery");
    });
    test("missing/corrupt dependencies and unsupported required extensions do not publish", [&] {
        const auto original = readDocument(source / "probes.gltf");
        const auto bad = source / "bad.gltf";
        for (const auto& bytes :
             {replaced(original, "mesh%20data.bin", "missing.bin"), replaced(original, "normal.png", "checker.jpg.bad"),
              replaced(original, "\"scene\":0", "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],\"scene\":0"),
              replaced(original, "mesh%20data.bin", "%2e%2e/escape.bin"),
              replaced(original, "\"MASK\"", "\"BLEND\"")}) {
            put(bad, bytes);
            const auto count = workspace->rebuildRegistry().size();
            rejects([&] { workspace->importFile(bad); });
            check(workspace->rebuildRegistry().size() == count, "Failed import published metadata");
        }
        put(source / "corrupt.png", "invalid PNG");
        put(bad, replaced(original, "normal.png", "corrupt.png"));
        rejects([&] { workspace->importFile(bad); });
        std::filesystem::remove(bad);
        std::filesystem::remove(source / "corrupt.png");
        SceneDocument document;
        document.load(root / "instances.scene.json");
        const auto snapshot = encodeScene(document.scene);
        const auto normal = resourcePath(root, model->source).parent_path() / "normal.png";
        const auto image = assetBytes(normal);
        put(normal, "broken PNG");
        rejects([&] { document.load(root / "instances.scene.json"); });
        check(encodeScene(document.scene) == snapshot, "Failed scene load mutated live scene");
        put(normal, image);
    });
    test("worker prepares an immutable bundle without touching the live catalog", [&] {
        ImportJob job;
        job.reload(workspace, model->id);
        rejects([&] { job.reload(workspace, model->id); });
        const auto started = Clock::now();
        std::shared_ptr<const ModelBundle> result;
        while (!(result = job.take()) && milliseconds(started) < 20000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(result && result->fromCache && result->decodedImages == 0 && !job.busy(), "Worker cache result");
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
