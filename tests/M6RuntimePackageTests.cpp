#include "assets/AssetIO.hpp"
#include "assets/PortableAssets.hpp"
#include "core/Diagnostics.hpp"
#include "project/BuildFileTree.hpp"
#include "runtime/BehaviorRuntime.hpp"
#include "runtime/PlayerGraphics.hpp"
#include "runtime/RuntimePackage.hpp"
#include "runtime/RuntimeSnapshot.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace proto;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kBehaviorType = "71000000-0000-4000-8000-000000000001";
constexpr std::string_view kBehaviorBinding = "71000000-0000-4000-8000-000000000002";

AssetId id(unsigned value) {
    std::string text = "71000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    text[text.size() - 1] = digits[value & 15];
    text[text.size() - 2] = digits[(value >> 4) & 15];
    return AssetId::parse(text);
}

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void rejects(const std::function<void()>& action, const char* label) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string("Expected rejection: ") + label);
}

std::wstring native(const fs::path& path) { return build_detail::extendedFilePath(path); }

void writeBytes(const fs::path& path, std::span<const uint8_t> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output), "fixture write failed");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(bool(output), "fixture write failed");
}

void writeText(const fs::path& path, std::string_view text) {
    writeBytes(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

std::vector<uint8_t> readBytes(const fs::path& path, size_t limit = 512 * 1024 * 1024) {
    return assetBytes(path, limit);
}

std::string readText(const fs::path& path) {
    const auto bytes = readBytes(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void copyFile(const fs::path& source, const fs::path& destination) {
    fs::create_directories(destination.parent_path());
    check(CopyFileW(native(source).c_str(), native(destination).c_str(), FALSE) != 0, "fixture file copy failed");
}

void copyFlatDirectory(const fs::path& source, const fs::path& destination) {
    fs::create_directories(destination);
    WIN32_FIND_DATAW data{};
    const auto search = native(source) + L"\\*";
    const auto handle = FindFirstFileW(search.c_str(), &data);
    check(handle != INVALID_HANDLE_VALUE, "fixture source directory is missing");
    struct CloseFind {
        HANDLE handle;
        ~CloseFind() {
            if (handle != INVALID_HANDLE_VALUE)
                FindClose(handle);
        }
    } close{handle};
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        check((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                  (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
              "SDK fixture contains an unexpected directory or reparse point");
        copyFile(source / std::wstring(name), destination / std::wstring(name));
    } while (FindNextFileW(handle, &data));
    check(GetLastError() == ERROR_NO_MORE_FILES, "fixture source directory enumeration failed");
}

std::string sdkBuildId(const fs::path& sdkRoot) {
    const auto document = readText(sdkRoot / "sdk.json");
    const auto key = document.find("\"sdkBuildId\"");
    check(key != std::string::npos, "SDK build ID is missing");
    auto cursor = document.find(':', key);
    check(cursor != std::string::npos, "SDK build ID field is malformed");
    while (++cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) {
    }
    check(cursor < document.size() && document[cursor] == '"', "SDK build ID is not a string");
    const auto begin = ++cursor;
    const auto end = document.find('"', begin);
    check(end != std::string::npos && end > begin, "SDK build ID is unterminated");
    return document.substr(begin, end - begin);
}

void copyRuntimeInputs(const fs::path& root, const fs::path& sdk, const fs::path& player) {
    fs::create_directories(root / "Config");
    fs::create_directories(root / "Data" / "shaders");
    fs::create_directories(root / "Licenses");
    copyFile(player, root / "ProtoPlayer.exe");
    copyFlatDirectory(sdk / "shaders", root / "Data" / "shaders");
    copyFlatDirectory(sdk / "licenses", root / "Licenses");
    const auto notices = fs::is_regular_file(sdk / "THIRD_PARTY_NOTICES.txt") ? sdk / "THIRD_PARTY_NOTICES.txt"
                                                                                : sdk / "THIRD_PARTY_NOTICES.md";
    copyFile(notices, root / notices.filename());
    writeText(root / "Config" / "graphics.json", encodePlayerGraphics(playerGraphicsProfile("Balanced")));
    // A regular root DLL is part of the complete immutable inventory. Its bytes
    // need not be loadable for this package-manifest contract test.
    writeText(root / "runtime-helper.dll", "M6 package DLL inventory fixture\n");
}

void removeTree(const fs::path& path) {
    std::error_code error;
    (void)build_detail::removeTree(path, error);
    check(!error, "fixture cleanup failed");
}

ModelBundle makeModel(const AssetId& modelId, const AssetId& meshId, const AssetId& materialId) {
    ModelBundle model;
    model.id = modelId;
    model.name = "M6 synthetic model";
    model.revision = "model-revision-m6";

    auto material = std::make_shared<MaterialAsset>();
    material->id = materialId;
    material->owner = modelId;
    material->name = "Synthetic material";
    material->revision = 3;
    material->values.baseColor = {.6f, .7f, .8f, 1.f};
    material->values.roughness = .9f;

    auto mesh = std::make_shared<MeshAsset>();
    mesh->id = meshId;
    mesh->name = "Synthetic triangle";
    mesh->revision = "mesh-revision-m6";
    mesh->vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 0}, {0, 0}},
                      {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {1, 0}, {1, 0}},
                      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 1}, {0, 1}}};
    mesh->indices = {0, 1, 2};
    mesh->parts = {{0, 3, materialId}};
    mesh->bounds = {{0, 0, 0}, {1, 1, 0}};
    mesh->triangles = {{0, 0}};
    mesh->bvh = {{{{0, 0, 0}, {1, 1, 0}}, 0, 1, 0, 0}};

    model.materials = {material};
    model.meshes = {mesh};
    model.nodes = {{"root", -1, glm::mat4(1), meshId}};
    return model;
}

struct RawProbe final : sdk::Behavior {
    static AssetId rawId;
    static std::vector<uint8_t> observed;
    void OnStart(sdk::BehaviorContext& context) override {
        const auto bytes = context.GetAssetBytes(rawId);
        observed.assign(bytes.begin(), bytes.end());
    }
};
AssetId RawProbe::rawId;
std::vector<uint8_t> RawProbe::observed;

Scene makeScene(const ModelBundle& model, const std::shared_ptr<const MaterialAsset>& overrideMaterial,
                const std::vector<AssetId>& rawIds, std::shared_ptr<const std::vector<uint8_t>> rawBytes) {
    Scene scene;
    scene.id = id(10);
    scene.name = "M6 package scene";
    scene.assets->publish(std::make_shared<const ModelBundle>(model));
    scene.assets->materials[overrideMaterial->id] = overrideMaterial;
    for (const auto rawId : rawIds)
        scene.assets->dataFiles.emplace(rawId, rawBytes);
    EntityRecord entity;
    entity.id = EntityId::parse("72000000-0000-4000-8000-000000000011");
    entity.name = "Raw probe entity";
    entity.mesh = MeshRenderer{model.meshes[0]->id, glm::vec4(1)};
    scene.create(entity);
    return scene;
}

void runPlayerValidation(const fs::path& packageRoot) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto commandText = L"\"" + native(packageRoot / "ProtoPlayer.exe") + L"\" --validate-package";
    std::vector<wchar_t> command(commandText.begin(), commandText.end());
    command.push_back(L'\0');
    const auto workingDirectory = native(packageRoot);
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         workingDirectory.c_str(), &startup, &process) != 0,
          "real Player validation process did not start");
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = STILL_ACTIVE;
    check(GetExitCodeProcess(process.hProcess, &exitCode) != 0, "real Player exit status failed");
    if (exitCode == STILL_ACTIVE) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("real Player validation timed out");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(exitCode == 0, "real Player package validation failed");
}

void replaceOnce(std::string& text, std::string_view from, std::string_view to, const char* message) {
    const auto at = text.find(from);
    check(at != std::string::npos, message);
    text.replace(at, from.size(), to);
}

void replaceResourceField(std::string& manifest, std::string_view field, std::string_view replacement) {
    const auto resources = manifest.find("\"resources\":[");
    check(resources != std::string::npos, "resource table missing");
    const auto at = manifest.find(field, resources);
    check(at != std::string::npos, "resource field missing");
    manifest.replace(at, field.size(), replacement);
}

void omitFileEntry(std::string& manifest, std::string_view path) {
    const auto marker = std::string("\"path\":\"") + std::string(path) + "\"";
    const auto at = manifest.find(marker);
    check(at != std::string::npos, "file inventory entry missing");
    auto begin = manifest.rfind('{', at);
    auto end = manifest.find('}', at);
    check(begin != std::string::npos && end != std::string::npos, "file inventory entry malformed");
    ++end;
    if (begin > 0 && manifest[begin - 1] == ',')
        --begin;
    else if (end < manifest.size() && manifest[end] == ',')
        ++end;
    manifest.erase(begin, end - begin);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::cerr << "usage: M6RuntimePackageTests <output-root> <sdk-root> <player-exe>\n";
        return 2;
    }
    const auto output = fs::absolute(argv[1]).lexically_normal();
    const auto sdk = fs::absolute(argv[2]).lexically_normal();
    const auto player = fs::absolute(argv[3]).lexically_normal();
    const auto fixture = output / utf8Path("m6-runtime-package-Україна spaces-" + Uuid::create().string());
    const auto sentinel = output / utf8Path("m6-runtime-sibling-sentinel-" + Uuid::create().string() + ".txt");
    fs::create_directories(output);
    writeText(sentinel, "preserve this sibling\n");

    unsigned passed{}, failed{};
    const auto test = [&](const char* name, const auto& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    std::shared_ptr<const std::vector<uint8_t>> rawBytes;
    std::vector<AssetId> rawIds;
    fs::path package;
    std::string buildId;
    std::string originalManifest;
    std::vector<uint8_t> originalModelBlob;
    fs::path modelBlobPath;
    RuntimePackage loaded;
    bool loadedPackage{};
    try {
        buildId = sdkBuildId(sdk);
        copyRuntimeInputs(fixture, sdk, player);
        const auto modelId = id(20), meshId = id(21), materialId = id(22);
        auto model = makeModel(modelId, meshId, materialId);
        auto overrideMaterial = std::make_shared<MaterialAsset>(*model.materials[0]);
        overrideMaterial->values.roughness = .2f;
        rawBytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'r', 'a', 'w', 0, 1, 2, 3, 4});
        rawIds = {id(30), id(31), id(32)};
        auto scene = makeScene(model, overrideMaterial, rawIds, rawBytes);

        test("synthetic portable model roundtrip", [&] {
            const auto decoded = decodePortableModel(encodePortableModel(model));
            check(decoded->id == model.id && decoded->meshes.size() == 1 && decoded->materials.size() == 1,
                  "synthetic model roundtrip changed topology");
            check(decoded->meshes[0]->vertices.size() == 3 && decoded->meshes[0]->bvh.size() == 1,
                  "synthetic model roundtrip lost geometry");
        });

        RuntimePackageMetadata metadata;
        metadata.projectId = id(40);
        metadata.name = "M6 Runtime Package Україна";
        metadata.sdkBuildId = buildId;
        metadata.executableName = "ProtoPlayer.exe";
        metadata.additionalAssets = {rawIds.front()};
        package = fixture;
        const auto stats = writeRuntimePackage(package, scene, metadata);
        check(stats.models == 1 && stats.rawAssets == rawIds.size(), "runtime package export stats mismatch");
        originalManifest = readText(package / "Data" / "runtime.json");
        auto exportedModel = *scene.assets->models.at(modelId);
        for (auto& material : exportedModel.materials)
            if (const auto found = scene.assets->materials.find(material->id); found != scene.assets->materials.end())
                material = found->second;
        const auto modelHash = sha256(encodePortableModel(exportedModel));
        modelBlobPath = package / "Data" / "blobs" / (modelHash + ".model");
        originalModelBlob = readBytes(modelBlobPath);

        test("loads package and preserves active material override dependencies", [&] {
            loaded = loadRuntimePackage(package, buildId);
            loadedPackage = true;
            check(loaded.projectId == metadata.projectId && loaded.stats.models == 1,
                  "runtime package identity/stats mismatch");
            const auto& loadedModel = *loaded.scene.assets->models.at(modelId);
            check(loadedModel.materials[0]->values.roughness == .2f &&
                      loaded.scene.assets->materials.at(materialId)->values.roughness == .2f,
                  "active catalog material override was not exported into the model");
            check(loaded.stats.resources >= 4, "runtime resource closure is incomplete");
            for (const auto rawId : rawIds)
                check(loaded.scene.assets->dataFiles.contains(rawId), "raw package resource is missing");
            check(loaded.scene.assets->dataFiles.at(rawIds[0]) == loaded.scene.assets->dataFiles.at(rawIds[1]) &&
                      loaded.scene.assets->dataFiles.at(rawIds[1]) == loaded.scene.assets->dataFiles.at(rawIds[2]),
                  "raw IDs did not share one immutable payload pointer");
        });

        test("real Player validates the copied package", [&] { runPlayerValidation(package); });

        test("raw data is available during an actual behavior OnStart", [&] {
            RawProbe::rawId = rawIds.front();
            RawProbe::observed.clear();
            // The copied core Player deliberately has an empty registry.
            // Attach the test-only behavior to a private in-memory scene.
            auto behaviorScene = Scene::fromSnapshot(loaded.scene.snapshot(), loaded.scene.assets);
            auto entity = behaviorScene.record(behaviorScene.entities().front());
            entity.behaviors = {{BehaviorBindingId::parse(kBehaviorBinding), BehaviorTypeId::parse(kBehaviorType), true, {}}};
            behaviorScene.edit(entity);
            sdk::BehaviorRegistry registry;
            registry.Add<RawProbe>({BehaviorTypeId::parse(kBehaviorType), "RawProbe", {}});
            BehaviorRuntime runtime(behaviorScene, registry, [](std::string_view) {});
            runtime.start();
            runtime.stop();
            check(RawProbe::observed == *rawBytes, "Behavior OnStart did not receive raw asset bytes");
        });

        test("package pins block root rename while live", [&] {
            const auto renamed = package.parent_path() / (package.filename().wstring() + L"-renamed");
            check(!MoveFileExW(native(package).c_str(), native(renamed).c_str(), MOVEFILE_REPLACE_EXISTING),
                  "live package pins allowed a root rename");
        });

        check(loadedPackage, "package was not loaded before mutation tests");
        loaded.pins.reset();
        loadedPackage = false;

        test("missing or corrupt model blobs are rejected", [&] {
            check(DeleteFileW(native(modelBlobPath).c_str()) != 0, "model blob removal failed");
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "missing model blob");
            writeBytes(modelBlobPath, originalModelBlob);
            auto corrupt = originalModelBlob;
            corrupt[corrupt.size() / 2] ^= 0x5a;
            writeBytes(modelBlobPath, corrupt);
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "corrupt model blob");
            writeBytes(modelBlobPath, originalModelBlob);
        });

        test("wrong SDK identity is rejected", [&] {
            rejects([&] { (void)loadRuntimePackage(package, buildId + "-wrong"); }, "wrong SDK identity");
        });

        test("altered resource type and dependency tables are rejected", [&] {
            auto altered = originalManifest;
            replaceResourceField(altered, "\"type\":\"Mesh\"", "\"type\":\"Texture\"");
            writeText(package / "Data" / "runtime.json", altered);
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "altered resource type");
            writeText(package / "Data" / "runtime.json", originalManifest);
            altered = originalManifest;
            const auto bogusDependencies = std::string("\"dependencies\":[\"") + rawIds.back().string() + "\"]";
            const auto resources = altered.find("\"resources\":[");
            const auto at = altered.find("\"dependencies\":[", resources);
            check(at != std::string::npos, "resource dependency field missing");
            const auto end = altered.find(']', at);
            check(end != std::string::npos, "resource dependency array malformed");
            altered.replace(at, end - at + 1, bogusDependencies);
            writeText(package / "Data" / "runtime.json", altered);
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "altered resource dependencies");
            writeText(package / "Data" / "runtime.json", originalManifest);
        });

        test("omitted DLL inventory entry and unlisted files are rejected", [&] {
            auto omitted = originalManifest;
            omitFileEntry(omitted, "runtime-helper.dll");
            writeText(package / "Data" / "runtime.json", omitted);
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "omitted DLL inventory entry");
            writeText(package / "Data" / "runtime.json", originalManifest);
            writeText(package / "unlisted-extra.txt", "unlisted package file\n");
            rejects([&] { (void)loadRuntimePackage(package, buildId); }, "unlisted package file");
            check(DeleteFileW(native(package / "unlisted-extra.txt").c_str()) != 0, "unlisted file cleanup failed");
        });

        test("declared mutable logs and user settings are accepted", [&] {
            writeText(package / "Player.log", "mutable log\n");
            writeText(package / "Config" / "graphics.user.json", "user settings\n");
            writeText(package / "Config" / "graphics.user.json.bak", "backup settings\n");
            const auto temporary = package / "Config" / (".tmp-" + Uuid::create().string());
            const auto backup = package / "Config" / (".tmp-" + Uuid::create().string() + "-backup");
            writeText(temporary, "atomic temporary\n");
            writeText(backup, "atomic backup\n");
            const auto mutableLoaded = loadRuntimePackage(package, buildId);
            check(mutableLoaded.projectId == metadata.projectId, "mutable package reload failed");
        });

        test("private Play snapshot carries raw bytes", [&] {
            auto packageLoaded = loadRuntimePackage(package, buildId);
            auto play = stagePlaySnapshot(package, packageLoaded.scene, buildId);
            const auto playScene = loadRuntimeSnapshot(play.manifest, buildId);
            check(playScene.assets->dataFiles.at(rawIds[0]) == playScene.assets->dataFiles.at(rawIds[1]) &&
                      *playScene.assets->dataFiles.at(rawIds[0]) == *rawBytes,
                  "private Play snapshot lost/shared raw bytes incorrectly");
            // Release both pin sets before fixture cleanup or any later file mutation.
            packageLoaded.pins.reset();
            play.pins.reset();
        });

        removeTree(fixture);
        check(readText(sentinel) == "preserve this sibling\n", "sibling sentinel was damaged");
        check(DeleteFileW(native(sentinel).c_str()) != 0, "sibling sentinel cleanup failed");

        test("writes a static textured MASK package for native graphics QA", [&] {
            const auto gpuRoot = output / ("m6-mask-package-" + Uuid::create().string());
            copyRuntimeInputs(gpuRoot, sdk, player);
            auto maskModel = makeModel(id(60), id(61), id(62));
            auto texture = std::make_shared<TextureAsset>();
            texture->id = id(63);
            auto pixels = std::make_shared<TexturePixels>();
            pixels->hash = "original-m6-mask-fixture";
            pixels->srgb = true;
            for (uint32_t size = 64; size; size /= 2) {
                ImageMip mip{size, size, std::vector<uint8_t>(size_t(size) * size * 4)};
                for (uint32_t y = 0; y < size; ++y)
                    for (uint32_t x = 0; x < size; ++x) {
                        const auto offset = (size_t(y) * size + x) * 4;
                        mip.rgba[offset] = 70;
                        mip.rgba[offset + 1] = 205;
                        mip.rgba[offset + 2] = 125;
                        mip.rgba[offset + 3] = (size == 1 || x >= size / 2) ? 255 : 0;
                    }
                pixels->mips.push_back(std::move(mip));
            }
            texture->pixels = pixels;
            maskModel.textures = {texture};
            auto material = std::make_shared<MaterialAsset>(*maskModel.materials[0]);
            material->values.mask = true;
            material->values.doubleSided = true;
            material->values.metallic = 0;
            material->textures[0].texture = texture->id;
            maskModel.materials = {material};
            auto staticScene = Scene::demo();
            staticScene.name = "M6 static MASK and shadow cache";
            staticScene.assets->publish(std::make_shared<const ModelBundle>(maskModel));
            staticScene.modelSources = {maskModel.id};
            EntityRecord mask;
            mask.id = EntityId::create();
            mask.name = "Static MASK caster";
            mask.mesh = MeshRenderer{maskModel.meshes[0]->id, glm::vec4(1)};
            mask.transform.position = {-2, .3f, 0};
            mask.transform.scale = {3, 3, 3};
            staticScene.create(mask);
            EntityRecord sun;
            sun.id = EntityId::create();
            sun.name = "Sun";
            sun.directionalLight = DirectionalLight{};
            sun.transform.rotation = glm::quat(glm::radians(glm::vec3(-50, -25, 0)));
            staticScene.create(sun);
            EntityRecord point;
            point.id = EntityId::create();
            point.name = "Point";
            point.pointLight = PointLight{};
            point.transform.position = {1, 3, 2};
            staticScene.create(point);
            const RuntimePackageMetadata gpuMetadata{id(65), "M6 Static MASK", buildId, "ProtoPlayer.exe", {}};
            (void)writeRuntimePackage(gpuRoot, staticScene, gpuMetadata);
            (void)loadRuntimePackage(gpuRoot, buildId);
            writeText(output / "m6-mask-package.json", "{\"root\":" + jsonString(utf8(gpuRoot.wstring())) + "}\n");
        });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            if (loadedPackage)
                loaded.pins.reset();
            if (!fixture.empty() && fs::exists(fixture))
                removeTree(fixture);
        } catch (...) {
        }
        return 1;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
