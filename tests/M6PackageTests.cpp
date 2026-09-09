#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "project/BuildFileTree.hpp"
#include "project/AssetFilePlanner.hpp"
#include "project/ManagedProcess.hpp"
#include "project/ProjectPackage.hpp"
#include "project/ProjectSession.hpp"
#include "runtime/RuntimePackage.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace proto;
namespace fs = std::filesystem;

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output), "M6 fixture write failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(output), "M6 fixture write failed");
}

void put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    check(offset + 2 <= bytes.size(), "synthetic PE write overflow");
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    check(offset + 4 <= bytes.size(), "synthetic PE write overflow");
    for (unsigned i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

void putText(std::vector<uint8_t>& bytes, size_t offset, std::string_view value) {
    check(offset + value.size() + 1 <= bytes.size(), "synthetic PE string overflow");
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    bytes[offset + value.size()] = 0;
}

std::vector<uint8_t> syntheticPeWithNormalAndDelayImports() {
    // One PE32+ section deliberately has distinct virtual/raw addresses and
    // sizes. Import and delay-import descriptors are separated and each has a
    // zero descriptor terminator.
    std::vector<uint8_t> bytes(0x500, 0);
    constexpr size_t pe = 0x80, optional = pe + 24, directory = optional + 112, section = optional + 0xf0;
    put16(bytes, 0, 0x5a4d);
    put32(bytes, 0x3c, static_cast<uint32_t>(pe));
    put32(bytes, pe, 0x00004550);
    put16(bytes, pe + 4, 0x8664);
    put16(bytes, pe + 6, 1);
    put16(bytes, pe + 20, 0xf0);
    put16(bytes, optional, 0x20b);
    put32(bytes, optional + 108, 16);
    put32(bytes, directory + 8 * 1, 0x1010);
    put32(bytes, directory + 8 * 1 + 4, 40);
    put32(bytes, directory + 8 * 13, 0x1070);
    put32(bytes, directory + 8 * 13 + 4, 64);
    putText(bytes, section, ".rdata");
    put32(bytes, section + 8, 0x180);  // virtual size
    put32(bytes, section + 12, 0x1000); // virtual address
    put32(bytes, section + 16, 0x240); // raw size
    put32(bytes, section + 20, 0x200); // raw file offset

    constexpr size_t normal = 0x210, delay = 0x270;
    put32(bytes, normal + 12, 0x1050);
    putText(bytes, 0x250, "normal-test.dll");
    put32(bytes, delay, 1); // RVA-based delay descriptor
    put32(bytes, delay + 4, 0x1060);
    putText(bytes, 0x260, "delay-test.dll");
    return bytes;
}

void writeBinary(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output), "M6 synthetic PE write failed");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(bool(output), "M6 synthetic PE write failed");
}

std::string read(const fs::path& path) {
    const auto bytes = assetBytes(path, 512 * 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void removeTree(const fs::path& path) {
    std::error_code error;
    (void)build_detail::removeTree(path, error);
    check(!error, "M6 fixture cleanup failed");
}

std::string sdkConfiguration(const fs::path& root) {
    const auto bytes = read(root / "sdk.json");
    const auto key = bytes.find("\"configuration\"");
    check(key != std::string::npos, "SDK configuration is missing");
    auto cursor = bytes.find(':', key);
    check(cursor != std::string::npos, "SDK configuration is malformed");
    while (++cursor < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[cursor]))) {
    }
    check(cursor < bytes.size() && bytes[cursor] == '"', "SDK configuration is not a string");
    const auto begin = ++cursor;
    const auto end = bytes.find('"', begin);
    check(end != std::string::npos, "SDK configuration is unterminated");
    return bytes.substr(begin, end - begin);
}

bool hasPreviousPackage(const fs::path& output) {
    const auto parent = output.parent_path();
    for (const auto& entry : fs::directory_iterator(parent))
        if (entry.is_directory() && entry.path().filename().wstring().starts_with(output.filename().wstring() + L".previous-"))
            return true;
    return false;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "usage: M6PackageTests <output-root> <sdk-root>\n";
        return 2;
    }
    const auto output = fs::absolute(argv[1]).lexically_normal();
    const auto sdk = fs::absolute(argv[2]).lexically_normal();
    const auto fixture = output / utf8Path("m6-package-Україна spaces-" + Uuid::create().string());
    fs::create_directories(output);
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

    std::shared_ptr<ProjectSession> session;
    fs::path package;
    try {
        std::string longName = "Я";
        for (int i = 0; i < 80; ++i)
            longName += "Я";
        session = ProjectSession::create(fixture, longName);
        write(fixture / "Assets" / "runtime-data.bin", "portable additional data\n");
        auto manifest = read(fixture / "project.proto.json");
        const auto marker = std::string("\"additionalAssets\":[]");
        const auto at = manifest.find(marker);
        check(at != std::string::npos, "M6 fixture manifest did not contain additionalAssets");
        manifest.replace(at, marker.size(), "\"additionalAssets\":[\"Assets/runtime-data.bin\"]");
        write(fixture / "project.proto.json", manifest);
        session->refresh();
        const auto rawPath = fixture / "Assets" / "runtime-data.bin";
        const auto rawSidecar = fs::path(rawPath.wstring() + L".meta");
        check(fs::is_regular_file(rawSidecar), "additional raw asset sidecar was not persisted");
        const auto sidecarDocument = JsonDoc(read(rawSidecar));
        const auto rawId = AssetId::parse(str(get(sidecarDocument.root(), "assetId")));
        const auto canonicalManifest = read(fixture / "project.proto.json");
        check(canonicalManifest.find("Assets/runtime-data.bin") == std::string::npos,
              "additional asset path was not canonicalized to its persistent ID");
        check(canonicalManifest.find(rawId.string()) != std::string::npos,
              "canonical additional asset ID is missing from the manifest");
        const auto renamedPath = fixture / "Assets" / "runtime-renamed.bin";
        const auto renamedSidecar = fs::path(renamedPath.wstring() + L".meta");
        fs::rename(rawPath, renamedPath);
        fs::rename(rawSidecar, renamedSidecar);
        session->refresh();
        const auto relocatedAssets = inspectProjectAssets(fixture);
        const auto renamed = std::find_if(relocatedAssets.begin(), relocatedAssets.end(), [&](const auto& asset) {
            return asset.id == rawId.string();
        });
        check(renamed != relocatedAssets.end() && renamed->path == "Assets/runtime-renamed.bin",
              "renaming a sidecar-backed additional asset lost its persistent identity");
        write(fixture / "Code" / "Behaviors.cpp", R"cpp(#include <Proto/Behavior.hpp>
using namespace proto;
using namespace proto::sdk;
const auto packageGuardId = BehaviorTypeId::parse("60000000-0000-4000-8000-000000000001");
class PackageGuard final : public Behavior {};
void RegisterProjectBehaviors(BehaviorRegistry& registry) {
    registry.Add<PackageGuard>({packageGuardId, "PackageGuard", {
        {"target", PropertyType::EntityRef, EntityRef{}, {}, {}, "Target"}
    }});
}
)cpp");

        ProjectPackageRequest request;
        request.session = session;
        request.sdkRoot = sdk;
        request.outputDirectory = fixture / "Build" / "M6 Package";
        request.configuration = sdkConfiguration(sdk);
        package = fs::absolute(request.outputDirectory).lexically_normal();
        std::atomic_bool cancel{false};
        ProjectPackageResult first;

        test("PE audit handles distinct normal and delay import tables", [&] {
            const auto synthetic = fixture / "synthetic-imports.exe";
            writeBinary(synthetic, syntheticPeWithNormalAndDelayImports());
            const auto imports = inspectPortableExecutableImports(synthetic);
            check(imports == std::vector<std::string>{"delay-test.dll", "normal-test.dll"},
                  "synthetic PE imports were not decoded exactly");
        });

        test("exports saved startup scene and explicit raw asset", [&] {
            first = buildProjectPackage(request, cancel);
            check(fs::is_regular_file(first.executable), "packaged executable is missing");
            const auto executableName = utf8(first.executable.filename().wstring());
            check(validUtf8(executableName) && executableName.size() <= 164,
                  "long Cyrillic build name was truncated inside a UTF-8 character");
            check(fs::is_regular_file(first.manifest), "package marker is missing");
            check(fs::is_regular_file(package / "Data" / "runtime.json"), "portable runtime manifest is missing");
            check(fs::is_regular_file(package / "Data" / "dll-audit.json"), "DLL audit is missing");
            check(fs::is_regular_file(package / "Config" / "graphics.json"), "packaged graphics settings are missing");
            check(fs::is_directory(package / "Data" / "shaders"), "packaged shader directory is missing");
            check(read(package / "Data" / "dll-audit.json").find("\"imports\"") != std::string::npos,
                  "DLL import audit did not run");
            const auto loaded = loadRuntimePackage(package, first.schema.sdkBuildId);
            check(loaded.projectId == session->info().id, "loaded package project identity mismatch");
        });

        test("package runs from its own directory and preserves schema discovery", [&] {
            ProcessSpec process;
            process.executable = first.executable;
            process.workingDirectory = package;
            process.arguments = {"--describe-behaviors"};
            process.stdoutLimit = 4 * 1024 * 1024;
            process.stderrLimit = 4 * 1024 * 1024;
            ManagedProcess child;
            child.start(process);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (child.running() && std::chrono::steady_clock::now() < deadline) {
                child.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(!child.running(), "packaged Player did not exit from another working directory");
            check(child.exitCode() == 0, "packaged Player describe failed");
        });

        test("re-export retains the previous successful output", [&] {
            const auto previousExecutableBytes = read(first.executable);
            const auto second = buildProjectPackage(request, cancel);
            check(fs::is_regular_file(second.executable), "second package executable is missing");
            check(hasPreviousPackage(package), "previous package folder was not retained");
            check(read(second.executable) == previousExecutableBytes, "unchanged package was not reproducible");
        });

        test("manifest edits without session refresh are rejected", [&] {
            const auto before = read(package / "Data" / "runtime.json");
            const auto manifestPath = fixture / "project.proto.json";
            const auto canonical = read(manifestPath);
            const auto at = canonical.find(rawId.string());
            check(at != std::string::npos, "canonical additional asset ID disappeared");
            auto edited = canonical;
            edited.replace(at, rawId.string().size(), "Assets/not-refreshed.bin");
            write(manifestPath, edited);
            bool stale{};
            try {
                (void)buildProjectPackage(request, cancel);
            } catch (const ProjectPackageError& error) {
                stale = std::string(error.what()).find("stale") != std::string::npos;
            }
            write(manifestPath, canonical);
            check(stale, "manifest edit without refresh was accepted by package export");
            check(read(package / "Data" / "runtime.json") == before,
                  "stale manifest export replaced the successful package");
        });

        test("invalid saved behavior scene cannot replace a successful package", [&] {
            const auto before = read(package / "Data" / "runtime.json");
            const auto scenePath = session->startupScenePath();
            const auto originalScene = read(scenePath);
            const auto marker = std::string("\"behaviors\": []");
            const auto at = originalScene.find(marker);
            check(at != std::string::npos, "M6 scene fixture has no behavior array");
            const auto invalidBinding =
                "\"behaviors\": [{\"id\":\"60000000-0000-4000-8000-000000000002\","
                "\"type\":\"60000000-0000-4000-8000-000000000001\",\"enabled\":true,"
                "\"properties\":{\"target\":{\"entityRef\":\"99999999-9999-4999-8999-999999999999\"}}}]";
            auto invalidScene = originalScene;
            invalidScene.replace(at, marker.size(), invalidBinding);
            write(scenePath, invalidScene);
            bool rejected{};
            try {
                (void)buildProjectPackage(request, cancel);
            } catch (const ProjectPackageError& error) {
                rejected = std::string(error.what()).find("missing entity") != std::string::npos;
            }
            write(scenePath, originalScene);
            check(rejected, "invalid behavior EntityRef was not rejected before publication");
            check(read(package / "Data" / "runtime.json") == before,
                  "invalid behavior scene replaced the successful package");
        });

        test("cancelled or failed export cannot replace a successful package", [&] {
            const auto before = read(package / "Data" / "runtime.json");
            cancel.store(true);
            bool cancelled{};
            try {
                (void)buildProjectPackage(request, cancel);
            } catch (const ProjectPackageError& error) {
                cancelled = std::string(error.what()).find("cancel") != std::string::npos;
            }
            cancel.store(false);
            check(cancelled, "package cancellation was not reported");
            check(read(package / "Data" / "runtime.json") == before, "cancelled export replaced package output");

            const auto rawPath = fixture / "Assets" / "runtime-renamed.bin";
            const auto rawBefore = read(rawPath);
            auto changedDuringStage = request;
            changedDuringStage.faultHook = [&](std::string_view stage) {
                if (stage == "before-input-validation")
                    write(rawPath, "changed while package was staged\n");
            };
            bool sourceChanged{};
            try {
                (void)buildProjectPackage(changedDuringStage, cancel);
            } catch (const ProjectPackageError& error) {
                sourceChanged = std::string(error.what()).find("changed") != std::string::npos;
            }
            write(rawPath, rawBefore);
            check(sourceChanged, "authored asset change during staging was not rejected");
            check(read(package / "Data" / "runtime.json") == before, "source-change export replaced package output");

            write(fixture / "Code" / "Behaviors.cpp", "this is an intentional M6 compile error;\n");
            bool failedBuild{};
            try {
                (void)buildProjectPackage(request, cancel);
            } catch (const ProjectPackageError&) {
                failedBuild = true;
            }
            check(failedBuild, "invalid Player source unexpectedly exported");
            check(read(package / "Data" / "runtime.json") == before, "failed export replaced package output");
        });

        session.reset();
        test("relocating the project preserves raw asset identity", [&] {
            const auto relocatedRoot = output / utf8Path("m6-package-relocated-" + Uuid::create().string());
            fs::create_directories(relocatedRoot);
            for (const auto* folder : {"Assets", "Scenes", "Code", "Config"})
                fs::copy(fixture / folder, relocatedRoot / folder,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::copy_file(fixture / "project.proto.json", relocatedRoot / "project.proto.json");
            auto relocatedSession = ProjectSession::open(relocatedRoot);
            check(relocatedSession->buildSettings().additionalAssets.size() == 1 &&
                      relocatedSession->buildSettings().additionalAssets.front() == rawId.string(),
                  "project relocation changed the persistent raw asset ID");
            const auto assets = inspectProjectAssets(relocatedRoot);
            const auto found = std::find_if(assets.begin(), assets.end(), [&](const auto& asset) {
                return asset.id == rawId.string();
            });
            check(found != assets.end() && found->path == "Assets/runtime-renamed.bin",
                  "relocated project lost the renamed raw asset sidecar");
            relocatedSession.reset();
            removeTree(relocatedRoot);
        });
        request.session.reset();
        removeTree(fixture);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            if (session)
                session.reset();
            if (fs::exists(fixture))
                removeTree(fixture);
        } catch (...) {
        }
        return 1;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
