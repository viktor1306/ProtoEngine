#include "project/ProjectPackage.hpp"

#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "project/BuildFileTree.hpp"
#include "project/FileTransactions.hpp"
#include "project/PlayAssets.hpp"
#include "runtime/PlayerGraphics.hpp"
#include "scene/SceneIO.hpp"

#include <windows.h>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace proto {
namespace {
namespace fs = std::filesystem;

constexpr size_t kMaxManifestBytes = 4 * 1024 * 1024;
constexpr size_t kMaxImportedDlls = 512;
constexpr size_t kMaxSnapshotFiles = 200000;
constexpr size_t kMaxSnapshotBytes = 1024ull * 1024ull * 1024ull;

[[noreturn]] void fail(std::string message) {
    throw ProjectPackageError(std::move(message));
}

void checkCancelled(const std::atomic_bool& cancel) {
    if (cancel.load(std::memory_order_relaxed))
        throw ProjectPackageError("Package export cancelled");
}

void report(const BuildProgressCallback& callback, std::string_view stage, std::string_view message) {
    if (callback)
        callback({std::string(stage), std::string(message)});
}

bool reparse(const fs::path& path) {
    const auto attributes = GetFileAttributesW(build_detail::extendedFilePath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void requireDirectory(const fs::path& path, std::string_view label) {
    const auto attributes = GetFileAttributesW(build_detail::extendedFilePath(path).c_str());
    if (path.empty() || attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || reparse(path))
        fail(std::string(label) + " is missing, not a directory, or is a reparse point");
}

void requireRegular(const fs::path& path, std::string_view label) {
    const auto attributes = GetFileAttributesW(build_detail::extendedFilePath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) || reparse(path))
        fail(std::string(label) + " is missing, not a regular file, or is a reparse point");
}

std::string pathText(const fs::path& path) {
    return utf8(path.generic_wstring());
}

fs::path absoluteNormal(const fs::path& path) {
    if (path.empty())
        fail("Package path is empty");
    const auto result = fs::absolute(path).lexically_normal();
    if (result.has_root_path() == false)
        fail("Package path is not rooted");
    return result;
}

bool samePath(const fs::path& left, const fs::path& right) {
    const auto a = absoluteNormal(left);
    const auto b = absoluteNormal(right);
    if (a == b)
        return true;
    std::error_code error;
    return fs::exists(a, error) && fs::exists(b, error) && fs::equivalent(a, b, error) && !error;
}

std::string lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

void copyFileChecked(const fs::path& source, const fs::path& destination, std::string_view label,
                    bool allowMissing = false) {
    if (!fs::exists(source)) {
        if (allowMissing)
            return;
        fail(std::string(label) + " is missing: " + pathText(source));
    }
    requireRegular(source, label);
    ensureParent(fs::path(build_detail::extendedFilePath(destination)));
    if (!CopyFileW(build_detail::extendedFilePath(source).c_str(), build_detail::extendedFilePath(destination).c_str(), TRUE))
        fail("Cannot copy " + std::string(label) + " to package: " + pathText(destination));
    if (reparse(destination))
        fail("Package destination became a reparse point: " + pathText(destination));
}

std::vector<fs::path> ordinaryTreeFiles(const fs::path& root) {
    std::vector<fs::path> pending{root}, files;
    size_t entries{};
    while (!pending.empty()) {
        auto directory = std::move(pending.back());
        pending.pop_back();
        requireDirectory(directory, "Package input directory");
        WIN32_FIND_DATAW data{};
        const auto handle = FindFirstFileW(build_detail::extendedFilePath(directory / L"*").c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                continue;
            fail("Cannot enumerate package input directory: " + pathText(directory));
        }
        struct CloseFind { HANDLE handle; ~CloseFind() { FindClose(handle); } } guard{handle};
        do {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..")
                continue;
            if (++entries > kMaxSnapshotFiles || data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                fail("Package input contains too many entries or a reparse point");
            const auto path = directory / data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                pending.push_back(path);
            else
                files.push_back(path);
        } while (FindNextFileW(handle, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            fail("Cannot finish enumerating package input directory");
    }
    return files;
}

void copyTreeChecked(const fs::path& source, const fs::path& destination, std::string_view label,
                    bool allowMissing = false) {
    if (!fs::exists(source)) {
        if (allowMissing)
            return;
        fail(std::string(label) + " is missing: " + pathText(source));
    }
    if (reparse(source))
        fail(std::string(label) + " is a reparse point: " + pathText(source));
    if (fs::is_regular_file(source)) {
        copyFileChecked(source, destination, label);
        return;
    }
    requireDirectory(source, label);
    for (const auto& current : ordinaryTreeFiles(source))
        copyFileChecked(current, destination / current.lexically_relative(source), label);
}

struct AuthoredSnapshot {
    std::map<std::string, std::pair<uintmax_t, std::string>> files;
    uintmax_t bytes{};
};

void captureTree(const fs::path& root, const fs::path& directory, AuthoredSnapshot& result) {
    if (!fs::exists(directory))
        return;
    requireDirectory(directory, "Project authored root");
    for (const auto& path : ordinaryTreeFiles(directory)) {
        if (result.files.size() >= kMaxSnapshotFiles)
            fail("Authored package input contains too many files");
        const auto bytes = assetBytes(fs::path(build_detail::extendedFilePath(path)), 512 * 1024 * 1024);
        if (result.bytes > kMaxSnapshotBytes || bytes.size() > kMaxSnapshotBytes - result.bytes)
            fail("Authored package input exceeds its size limit");
        const auto relative = projectRelative(root, path);
        result.files.emplace(relative, std::make_pair(static_cast<uintmax_t>(bytes.size()), sha256(bytes)));
        result.bytes += bytes.size();
    }
}

AuthoredSnapshot captureAuthored(const fs::path& root) {
    AuthoredSnapshot result;
    captureTree(root, projectPath(root, "Assets", true), result);
    captureTree(root, projectPath(root, "Scenes", true), result);
    captureTree(root, projectPath(root, "Code", true), result);
    captureTree(root, projectPath(root, "Config", true), result);
    captureTree(root, root / "Notices", result);
    captureTree(root, root / "licenses", result);
    const auto manifest = projectPath(root, "project.proto.json", true);
    if (fs::exists(manifest)) {
        const auto bytes = assetBytes(manifest, 32 * 1024 * 1024);
        result.files.emplace("project.proto.json", std::make_pair(static_cast<uintmax_t>(bytes.size()), sha256(bytes)));
        result.bytes += bytes.size();
    }
    for (const auto* name : {"THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt", "NOTICE", "LICENSE"}) {
        const auto path = root / name;
        if (!fs::exists(path))
            continue;
        requireRegular(path, "Project notice");
        const auto bytes = assetBytes(path, kMaxManifestBytes);
        result.files.emplace(name, std::make_pair(static_cast<uintmax_t>(bytes.size()), sha256(bytes)));
        result.bytes += bytes.size();
    }
    return result;
}

bool sameAuthored(const AuthoredSnapshot& before, const AuthoredSnapshot& after) {
    return before.bytes == after.bytes && before.files == after.files;
}

bool sameBuildSettings(const ProjectBuildSettings& left, const ProjectBuildSettings& right) {
    return left.target == right.target && left.name == right.name && left.additionalAssets == right.additionalAssets;
}

void rejectStaleSession(const ProjectSession& session, const ProjectSession::RefreshCandidate& fresh) {
    const auto& current = session.info();
    if (fresh.info.id != current.id || fresh.info.name != current.name || fresh.info.startupScene != current.startupScene ||
        !samePath(fresh.startupScenePath, session.startupScenePath()) ||
        !sameBuildSettings(fresh.buildSettings, session.buildSettings()) ||
        !samePath(fresh.graphicsSettingsPath, session.graphicsSettingsPath()))
        fail("ProjectSession is stale; refresh before package export");
}

Scene loadSavedScene(const std::shared_ptr<ProjectSession>& session) {
    const auto path = session->startupScenePath();
    requireRegular(path, "Saved startup scene");
    const auto bytes = readDocument(path);
    JsonDoc header(bytes);
    auto* sources = yyjson_obj_get(header.root(), "modelSources");
    auto catalog = std::make_shared<AssetCatalog>();
    if (sources) {
        if (!yyjson_is_arr(sources) || yyjson_arr_size(sources) > 4096)
            fail("Saved startup scene has an invalid model source list");
        size_t index{}, count{};
        Json* value{};
        yyjson_arr_foreach(sources, index, count, value) {
            const auto id = AssetId::parse(str(value));
            if (catalog->models.contains(id))
                fail("Saved startup scene repeats a model source");
            const auto model = session->assetWorkspace()->load(id);
            if (!model || model->id != id)
                fail("Saved startup scene model source is invalid: " + id.string());
            catalog->publish(model);
        }
    }
    auto scene = decodeScene(bytes, std::move(catalog));
    if (scene.id != session->info().startupScene)
        fail("Saved startup scene no longer matches project.proto.json");
    if (!scene.modelSources.empty()) {
        const auto assetRoot = utf8Path(scene.assetRoot);
        if (assetRoot.empty() || assetRoot.is_absolute() || assetRoot.has_root_name())
            fail("Saved startup scene has an invalid assetRoot");
        if (!samePath(path.parent_path() / assetRoot, session->root()))
            fail("Saved startup scene assetRoot leaves the project");
    }
    return scene;
}

void validateGraphics(const fs::path& source) {
    const auto bytes = readDocument(source);
    if (bytes.size() > kMaxManifestBytes)
        fail("Project graphics settings exceed the package limit");
    try {
        (void)decodePlayerGraphics(bytes);
    } catch (const std::exception& error) {
        fail(std::string("Project graphics settings are invalid: ") + error.what());
    }
}

std::string safeExecutableStem(std::string value) {
    if (value.empty())
        value = "ProtoPlayer";
    for (auto& c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*')
            c = '_';
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' '))
        value.pop_back();
    if (value.empty())
        value = "ProtoPlayer";
    if (lower(value).ends_with(".exe"))
        value.resize(value.size() - 4);
    while (!value.empty() && (value.back() == '.' || value.back() == ' '))
        value.pop_back();
    if (value.empty())
        value = "ProtoPlayer";
    const auto dot = value.find('.');
    const auto stem = lower(value.substr(0, dot));
    static constexpr std::array<std::string_view, 22> reserved = {
        "con", "prn", "aux", "nul", "clock$", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8"};
    if (std::find(reserved.begin(), reserved.end(), stem) != reserved.end() || stem == "lpt9")
        value = "ProtoPlayer-" + value;
    if (value.size() > 160) {
        value.resize(160);
        while (!value.empty() && !validUtf8(value))
            value.pop_back();
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' '))
        value.pop_back();
    return value;
}

struct PeSection {
    uint32_t virtualAddress{}, virtualSize{}, rawAddress{}, rawSize{};
};

uint16_t pe16(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        fail("Truncated Player PE header");
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
}

uint32_t pe32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        fail("Truncated Player PE import table");
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
}

size_t rvaOffset(const std::vector<uint8_t>& bytes, uint32_t rva, const std::vector<PeSection>& sections) {
    for (const auto& section : sections) {
        const auto span = std::max(section.virtualSize, section.rawSize);
        if (rva >= section.virtualAddress && rva - section.virtualAddress < span) {
            const auto offset = uint64_t(section.rawAddress) + (rva - section.virtualAddress);
            if (offset > bytes.size())
                break;
            return static_cast<size_t>(offset);
        }
    }
    fail("Player PE import RVA is outside its sections");
}

std::string peString(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset >= bytes.size())
        fail("Truncated Player PE import name");
    std::string value;
    for (size_t i = offset; i < bytes.size() && value.size() <= 512; ++i) {
        if (!bytes[i])
            return value;
        value.push_back(static_cast<char>(bytes[i]));
    }
    fail("Unbounded Player PE import name");
}

void appendImportNames(const std::vector<uint8_t>& bytes, uint32_t rva, const std::vector<PeSection>& sections,
                       bool delay, std::vector<std::string>& names) {
    if (!rva)
        return;
    auto offset = rvaOffset(bytes, rva, sections);
    for (size_t item = 0; item < kMaxImportedDlls; ++item) {
        const auto entry = offset + item * (delay ? 32u : 20u);
        if (entry > bytes.size() || bytes.size() - entry < (delay ? 32u : 20u))
            fail("Truncated Player PE import descriptor");
        const auto descriptorSize = delay ? 32u : 20u;
        const bool terminator = std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(entry),
                                            bytes.begin() + static_cast<std::ptrdiff_t>(entry + descriptorSize),
                                            [](uint8_t value) { return value == 0; });
        if (terminator)
            return;
        if (delay && !(pe32(bytes, entry) & 1u))
            fail("Player PE delay-import table uses unsupported absolute addresses");
        const auto nameRva = delay ? pe32(bytes, entry + 4) : pe32(bytes, entry + 12);
        if (!nameRva)
            return;
        const auto name = peString(bytes, rvaOffset(bytes, nameRva, sections));
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos)
            fail("Player PE import has an unsafe DLL name");
        names.push_back(name);
    }
    fail("Player PE import table exceeds its bound");
}

std::vector<std::string> peImports(const fs::path& executable) {
    const auto bytes = assetBytes(fs::path(build_detail::extendedFilePath(executable)), 512 * 1024 * 1024);
    if (bytes.size() < 0x40 || pe16(bytes, 0) != 0x5a4d)
        fail("Player output is not a PE executable");
    const auto peOffset = pe32(bytes, 0x3c);
    if (peOffset > bytes.size() || bytes.size() - peOffset < 24 || pe32(bytes, peOffset) != 0x00004550)
        fail("Player output has an invalid PE signature");
    const auto sectionCount = pe16(bytes, peOffset + 6);
    const auto optionalSize = pe16(bytes, peOffset + 20);
    const auto optional = peOffset + 24;
    if (optional > bytes.size() || bytes.size() - optional < optionalSize || optionalSize < 96)
        fail("Player PE optional header is truncated");
    const auto magic = pe16(bytes, optional);
    const bool pe32plus = magic == 0x20b;
    if (!pe32plus && magic != 0x10b)
        fail("Player PE optional header has an unsupported format");
    const auto directoryCountOffset = optional + (pe32plus ? 108u : 92u);
    const auto directoryOffset = optional + (pe32plus ? 112u : 96u);
    const auto requiredOptional = pe32plus ? 112u + 8u * 14u : 96u + 8u * 14u;
    if (optionalSize < requiredOptional || directoryCountOffset + 4 > bytes.size())
        fail("Player PE data directory header is truncated");
    const auto numberOfDirectories = pe32(bytes, directoryCountOffset);
    if (numberOfDirectories <= 1)
        return {};
    if (numberOfDirectories > 64)
        fail("Player PE directory count is unreasonable");
    const auto sectionTable = optional + optionalSize;
    if (sectionTable > bytes.size() || bytes.size() - sectionTable < size_t(sectionCount) * 40u)
        fail("Player PE section table is truncated");
    std::vector<PeSection> sections;
    sections.reserve(sectionCount);
    for (size_t i = 0; i < sectionCount; ++i) {
        const auto at = sectionTable + i * 40u;
        sections.push_back({pe32(bytes, at + 12), pe32(bytes, at + 8), pe32(bytes, at + 20), pe32(bytes, at + 16)});
    }
    std::vector<std::string> result;
    if (directoryOffset + 8u * 14u > bytes.size())
        fail("Player PE data directories are truncated");
    appendImportNames(bytes, pe32(bytes, directoryOffset + 8u), sections, false, result);
    if (numberOfDirectories > 13)
        appendImportNames(bytes, pe32(bytes, directoryOffset + 8u * 13u), sections, true, result);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool isSystemDll(const std::string& name) {
    const auto value = lower(name);
    if (value.starts_with("api-ms-win-") || value.starts_with("ext-ms-win-"))
        return true;
    // These are explicit Windows/Vulkan runtime prerequisites. Host-installed
    // MSVC redistributables and arbitrary System32 entries are intentionally
    // not accepted here; they must be supplied by the SDK or fail the audit.
    static constexpr std::array<std::string_view, 18> names = {
        "advapi32.dll", "bcrypt.dll", "combase.dll", "gdi32.dll", "gdi32full.dll", "kernel32.dll",
        "kernelbase.dll", "ntdll.dll", "ole32.dll", "shell32.dll", "user32.dll", "ucrtbase.dll",
        "version.dll", "winmm.dll", "ws2_32.dll", "uuid.dll", "vulkan-1.dll", "msvcrt.dll"};
    return std::find(names.begin(), names.end(), value) != names.end();
}

fs::path findSdkDll(const fs::path& sdkRoot, const std::string& name) {
    const std::array<fs::path, 4> candidates = {sdkRoot / name, sdkRoot / "bin" / name, sdkRoot / "lib" / name,
                                                 sdkRoot / "validation" / name};
    for (const auto& candidate : candidates)
        if (fs::exists(candidate)) {
            requireRegular(candidate, "SDK DLL");
            return candidate;
        }
    return {};
}

PackageDllAudit auditAndCopyDlls(const fs::path& executable, const fs::path& sdkRoot, const fs::path& stage) {
    PackageDllAudit result;
    std::deque<std::string> pending;
    for (const auto& name : peImports(executable))
        pending.push_back(name);
    std::set<std::string> seen;
    while (!pending.empty()) {
        const auto name = std::move(pending.front());
        pending.pop_front();
        const auto key = lower(name);
        if (!seen.insert(key).second)
            continue;
        if (seen.size() > kMaxImportedDlls)
            fail("Player DLL import closure exceeds its bound");
        result.imported.push_back(name);
        if (isSystemDll(name)) {
            result.system.push_back(name);
            continue;
        }
        const auto source = findSdkDll(sdkRoot, name);
        if (source.empty())
            fail("Player imports an unknown or unavailable DLL: " + name);
        copyFileChecked(source, stage / utf8Path(name), "SDK imported DLL");
        result.copied.push_back(name);
        for (const auto& dependency : peImports(source))
            pending.push_back(dependency);
    }
    std::sort(result.imported.begin(), result.imported.end(), [](const auto& left, const auto& right) {
        return lower(left) < lower(right);
    });
    std::sort(result.system.begin(), result.system.end(), [](const auto& left, const auto& right) {
        return lower(left) < lower(right);
    });
    std::sort(result.copied.begin(), result.copied.end(), [](const auto& left, const auto& right) {
        return lower(left) < lower(right);
    });
    return result;
}

void copySdkResources(const fs::path& sdkRoot, const fs::path& stage) {
    const auto shaders = sdkRoot / "shaders";
    requireDirectory(shaders, "SDK shader directory");
    std::error_code error;
    size_t count{};
    for (fs::directory_iterator it(shaders, fs::directory_options::none, error), end; it != end; it.increment(error)) {
        if (error)
            fail("Cannot scan SDK shaders");
        const auto source = it->path();
        if (reparse(source))
            fail("SDK shader tree contains a reparse point");
        if (it->is_regular_file(error) && source.extension() == ".spv") {
            copyFileChecked(source, stage / "Data" / "shaders" / source.filename(), "SDK shader");
            ++count;
        }
    }
    if (!count)
        fail("SDK shader directory contains no SPIR-V shaders");

    // RuntimePackage's immutable layout is intentionally strict: licenses
    // are rooted at Licenses/ and at least one SDK notice is top-level.
    copyTreeChecked(sdkRoot / "licenses", stage / "Licenses", "SDK licenses", true);
    bool notice{};
    for (const auto* name : {"THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt", "NOTICE", "LICENSE"})
        if (fs::exists(sdkRoot / name)) {
            if (std::string_view(name).starts_with("THIRD_PARTY_NOTICES")) {
                copyFileChecked(sdkRoot / name, stage / name, "SDK notice");
                notice = true;
            } else
                copyFileChecked(sdkRoot / name, stage / "Licenses" / "sdk" / name, "SDK notice");
        }
    if (!notice)
        for (const auto* name : {"NOTICE", "LICENSE"})
            if (fs::exists(sdkRoot / name)) {
                copyFileChecked(sdkRoot / name, stage / "THIRD_PARTY_NOTICES.txt", "SDK notice");
                notice = true;
                break;
            }
    if (!notice)
        fail("SDK package contains no third-party notice");
}

void copyProjectNotices(const fs::path& projectRoot, const fs::path& stage) {
    for (const auto* name : {"THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt", "NOTICE", "LICENSE"})
        copyFileChecked(projectRoot / name, stage / "Licenses" / "project" / name, "Project notice", true);
    copyTreeChecked(projectRoot / "Notices", stage / "Licenses" / "project" / "notices", "Project notices", true);
    copyTreeChecked(projectRoot / "licenses", stage / "Licenses" / "project" / "licenses", "Project licenses", true);
}

std::string arrayJson(const std::vector<std::string>& values) {
    std::string result = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            result += ',';
        result += jsonString(values[i]);
    }
    result += ']';
    return result;
}

std::string dllAuditJson(const PackageDllAudit& audit) {
    std::vector<std::string> imported = audit.imported;
    std::vector<std::string> system = audit.system;
    std::vector<std::string> copied = audit.copied;
    return std::string("{\n  \"format\":\"proto.dll-audit\",\n  \"formatVersion\":1,\n") +
           "  \"imports\":" + arrayJson(imported) + ",\n" +
           "  \"systemImports\":" + arrayJson(system) + ",\n" +
           "  \"copiedImports\":" + arrayJson(copied) + "\n}\n";
}

bool validExistingPackage(const fs::path& root, const ProjectSession& session) {
    const auto marker = root / "Data" / "runtime.json";
    const auto audit = root / "Data" / "dll-audit.json";
    if (!fs::is_directory(root) || reparse(root) || !fs::is_regular_file(marker) || reparse(marker) ||
        !fs::is_regular_file(audit) || reparse(audit))
        return false;
    try {
        JsonDoc document(readDocument(marker));
        auto* object = document.root();
        auto* format = yyjson_obj_get(object, "format");
        auto* version = yyjson_obj_get(object, "formatVersion");
        auto* projectId = yyjson_obj_get(object, "projectId");
        auto* sdkBuildId = yyjson_obj_get(object, "sdkBuildId");
        if (!(format && yyjson_is_str(format) &&
               std::string_view(yyjson_get_str(format), yyjson_get_len(format)) == "proto.runtime" && version &&
               yyjson_is_uint(version) && yyjson_get_uint(version) == 1 && projectId && yyjson_is_str(projectId) &&
               std::string_view(yyjson_get_str(projectId), yyjson_get_len(projectId)) == session.info().id.string() &&
               sdkBuildId && yyjson_is_str(sdkBuildId)))
            return false;
        JsonDoc auditDocument(readDocument(audit));
        auto* auditObject = auditDocument.root();
        auto* auditFormat = yyjson_obj_get(auditObject, "format");
        auto* auditVersion = yyjson_obj_get(auditObject, "formatVersion");
        if (!auditFormat || !yyjson_is_str(auditFormat) ||
            std::string_view(yyjson_get_str(auditFormat), yyjson_get_len(auditFormat)) != "proto.dll-audit" ||
            !auditVersion || !yyjson_is_uint(auditVersion) || yyjson_get_uint(auditVersion) != 1)
            return false;
        // The runtime loader validates hashes, resource closure, graphics and
        // the complete immutable inventory before the old directory is moved.
        (void)loadRuntimePackage(root, std::string_view(yyjson_get_str(sdkBuildId), yyjson_get_len(sdkBuildId)));
        return true;
    } catch (...) {
        return false;
    }
}

void cleanupTree(const fs::path& path) {
    if (!fs::exists(path))
        return;
    std::error_code error;
    (void)build_detail::removeTree(path, error);
    if (error)
        throw std::runtime_error("Cannot clean package staging directory: " + pathText(path) + ": " + error.message());
}

fs::path publishStage(const fs::path& stage, const fs::path& output, const ProjectSession& session,
                     const std::atomic_bool& cancel) {
    checkCancelled(cancel);
    ensureParent(output);
    if (fs::exists(output)) {
        if (!validExistingPackage(output, session))
            fail("Output directory exists and is not a matching Proto package");
        const auto previous = output.parent_path() /
                              fs::path(output.filename().wstring() + L".previous-" +
                                       fs::path(Uuid::create().string()).wstring());
        checkCancelled(cancel); // immediately before the first publication rename
        if (!MoveFileExW(output.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH))
            fail("Cannot preserve previous package output");
        if (cancel.load(std::memory_order_relaxed)) {
            if (!MoveFileExW(previous.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
                fail("Package export cancelled; previous output was retained at " + pathText(previous));
            fail("Package export cancelled before publication");
        }
        if (!MoveFileExW(stage.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (!MoveFileExW(previous.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
                fail("Cannot publish package output; previous output was retained at " + pathText(previous));
            fail("Cannot publish package output; previous output was restored");
        }
        return previous;
    }
    checkCancelled(cancel); // immediately before the first publication rename
    if (!MoveFileExW(stage.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        fail("Cannot publish package output");
    return {};
}

} // namespace

std::vector<std::string> inspectPortableExecutableImports(const std::filesystem::path& executable) {
    return peImports(executable);
}

ProjectPackageResult buildProjectPackage(const ProjectPackageRequest& request, std::atomic_bool& cancel,
                                         BuildProgressCallback progress) {
    fs::path stage;
    bool stageOwned{};
    try {
        checkCancelled(cancel);
        if (!request.session)
            fail("Package export requires an open ProjectSession");
        const auto freshManifest = request.session->prepareRefresh();
        rejectStaleSession(*request.session, freshManifest);
        requireDirectory(request.session->root(), "Project root");
        requireDirectory(request.sdkRoot, "SDK root");
        if (request.session->buildSettings().target != "windows-x64")
            fail("Project target is not windows-x64");
        if (request.configuration != "Debug" && request.configuration != "Release")
            fail("Package configuration must be Debug or Release");

        const auto projectRoot = absoluteNormal(request.session->root());
        const auto sdkRoot = absoluteNormal(request.sdkRoot);
        const auto output = absoluteNormal(request.outputDirectory);
        if (output == projectRoot || output == sdkRoot || output.filename().empty())
            fail("Package output directory is invalid");
        const auto before = captureAuthored(projectRoot);
        report(progress, "build", "building the project Player");
        ProjectBuildRequest buildRequest;
        buildRequest.projectRoot = projectRoot;
        buildRequest.sdkRoot = sdkRoot;
        buildRequest.configuration = request.configuration;
        buildRequest.processOutputLimit = request.processOutputLimit;
        buildRequest.parallelism = request.parallelism;
        buildRequest.expectedSdkBuildId = request.expectedSdkBuildId;
        ProjectBuildResult build;
        try {
            build = buildProject(buildRequest, cancel, progress);
        } catch (const ProjectBuildError& error) {
            throw ProjectPackageError(error.what(), error.diagnostics());
        }
        checkCancelled(cancel);

        report(progress, "assets", "resolving startup scene and explicit asset roots");
        auto scene = loadSavedScene(request.session);
        const auto closure = preparePlayAssets(scene, projectRoot, build.schema,
                                                request.session->buildSettings().additionalAssets);
        auto sceneSnapshot = scene.snapshot();
        sceneSnapshot.assetRoot.clear();
        scene = Scene::fromSnapshot(sceneSnapshot, scene.assets);
        validateSceneBehaviors(scene, build.schema);
        checkCancelled(cancel);

        const auto parent = output.parent_path();
        fs::create_directories(parent);
        stage = parent / fs::path(output.filename().wstring() + L".tmp-" + fs::path(Uuid::create().string()).wstring());
        if (fs::exists(stage))
            fail("Package staging directory unexpectedly exists");
        if (!fs::create_directory(stage))
            fail("Cannot create package staging directory");
        stageOwned = true;

        const auto executableName = safeExecutableStem(request.session->buildSettings().name) + ".exe";
        const auto stagedExecutable = stage / executableName;
        copyFileChecked(build.executable, stagedExecutable, "Built Player executable");
        copySdkResources(sdkRoot, stage);
        copyProjectNotices(projectRoot, stage);
        const auto graphics = request.session->graphicsSettingsPath();
        requireRegular(graphics, "Project graphics settings");
        validateGraphics(graphics);
        copyFileChecked(graphics, stage / "Config" / "graphics.json", "Project graphics settings");

        RuntimePackageMetadata metadata;
        metadata.projectId = request.session->info().id;
        metadata.name = request.session->info().name;
        metadata.sdkBuildId = build.schema.sdkBuildId;
        metadata.executableName = executableName;
        metadata.additionalAssets = closure.resolvedRoots;
        report(progress, "imports", "auditing Player DLL imports");
        const auto dllAudit = auditAndCopyDlls(stagedExecutable, sdkRoot, stage);
        // RuntimePackage inventories this audit as an immutable package file.
        atomicWrite(stage / "Data" / "dll-audit.json", dllAuditJson(dllAudit));
        report(progress, "package", "writing portable runtime data");
        const auto runtimeStats = writeRuntimePackage(stage, scene, metadata, closure.rawAssets);
        if (request.faultHook)
            request.faultHook("staged");

        // Runtime validation happens before the final rename. This validates
        // every Data manifest/blob and catches package writer regressions while
        // the old output remains untouched.
        (void)loadRuntimePackage(stage, build.schema.sdkBuildId);
        checkCancelled(cancel);
        if (request.faultHook)
            request.faultHook("before-input-validation");
        const auto after = captureAuthored(projectRoot);
        if (!sameAuthored(before, after))
            fail("Project authored assets changed while package was being prepared");

        report(progress, "publish", "publishing package output");
        if (request.faultHook)
            request.faultHook("before-publish");
        const auto previous = publishStage(stage, output, *request.session, cancel);
        stageOwned = false;
        stage.clear();
        (void)previous;
        report(progress, "complete", "standalone package export succeeded");
        return {output, output / executableName, output / "Data" / "runtime.json", build.key, std::move(build.schema),
                runtimeStats, dllAudit};
    } catch (const ProjectPackageError&) {
        if (stageOwned && !stage.empty()) {
            try {
                cleanupTree(stage);
            } catch (...) {
            }
        }
        throw;
    } catch (const std::exception& error) {
        if (stageOwned && !stage.empty()) {
            try {
                cleanupTree(stage);
            } catch (...) {
            }
        }
        throw ProjectPackageError(error.what());
    }
}

} // namespace proto
