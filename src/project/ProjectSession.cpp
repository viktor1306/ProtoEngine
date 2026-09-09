#include "project/ProjectSession.hpp"

#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "core/Diagnostics.hpp"
#include "editor/SceneDocument.hpp"
#include "project/FileTransactions.hpp"
#include "project/ProjectWatch.hpp"
#include "scene/SceneIO.hpp"

#include <windows.h>
#include <yyjson.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace proto {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view manifestName = "project.proto.json";

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::string text(Json* value, const char* fieldName) {
    if (!value)
        fail(std::string("Project field missing: ") + fieldName);
    auto result = str(value);
    if (result.find('\0') != std::string::npos || !validUtf8(result))
        fail(std::string("Invalid UTF-8 in project field: ") + fieldName);
    return result;
}

void strictKeys(Json* object, std::initializer_list<std::string_view> allowed, const char* context) {
    if (!yyjson_is_obj(object))
        fail(std::string("Expected project object: ") + context);
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second)
            fail(std::string("Duplicate project field: ") + std::string(name));
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            fail(std::string("Unsupported project field: ") + std::string(name));
    }
}

uint64_t uintField(Json* object, const char* name) {
    auto* value = yyjson_obj_get(object, name);
    if (!value || !yyjson_is_uint(value))
        fail(std::string("Expected project unsigned integer: ") + name);
    return yyjson_get_uint(value);
}

std::string requireRelative(const fs::path& root, const std::string& value, const char* fieldName) {
    if (value.empty() || value.find('\\') != std::string::npos)
        fail(std::string("Invalid project path: ") + fieldName);
    const auto path = utf8Path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        fail(std::string("Project path must be relative: ") + fieldName);
    (void)projectPath(root, value, true);
    return value;
}

bool reparsePoint(const fs::path& path);

void writeNewFileExclusive(const fs::path& path, std::string_view bytes) {
    ensureParent(path);
    const auto temporary = path.parent_path() / (".tmp-" + Uuid::create().string());
    const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        fail("Additional asset sidecar appeared or cannot be created: " + utf8(path.wstring()));
    size_t offset{};
    try {
        while (offset < bytes.size()) {
            const auto amount = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 64 * 1024));
            DWORD written{};
            if (!WriteFile(handle, bytes.data() + offset, amount, &written, nullptr) || written != amount)
                fail("Cannot write additional asset sidecar: " + utf8(path.wstring()));
            offset += written;
        }
        if (!FlushFileBuffers(handle))
            fail("Cannot flush additional asset sidecar: " + utf8(path.wstring()));
    } catch (...) {
        CloseHandle(handle);
        DeleteFileW(temporary.c_str());
        throw;
    }
    CloseHandle(handle);
    // Publish only complete metadata, and never replace a sidecar created by
    // another author while this registration was preparing its bytes.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        fail("Additional asset sidecar appeared or cannot be published: " + utf8(path.wstring()));
    }
}

struct AdditionalAssetIdentity {
    std::string id;
    bool createdSidecar{};
};

AdditionalAssetIdentity additionalAssetIdentity(const fs::path& root, const std::string& relative) {
    if (!projectVisible(relative))
        fail("Additional asset must be an authored runtime file: " + relative);
    const auto source = projectPath(root, relative, false);
    if (reparsePoint(source) || !fs::is_regular_file(source))
        fail("Additional asset is missing or not a regular file: " + relative);
    const auto sidecar = fs::path(source.wstring() + L".meta");
    if (fs::exists(sidecar)) {
        if (!fs::is_regular_file(sidecar) || reparsePoint(sidecar))
            fail("Additional asset sidecar is missing, redirected, or not a file: " + relative);
        JsonDoc document(readDocument(sidecar));
        auto* object = document.root();
        if (!yyjson_is_obj(object))
            fail("Additional asset sidecar must contain an object: " + relative);
        if (text(yyjson_obj_get(object, "format"), "format") != "proto.assetmeta" ||
            uintField(object, "formatVersion") != 1)
            fail("Unsupported additional asset sidecar format: " + relative);
        const auto kind = text(yyjson_obj_get(object, "kind"), "kind");
        if (kind.empty())
            fail("Additional asset sidecar kind is empty: " + relative);
        const auto id = AssetId::parse(text(yyjson_obj_get(object, "assetId"), "assetId"));
        if (auto* sourcePath = yyjson_obj_get(object, "sourcePath")) {
            const auto declared = text(sourcePath, "sourcePath");
            if (projectPathKey(declared) != projectPathKey(relative))
                fail("Additional asset sidecar sourcePath conflicts with its file: " + relative);
        }
        return {id.string(), false};
    }

    const auto id = AssetId::create();
    const auto bytes = std::string("{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"kind\":\"RawDependency\",\"assetId\":") +
                       jsonString(id.string()) + ",\"sourcePath\":" + jsonString(relative) + "}\n";
    const auto sourceBefore = projectStamp(root, relative);
    writeNewFileExclusive(sidecar, bytes);
    if (!(projectStamp(root, relative) == sourceBefore)) {
        DeleteFileW(sidecar.c_str());
        fail("Additional asset changed while its sidecar was being registered: " + relative);
    }
    return {id.string(), true};
}

std::string writeMutableJson(yyjson_mut_doc* document) {
    size_t length{};
    std::unique_ptr<char, decltype(&free)> data(
        yyjson_mut_write(document, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length), free);
    if (!data || length > 32 * 1024 * 1024)
        fail("Project manifest output is too large");
    return {data.get(), length};
}

std::string canonicalizeAdditionalAssets(const std::string& bytes, const std::vector<std::string>& values) {
    JsonDoc source(bytes);
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document(
        yyjson_doc_mut_copy(source.doc.get(), nullptr), yyjson_mut_doc_free);
    if (!document)
        fail("Cannot copy project manifest for additional asset IDs");
    for (size_t i = 0; i < values.size(); ++i) {
        auto* replacement = yyjson_mut_strcpy(document.get(), values[i].c_str());
        if (!replacement ||
            !yyjson_mut_doc_ptr_set(document.get(), ("/build/additionalAssets/" + std::to_string(i)).c_str(),
                                    replacement))
            fail("Cannot canonicalize project additionalAssets");
    }
    return writeMutableJson(document.get());
}

bool serviceName(std::string_view name) {
    std::string key;
    try {
        key = projectPathKey(std::string(name));
    } catch (...) {
        return false;
    }
    return key == ".PROTO" || key.ends_with(".BAK") || key.ends_with(".LOCK") || key.find(".TMP-") != std::string::npos;
}

bool servicePath(const fs::path& root, const fs::path& path) {
    try {
        const auto relative = utf8(path.lexically_relative(root).generic_wstring());
        for (const auto& part : utf8Path(relative))
            if (serviceName(utf8(part.generic_wstring())))
                return true;
    } catch (...) {
        return false;
    }
    return false;
}

bool reparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void validatePathChain(const fs::path& path) {
    auto current = path;
    for (;;) {
        if (reparsePoint(current))
            fail("Project root contains a reparse point");
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current)
            break;
        current = parent;
    }
}

fs::path absoluteDirectory(const fs::path& input) {
    const auto path = fs::absolute(input).lexically_normal();
    validatePathChain(path);
    if (reparsePoint(path))
        fail("Project root is a reparse point");
    if (!fs::exists(path) || !fs::is_directory(path))
        fail("Project root directory does not exist");
    (void)projectPath(path, "", true);
    return path;
}

fs::path rootFromInput(const fs::path& input) {
    const auto path = fs::absolute(input).lexically_normal();
    if (reparsePoint(path))
        fail("Project input is a reparse point");
    if (fs::is_directory(path))
        return absoluteDirectory(path);
    if (!fs::is_regular_file(path) ||
        projectPathKey(utf8(path.filename().generic_wstring())) != projectPathKey(std::string(manifestName)))
        fail("Open expects a project directory or project.proto.json");
    return absoluteDirectory(path.parent_path());
}

void validateFixedLayout(const fs::path& root) {
    for (const char* name : {"Assets", "Scenes", "Code", "Config"}) {
        const auto path = projectPath(root, name, true);
        if (!fs::exists(path) || !fs::is_directory(path) || reparsePoint(path))
            fail(std::string("Missing or redirected project folder: ") + name);
    }
    const auto control = projectPath(root, ".proto", true);
    if (!fs::exists(control) || !fs::is_directory(control) || reparsePoint(control))
        fail("Missing or redirected .proto folder");
}

struct Candidate {
    ProjectInfo info;
    fs::path startupScenePath;
    ProjectBuildSettings buildSettings;
    fs::path graphicsSettingsPath;
};

struct SceneIndexEntry {
    fs::path path;
    AssetId id;
};

bool hasSuffix(const fs::path& path, std::wstring_view suffix) {
    const auto text = path.filename().wstring();
    if (text.size() < suffix.size())
        return false;
    auto tail = text.substr(text.size() - suffix.size());
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](wchar_t c) { return c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c; });
    std::wstring expected(suffix);
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](wchar_t c) { return c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c; });
    return tail == expected;
}

AssetId validateSceneSidecar(const fs::path& scenePath, const fs::path& sidecar) {
    JsonDoc document(readDocument(sidecar));
    auto* root = document.root();
    strictKeys(root, {"format", "formatVersion", "assetId", "kind"}, "scene metadata");
    if (text(yyjson_obj_get(root, "format"), "format") != "proto.assetmeta" || uintField(root, "formatVersion") != 1 ||
        text(yyjson_obj_get(root, "kind"), "kind") != "Scene")
        fail("Invalid scene metadata header");
    const auto id = AssetId::parse(text(yyjson_obj_get(root, "assetId"), "assetId"));
    (void)scenePath;
    return id;
}

std::vector<SceneIndexEntry> scanScenes(const fs::path& root) {
    const auto scenes = projectPath(root, "Scenes", true);
    std::vector<fs::path> sceneFiles;
    std::vector<fs::path> sidecars;
    fs::recursive_directory_iterator iterator(scenes, fs::directory_options::skip_permission_denied);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
        const auto path = iterator->path();
        if (reparsePoint(path)) {
            if (iterator->is_directory())
                iterator.disable_recursion_pending();
            continue;
        }
        if (servicePath(root, path)) {
            if (iterator->is_directory())
                iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_directory())
            continue;
        if (!iterator->is_regular_file())
            continue;
        if (hasSuffix(path, L".scene.json.meta"))
            sidecars.push_back(path);
        else if (hasSuffix(path, L".scene.json"))
            sceneFiles.push_back(path);
    }
    std::sort(sceneFiles.begin(), sceneFiles.end());
    std::sort(sidecars.begin(), sidecars.end());
    std::set<fs::path> sceneSet(sceneFiles.begin(), sceneFiles.end());
    std::map<AssetId, fs::path> byId;
    std::vector<SceneIndexEntry> result;
    for (const auto& scenePath : sceneFiles) {
        const auto expectedMeta = fs::path(scenePath.wstring() + L".meta");
        if (!fs::exists(expectedMeta) || !fs::is_regular_file(expectedMeta) || reparsePoint(expectedMeta))
            fail("Scene sidecar is missing: " + utf8(scenePath.wstring()));
        SceneDocument document;
        document.loadProject(scenePath, std::make_shared<AssetWorkspace>(root), false);
        const auto id = document.scene.id;
        const auto metadataId = validateSceneSidecar(scenePath, expectedMeta);
        if (metadataId != id)
            fail("Scene and sidecar UUIDs do not match");
        if (!byId.emplace(id, scenePath).second)
            fail("Duplicate scene UUID: " + id.string());
        result.push_back({scenePath, id});
    }
    for (const auto& sidecar : sidecars) {
        auto text = sidecar.wstring();
        text.resize(text.size() - 5); // remove the trailing .meta
        const fs::path scenePath(text);
        if (!sceneSet.contains(scenePath))
            fail("Scene sidecar has no scene document: " + utf8(sidecar.wstring()));
        // Parsing here also rejects a sidecar with unsupported extra fields;
        // the scene loop checks the exact UUID relationship.
        (void)validateSceneSidecar(scenePath, sidecar);
    }
    return result;
}

Candidate readCandidate(const fs::path& root, bool persistAdditionalIds) {
    validateFixedLayout(root);
    const auto manifestPath = projectPath(root, std::string(manifestName), true);
    if (!fs::exists(manifestPath) || !fs::is_regular_file(manifestPath) || reparsePoint(manifestPath))
        fail("Project manifest is missing");
    const auto manifestBytes = readDocument(manifestPath);
    JsonDoc document(manifestBytes);
    auto* object = document.root();
    strictKeys(object,
               {"format", "formatVersion", "projectId", "name", "engine", "startupScene", "contentRoots", "code",
                "graphicsSettings", "build"},
               "project manifest");
    if (text(yyjson_obj_get(object, "format"), "format") != "proto.project" || uintField(object, "formatVersion") != 1)
        fail("Unsupported project manifest format");

    ProjectInfo info;
    info.id = AssetId::parse(text(yyjson_obj_get(object, "projectId"), "projectId"));
    info.name = text(yyjson_obj_get(object, "name"), "name");
    if (info.name.empty() || info.name.size() > 1024)
        fail("Invalid project name");
    info.startupScene = AssetId::parse(text(yyjson_obj_get(object, "startupScene"), "startupScene"));

    auto* engine = yyjson_obj_get(object, "engine");
    strictKeys(engine, {"sdkVersion", "behaviorApiVersion"}, "engine");
    if (text(yyjson_obj_get(engine, "sdkVersion"), "sdkVersion") != "0.1" ||
        uintField(engine, "behaviorApiVersion") != 1)
        fail("Unsupported project engine contract");

    auto* roots = yyjson_obj_get(object, "contentRoots");
    if (!roots || !yyjson_is_arr(roots) || yyjson_arr_size(roots) != 2)
        fail("Project contentRoots must contain Assets and Scenes");
    if (text(yyjson_arr_get(roots, 0), "contentRoots") != "Assets" ||
        text(yyjson_arr_get(roots, 1), "contentRoots") != "Scenes")
        fail("Project contentRoots are not the M4 roots");

    auto* code = yyjson_obj_get(object, "code");
    strictKeys(code, {"sourceRoot", "registrationFile"}, "code");
    if (requireRelative(root, text(yyjson_obj_get(code, "sourceRoot"), "sourceRoot"), "sourceRoot") != "Code" ||
        requireRelative(root, text(yyjson_obj_get(code, "registrationFile"), "registrationFile"), "registrationFile") !=
            "Code/Behaviors.cpp")
        fail("Project code roots are not the M4 placeholders");

    const auto graphics =
        requireRelative(root, text(yyjson_obj_get(object, "graphicsSettings"), "graphicsSettings"), "graphicsSettings");
    if (graphics != "Config/graphics.json")
        fail("Project graphics settings path is invalid");

    auto* build = yyjson_obj_get(object, "build");
    strictKeys(build, {"target", "name", "additionalAssets"}, "build");
    ProjectBuildSettings buildSettings;
    buildSettings.target = text(yyjson_obj_get(build, "target"), "target");
    buildSettings.name = text(yyjson_obj_get(build, "name"), "name");
    if (buildSettings.target != "windows-x64" || buildSettings.name != info.name)
        fail("Project build placeholder is invalid");
    auto* additional = yyjson_obj_get(build, "additionalAssets");
    if (!additional || !yyjson_is_arr(additional))
        fail("Project additionalAssets must be an array");
    size_t index{}, count{};
    Json* value{};
    yyjson_arr_foreach(additional, index, count, value) {
        const auto path = text(value, "additionalAssets");
        std::string canonical = path;
        // The v0.1 document reserves this array for runtime dependencies;
        // existing projects use either an AssetId or a project-relative path.
        // M4 preserves both forms while still rejecting absolute/traversal
        // paths when a path is supplied.
        try {
            canonical = AssetId::parse(path).string();
        } catch (const std::exception&) {
            (void)requireRelative(root, path, "additionalAssets");
            if (persistAdditionalIds)
                canonical = additionalAssetIdentity(root, path).id;
        }
        buildSettings.additionalAssets.push_back(std::move(canonical));
    }

    const auto scenes = scanScenes(root);
    auto matching =
        std::find_if(scenes.begin(), scenes.end(), [&](const auto& scene) { return scene.id == info.startupScene; });
    if (matching == scenes.end())
        fail("Project startupScene does not resolve to a scene document");
    if (persistAdditionalIds) {
        std::vector<std::string> original;
        original.reserve(buildSettings.additionalAssets.size());
        size_t additionalIndex{};
        yyjson_arr_foreach(additional, additionalIndex, count, value)
            original.push_back(text(value, "additionalAssets"));
        if (original != buildSettings.additionalAssets) {
            const auto updated = canonicalizeAdditionalAssets(manifestBytes, buildSettings.additionalAssets);
            atomicWrite(manifestPath, updated, manifestBytes);
        }
    }
    return {info, matching->path, std::move(buildSettings), projectPath(root, graphics)};
}

std::string manifestBytes(const ProjectInfo& info) {
    std::ostringstream out;
    out << "{\n"
        << "  \"format\":\"proto.project\",\n"
        << "  \"formatVersion\":1,\n"
        << "  \"projectId\":" << jsonString(info.id.string()) << ",\n"
        << "  \"name\":" << jsonString(info.name) << ",\n"
        << "  \"engine\":{\"sdkVersion\":\"0.1\",\"behaviorApiVersion\":1},\n"
        << "  \"startupScene\":" << jsonString(info.startupScene.string()) << ",\n"
        << "  \"contentRoots\":[\"Assets\",\"Scenes\"],\n"
        << "  \"code\":{\"sourceRoot\":\"Code\",\"registrationFile\":\"Code/Behaviors.cpp\"},\n"
        << "  \"graphicsSettings\":\"Config/graphics.json\",\n"
        << "  \"build\":{\"target\":\"windows-x64\",\"name\":" << jsonString(info.name) << ",\"additionalAssets\":[]}\n"
        << "}\n";
    return out.str();
}

constexpr std::string_view graphicsBytes =
    "{\n"
    "  \"format\":\"proto.graphics\",\n"
    "  \"formatVersion\":1,\n"
    "  \"profile\":\"Balanced\",\n"
    "  \"renderScale\":1,\n"
    "  \"vSync\":true,\n"
    "  \"viewDistance\":300,\n"
    "  \"sunShadows\":{\"enabled\":true,\"cascades\":3,\"resolution\":2048,\"distance\":80,\"filter\":\"pcf3x3\"},\n"
    "  "
    "\"pointShadows\":{\"enabled\":true,\"resolution\":512,\"filterTaps\":9,\"poolBudgetMiB\":96,\"overflow\":"
    "\"multiPass\"}\n"
    "}\n";

constexpr std::string_view registrationBytes =
    "#include <Proto/Behavior.hpp>\n\n"
    "void RegisterProjectBehaviors(proto::sdk::BehaviorRegistry& registry) {\n"
    "    (void)registry;\n"
    "    // Register each class explicitly: registry.Add<YourBehavior>({stableTypeId, \"YourBehavior\", {}});\n"
    "}\n";

std::string sceneMetadata(const AssetId id) {
    return "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"assetId\":" + jsonString(id.string()) +
           ",\"kind\":\"Scene\"}\n";
}

fs::path createRootPath(const fs::path& input) {
    const auto root = fs::absolute(input).lexically_normal();
    if (fs::exists(root))
        fail("Project creation target already exists");
    const auto parent = root.parent_path().empty() ? fs::current_path() : root.parent_path();
    validatePathChain(parent);
    if (!fs::exists(parent) || !fs::is_directory(parent) || reparsePoint(parent))
        fail("Project creation parent does not exist or is redirected");
    const auto relative = utf8(root.filename().generic_wstring());
    (void)projectPath(parent, relative, true);
    return root;
}

} // namespace

struct ProjectSession::Lock {
    HANDLE handle{INVALID_HANDLE_VALUE};

    explicit Lock(HANDLE value) : handle(value) {}
    ~Lock() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

ProjectSession::ProjectSession(fs::path root, ProjectInfo info, fs::path startupScenePath,
                               ProjectBuildSettings buildSettings, fs::path graphicsSettingsPath,
                               std::shared_ptr<AssetWorkspace> workspace, std::unique_ptr<Lock> lock)
    : root_(std::move(root)), info_(std::move(info)), startupScenePath_(std::move(startupScenePath)),
      buildSettings_(std::move(buildSettings)), graphicsSettingsPath_(std::move(graphicsSettingsPath)),
      workspace_(std::move(workspace)), lock_(std::move(lock)), watcher_(std::make_unique<DirectoryWatcher>(root_)) {}

ProjectSession::~ProjectSession() = default;

std::shared_ptr<ProjectSession> ProjectSession::create(const fs::path& input, std::string name) {
    if (name.empty() || name.size() > 1024 || name.find('\0') != std::string::npos || !validUtf8(name))
        fail("Invalid project name");
    const auto root = createRootPath(input);
    const auto parent = root.parent_path();
    const auto stage =
        parent / fs::path(root.filename().wstring() + L".tmp-" + fs::path(Uuid::create().string()).wstring());
    if (fs::exists(stage))
        fail("Project staging path unexpectedly exists");
    if (!fs::create_directory(stage))
        fail("Cannot create project staging directory");
    bool published = false;
    try {
        for (const auto* folder : {"Assets", "Scenes", "Code", "Config", ".proto"})
            if (!fs::create_directory(stage / utf8Path(folder)))
                fail("Cannot create project folder");

        SceneDocument scene;
        scene.newScene();
        scene.scene.name = "Main";
        const auto sceneId = scene.scene.id;
        const auto scenePath = stage / "Scenes" / "Main.scene.json";
        atomicWrite(scenePath, encodeScene(scene.scene));
        atomicWrite(fs::path(scenePath.wstring() + L".meta"), sceneMetadata(sceneId));
        atomicWrite(stage / "Code" / "Behaviors.cpp", registrationBytes);
        atomicWrite(stage / "Config" / "graphics.json", graphicsBytes);

        ProjectInfo info{AssetId::create(), std::move(name), sceneId};
        atomicWrite(stage / std::string(manifestName), manifestBytes(info));

        if (!MoveFileExW(stage.c_str(), root.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            fail("Cannot publish project directory without overwrite (Windows error " + std::to_string(error) + ")");
        }
        published = true;
        return open(root);
    } catch (...) {
        if (!published) {
            std::error_code ignored;
            fs::remove_all(stage, ignored);
        }
        throw;
    }
}

std::shared_ptr<ProjectSession> ProjectSession::open(const fs::path& rootOrManifest) {
    const auto root = rootFromInput(rootOrManifest);
    const auto control = projectPath(root, ".proto", true);
    if (!fs::exists(control) && !fs::is_regular_file(projectPath(root, std::string(manifestName), true)))
        fail("Project manifest is missing");
    // Derived files are omitted by source control and may be discarded by the
    // user. Recreate only this service directory before acquiring its lock.
    std::error_code controlError;
    fs::create_directory(control, controlError);
    if (controlError || !fs::is_directory(control))
        fail("Cannot create the project service directory");
    // Recover an interrupted journal while no project writer is active.  This
    // runs before the scene/catalog scan and any candidate becomes visible.
    auto lockPath = projectPath(root, ".proto/editor.lock", true);
    const HANDLE handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        fail("Project is already open by another writer");
    auto lock = std::make_unique<Lock>(handle);
    try {
        FileTransaction::recover(root);
        auto candidate = readCandidate(root, true);
        auto workspace = std::make_shared<AssetWorkspace>(root);
        return std::shared_ptr<ProjectSession>(new ProjectSession(root, std::move(candidate.info),
                                                                  std::move(candidate.startupScenePath),
                                                                  std::move(candidate.buildSettings),
                                                                  std::move(candidate.graphicsSettingsPath),
                                                                  std::move(workspace), std::move(lock)));
    } catch (...) {
        throw;
    }
}

void ProjectSession::refresh() {
    // Recovery belongs exclusively to open(), before live Undo commands exist.
    // A refresh must never delete the current session's staged snapshots.
    auto candidate = readCandidate(root_, true);
    publishRefresh({std::move(candidate.info), std::move(candidate.startupScenePath),
                    std::move(candidate.buildSettings), std::move(candidate.graphicsSettingsPath)});
}
ProjectSession::RefreshCandidate ProjectSession::prepareRefresh() const {
    auto candidate = readCandidate(root_, false);
    return {std::move(candidate.info), std::move(candidate.startupScenePath), std::move(candidate.buildSettings),
            std::move(candidate.graphicsSettingsPath)};
}
void ProjectSession::publishRefresh(RefreshCandidate&& candidate) noexcept {
    // The old state is untouched until the complete candidate has validated.
    using std::swap;
    swap(info_, candidate.info);
    swap(startupScenePath_, candidate.startupScenePath);
    swap(buildSettings_, candidate.buildSettings);
    swap(graphicsSettingsPath_, candidate.graphicsSettingsPath);
}

bool ProjectSession::consumeChanged() noexcept {
    return watcher_ && watcher_->consumeChanged();
}

} // namespace proto
