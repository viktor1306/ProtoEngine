#include "project/ProjectBuild.hpp"

#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "project/FileTransactions.hpp"
#include "project/ManagedProcess.hpp"
#include "project/BuildFileTree.hpp"

#include <windows.h>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <regex>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace proto {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view kSourceRoot = "Code";
constexpr std::string_view kRegistrationFile = "Code/Behaviors.cpp";
constexpr size_t kSdkJsonLimit = 1024 * 1024;
constexpr size_t kCompilerIdentityLimit = 1024 * 1024;
constexpr size_t kDescribeLimit = 4 * 1024 * 1024;
constexpr size_t kMaxSourceFiles = 16384;
constexpr size_t kBuildDirectoryKeyLength = 16;

struct SdkMetadata {
    fs::path root;
    std::string sdkBuildId;
    std::string configuration;
    fs::path compiler;
    fs::path cmake;
    fs::path ninja;
    std::string expectedCompilerVersion;
    std::string expectedCompilerTarget;
    std::string cxxFlags;
    std::string cxxFlagsDebug;
    std::string cxxFlagsRelease;
    std::string exeLinkerFlags;
};

struct SourceFile {
    std::string relative;
    fs::path original;
    std::string bytes;
    std::string hash;
};

struct SourceSnapshot {
    std::vector<SourceFile> files;
    size_t bytes{};
};

struct ProcessResult {
    uint32_t exitCode{};
    std::string stdoutBytes;
    std::string stderrBytes;
    bool stdoutTruncated{};
    bool stderrTruncated{};
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void report(const BuildProgressCallback& callback, std::string_view stage, std::string_view message) {
    if (!callback)
        return;
    callback(BuildProgress{std::string(stage), std::string(message)});
}

void checkCancelled(const std::atomic_bool& cancel) {
    if (cancel.load(std::memory_order_relaxed))
        throw ProjectBuildError("Build cancelled", {{BuildDiagnosticLevel::Error, "cancel", "Build cancelled"}});
}

std::string trim(std::string_view input) {
    size_t begin{};
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
        ++begin;
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;
    return std::string(input.substr(begin, end - begin));
}

std::string canonicalLineEndings(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\r') {
            if (i + 1 < value.size() && value[i + 1] == '\n')
                ++i;
            result.push_back('\n');
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

bool isReparse(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void requireDirectory(const fs::path& path, std::string_view label) {
    if (!fs::exists(path) || !fs::is_directory(path) || isReparse(path))
        fail(std::string(label) + " is missing, not a directory, or is a reparse point");
}

fs::path requireTool(const fs::path& root, std::string_view field, const std::string& value) {
    if (value.empty() || !validUtf8(value))
        fail("SDK " + std::string(field) + " path is empty or invalid UTF-8");
    const auto path = utf8Path(value);
    if (!path.is_absolute() || path.has_root_directory() == false || isReparse(path) || !fs::is_regular_file(path))
        fail("SDK " + std::string(field) + " must be an absolute regular file");
    (void)root;
    return path.lexically_normal();
}

std::string pathText(const fs::path& path) {
    return utf8(path.generic_wstring());
}

fs::path cmakePath(const fs::path& path) {
    const auto required = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (!required)
        return path;
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    const auto length = GetShortPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        return path;
    const std::wstring shortPath(buffer.data(), length);
    if (std::any_of(shortPath.begin(), shortPath.end(), [](wchar_t c) { return c > 127; }))
        return path;
    return fs::path(shortPath);
}

std::string cmakePathText(const fs::path& path) {
    return pathText(cmakePath(path));
}

void strictKeys(Json* object, std::initializer_list<std::string_view> allowed, std::string_view context) {
    if (!yyjson_is_obj(object))
        fail("Expected SDK object: " + std::string(context));
    std::unordered_set<std::string_view> seen;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (!seen.insert(name).second)
            fail("Duplicate SDK field: " + std::string(name));
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            fail("Unsupported SDK field: " + std::string(name));
    }
}

std::string requiredString(Json* object, const char* key, std::string_view context) {
    auto* value = yyjson_obj_get(object, key);
    if (!value || !yyjson_is_str(value))
        fail("SDK field must be a string: " + std::string(context) + "." + key);
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos || !validUtf8(result))
        fail("SDK field is not valid UTF-8: " + std::string(context) + "." + key);
    return result;
}

std::string optionalString(Json* object, const char* key, std::string_view context) {
    auto* value = yyjson_obj_get(object, key);
    if (!value)
        return {};
    return requiredString(object, key, context);
}

uint64_t requiredUnsigned(Json* object, const char* key, std::string_view context) {
    auto* value = yyjson_obj_get(object, key);
    if (!value || !yyjson_is_uint(value))
        fail("SDK field must be an unsigned integer: " + std::string(context) + "." + key);
    return yyjson_get_uint(value);
}

std::string compilerPathString(Json* object, const char* key) {
    auto* value = yyjson_obj_get(object, key);
    if (!value)
        fail("SDK field missing: " + std::string(key));
    if (yyjson_is_str(value))
        return requiredString(object, key, "sdk");
    // A package may keep the path alongside an identity object.  Accept this
    // representation while still validating the public path contract.
    if (yyjson_is_obj(value)) {
        strictKeys(value, {"path", "version", "target"}, key);
        return requiredString(value, "path", key);
    }
    fail("SDK field must be a path string: " + std::string(key));
}

SdkMetadata readSdk(const ProjectBuildRequest& request) {
    const auto root = fs::absolute(request.sdkRoot).lexically_normal();
    requireDirectory(root, "SDK root");
    const auto sdkPath = root / "sdk.json";
    if (isReparse(sdkPath) || !fs::is_regular_file(sdkPath))
        fail("SDK sdk.json is missing or redirected");
    const auto bytes = assetBytes(sdkPath, kSdkJsonLimit);
    const std::string document(bytes.begin(), bytes.end());
    JsonDoc json(document);
    auto* object = json.root();
    strictKeys(object,
               {"format", "formatVersion", "behaviorApiVersion", "sdkBuildId", "configuration", "compiler", "cmake",
                "ninja", "compilerVersion", "compilerTarget", "cxxFlags", "cxxFlagsDebug", "cxxFlagsRelease",
                "exeLinkerFlags"},
               "sdk");
    if (requiredString(object, "format", "sdk") != "proto.sdk" ||
        requiredUnsigned(object, "formatVersion", "sdk") != 1 ||
        requiredUnsigned(object, "behaviorApiVersion", "sdk") != sdk::behaviorApiVersion)
        fail("Unsupported SDK package format or behavior API version");
    SdkMetadata metadata;
    metadata.root = root;
    metadata.sdkBuildId = requiredString(object, "sdkBuildId", "sdk");
    metadata.configuration = requiredString(object, "configuration", "sdk");
    if (metadata.sdkBuildId.empty() || metadata.sdkBuildId.size() > 4096)
        fail("SDK build identity is empty or too long");
    if (metadata.configuration != "Debug" && metadata.configuration != "Release")
        fail("SDK configuration must be Debug or Release");
    metadata.compiler = requireTool(root, "compiler", compilerPathString(object, "compiler"));
    metadata.cmake = requireTool(root, "cmake", requiredString(object, "cmake", "sdk"));
    metadata.ninja = requireTool(root, "ninja", requiredString(object, "ninja", "sdk"));
    metadata.expectedCompilerVersion = optionalString(object, "compilerVersion", "sdk");
    metadata.expectedCompilerTarget = optionalString(object, "compilerTarget", "sdk");
    metadata.cxxFlags = optionalString(object, "cxxFlags", "sdk");
    metadata.cxxFlagsDebug = optionalString(object, "cxxFlagsDebug", "sdk");
    metadata.cxxFlagsRelease = optionalString(object, "cxxFlagsRelease", "sdk");
    metadata.exeLinkerFlags = optionalString(object, "exeLinkerFlags", "sdk");
    if (auto* compiler = yyjson_obj_get(object, "compiler"); compiler && yyjson_is_obj(compiler)) {
        metadata.expectedCompilerVersion = metadata.expectedCompilerVersion.empty()
                                               ? optionalString(compiler, "version", "compiler")
                                               : metadata.expectedCompilerVersion;
        metadata.expectedCompilerTarget = metadata.expectedCompilerTarget.empty()
                                              ? optionalString(compiler, "target", "compiler")
                                              : metadata.expectedCompilerTarget;
    }
    return metadata;
}

bool sourceExtension(const fs::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; });
    return extension == L".cpp" || extension == L".cc" || extension == L".cxx" || extension == L".h" ||
           extension == L".hh" || extension == L".hpp" || extension == L".hxx" || extension == L".inl" ||
           extension == L".ipp" || extension == L".tcc" || extension == L".inc";
}

SourceSnapshot snapshotSources(const ProjectBuildRequest& request, const std::atomic_bool& cancel) {
    if (request.sourceRoot != kSourceRoot || request.registrationFile != kRegistrationFile)
        fail("M5 project Code root and registration file are fixed to Code and Code/Behaviors.cpp");
    const auto root = fs::absolute(request.projectRoot).lexically_normal();
    requireDirectory(root, "Project root");
    const auto code = projectPath(root, std::string(kSourceRoot));
    requireDirectory(code, "Project Code root");
    SourceSnapshot snapshot;
    fs::recursive_directory_iterator iterator(code, fs::directory_options::skip_permission_denied);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
        checkCancelled(cancel);
        const auto path = iterator->path();
        if (isReparse(path)) {
            if (iterator->is_directory())
                iterator.disable_recursion_pending();
            fail("Project Code contains a reparse point: " + utf8(path.wstring()));
        }
        if (iterator->is_directory())
            continue;
        if (!iterator->is_regular_file() || !sourceExtension(path))
            continue;
        const auto relative = projectRelative(root, path);
        // projectRelative/projectPath reject absolute/traversal/escaped paths
        // and retain the project's forward-slash UTF-8 identity.
        (void)projectPath(root, relative);
        const auto stampBefore = projectStamp(root, relative);
        if (stampBefore.kind != FileKind::File || stampBefore.size > request.maxSourceFileBytes)
            fail("Project source exceeds the per-file build limit: " + relative);
        const auto bytes = assetBytes(path, request.maxSourceFileBytes);
        const auto stampAfter = projectStamp(root, relative);
        if (stampBefore != stampAfter)
            fail("Project source changed while it was being snapshotted: " + relative);
        if (snapshot.bytes > request.maxSourceBytes - std::min(request.maxSourceBytes, bytes.size()))
            fail("Project Code exceeds the total build source limit");
        SourceFile source;
        source.relative = relative;
        source.original = path;
        source.bytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        source.hash = sha256(source.bytes);
        if (snapshot.files.size() >= kMaxSourceFiles)
            fail("Project Code contains too many supported source files");
        snapshot.bytes += source.bytes.size();
        snapshot.files.push_back(std::move(source));
    }
    std::sort(snapshot.files.begin(), snapshot.files.end(),
              [](const SourceFile& a, const SourceFile& b) { return a.relative < b.relative; });
    if (snapshot.files.empty())
        fail("Project Code contains no supported C++ source or header files");
    if (std::none_of(snapshot.files.begin(), snapshot.files.end(),
                     [](const SourceFile& file) { return file.relative == kRegistrationFile; }))
        fail("Project registration file is missing: Code/Behaviors.cpp");
    return snapshot;
}

std::string runOutput(const ProcessResult& result) {
    std::string output;
    output.reserve(result.stdoutBytes.size() + result.stderrBytes.size() + 64);
    output += result.stdoutBytes;
    if (!result.stderrBytes.empty()) {
        if (!output.empty() && output.back() != '\n')
            output.push_back('\n');
        output += result.stderrBytes;
    }
    if (result.stdoutTruncated || result.stderrTruncated)
        output += "\n[process output truncated]\n";
    return output;
}

ProcessResult runProcess(const ProcessSpec& spec, const std::atomic_bool& cancel, std::chrono::milliseconds timeout,
                         std::string_view stage, const BuildProgressCallback& progress) {
    ManagedProcess process;
    process.start(spec);
    ProcessResult result;
    const auto started = Clock::now();
    for (;;) {
        const auto poll = process.poll();
        auto out = process.takeStdout();
        auto err = process.takeStderr();
        result.stdoutBytes += out;
        result.stderrBytes += err;
        result.stdoutTruncated = result.stdoutTruncated || poll.stdoutTruncated;
        result.stderrTruncated = result.stderrTruncated || poll.stderrTruncated;
        if (progress && !out.empty())
            progress(BuildProgress{std::string(stage), std::move(out)});
        if (progress && !err.empty())
            progress(BuildProgress{std::string(stage), std::move(err)});
        if (cancel.load(std::memory_order_relaxed)) {
            process.forceStop(std::chrono::milliseconds(2000));
            throw ProjectBuildError(std::string(stage) + " cancelled",
                                    {{BuildDiagnosticLevel::Error, std::string(stage), "Build cancelled"}});
        }
        if (!poll.running) {
            result.exitCode = poll.exitCode.value_or(1);
            break;
        }
        if (timeout.count() >= 0 && Clock::now() - started >= timeout) {
            process.forceStop(std::chrono::milliseconds(2000));
            throw ProjectBuildError(std::string(stage) + " timed out",
                                    {{BuildDiagnosticLevel::Error, std::string(stage), "Process timeout"}});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // There can be a final pipe batch after the process signalled exit.
    for (int i = 0; i != 8; ++i) {
        const auto poll = process.poll();
        auto out = process.takeStdout();
        auto err = process.takeStderr();
        result.stdoutBytes += out;
        result.stderrBytes += err;
        result.stdoutTruncated = result.stdoutTruncated || poll.stdoutTruncated;
        result.stderrTruncated = result.stderrTruncated || poll.stderrTruncated;
        if (progress && !out.empty())
            progress(BuildProgress{std::string(stage), std::move(out)});
        if (progress && !err.empty())
            progress(BuildProgress{std::string(stage), std::move(err)});
        if (!poll.stdoutBytesRead && !poll.stderrBytesRead)
            break;
    }
    report(progress, stage, "process exited with code " + std::to_string(result.exitCode));
    return result;
}

std::string compilerIdentity(const SdkMetadata& sdk, const std::atomic_bool& cancel,
                             const BuildProgressCallback& progress, std::string& targetIdentity) {
    ProcessSpec version;
    version.executable = sdk.compiler;
    version.arguments = {"--version"};
    version.workingDirectory = sdk.root;
    version.stdoutLimit = kCompilerIdentityLimit;
    version.stderrLimit = kCompilerIdentityLimit;
    report(progress, "compiler", "reading compiler identity");
    const auto versionResult = runProcess(version, cancel, std::chrono::seconds(30), "compiler", progress);
    if (versionResult.exitCode != 0)
        fail("Compiler --version failed (" + std::to_string(versionResult.exitCode) + "): " + runOutput(versionResult));
    const auto versionText = canonicalLineEndings(
        trim(versionResult.stdoutBytes.empty() ? versionResult.stderrBytes : versionResult.stdoutBytes));
    if (versionText.empty())
        fail("Compiler --version returned no identity");
    if (!sdk.expectedCompilerVersion.empty() && versionText != trim(canonicalLineEndings(sdk.expectedCompilerVersion)))
        fail("Compiler identity does not match SDK compilerVersion");

    ProcessSpec target;
    target.executable = sdk.compiler;
    target.arguments = {"-dumpmachine"};
    target.workingDirectory = sdk.root;
    target.stdoutLimit = 64 * 1024;
    target.stderrLimit = 64 * 1024;
    const auto targetResult = runProcess(target, cancel, std::chrono::seconds(30), "compiler", progress);
    if (targetResult.exitCode != 0)
        fail("Compiler target query failed: " + runOutput(targetResult));
    targetIdentity = trim(targetResult.stdoutBytes);
    if (targetIdentity.empty())
        targetIdentity = trim(targetResult.stderrBytes);
    if (!sdk.expectedCompilerTarget.empty() && targetIdentity != sdk.expectedCompilerTarget)
        fail("Compiler target does not match SDK compilerTarget");
    return versionText;
}

std::string makeKey(const SdkMetadata& sdk, const ProjectBuildRequest& request, const SourceSnapshot& snapshot,
                    std::string_view compilerVersion, std::string_view compilerTarget) {
    std::string material;
    material.reserve(snapshot.bytes + snapshot.files.size() * 128 + 1024);
    const auto add = [&](std::string_view value) {
        material.append(value);
        material.push_back('\0');
    };
    add("proto.m5.build");
    add(pathText(fs::absolute(request.projectRoot).lexically_normal()));
    add(sdk.sdkBuildId);
    add(sdk.configuration);
    add(request.configuration);
    add(pathText(sdk.compiler));
    add(pathText(sdk.cmake));
    add(pathText(sdk.ninja));
    add(compilerVersion);
    add(compilerTarget);
    add(request.sourceRoot);
    add(request.registrationFile);
    add("-G Ninja");
    add("-D CMAKE_CXX_COMPILER");
    add("-D CMAKE_MAKE_PROGRAM");
    add("-D CMAKE_PREFIX_PATH");
    add("-D ProtoSDK_DIR");
    add("-D CMAKE_BUILD_TYPE");
    add(sdk.cxxFlags);
    add(sdk.cxxFlagsDebug);
    add(sdk.cxxFlagsRelease);
    add(sdk.exeLinkerFlags);
    add(std::to_string(request.parallelism));
    for (const auto& source : snapshot.files) {
        add(source.relative);
        add(std::to_string(source.bytes.size()));
        material.append(source.bytes);
        material.push_back('\0');
    }
    return sha256(material);
}

void ensureDirectoryTree(const fs::path& path) {
    if (isReparse(path))
        fail("Build path is a reparse point: " + utf8(path.wstring()));
    std::error_code error;
    fs::create_directories(path, error);
    if (error || !fs::is_directory(path) || isReparse(path))
        fail("Cannot create internal build directory: " + utf8(path.wstring()));
}

void writeBytes(const fs::path& path, std::string_view bytes) {
    if (isReparse(path))
        fail("Generated build path is a reparse point: " + utf8(path.wstring()));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        fail("Cannot write generated build file: " + utf8(path.wstring()));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output)
        fail("Cannot finish generated build file: " + utf8(path.wstring()));
}

std::string snapshotRelative(const fs::path& path, const fs::path& sourceRoot) {
    return pathText(path.lexically_relative(sourceRoot));
}

void addExpectedDirectories(const fs::path& local, std::set<std::string>& directories) {
    auto parent = local.parent_path();
    while (!parent.empty() && parent != ".") {
        directories.insert(pathText(parent));
        const auto next = parent.parent_path();
        if (next == parent)
            break;
        parent = next;
    }
}

bool snapshotMatches(const fs::path& sourceRoot, const SourceSnapshot& snapshot) {
    if (!fs::exists(sourceRoot))
        return false;
    if (isReparse(sourceRoot))
        fail("Internal source snapshot is a reparse point");
    if (!fs::is_directory(sourceRoot))
        fail("Internal source snapshot is not a directory");

    std::map<std::string, const SourceFile*> expectedFiles;
    std::set<std::string> expectedDirectories;
    for (const auto& source : snapshot.files) {
        const auto local = utf8Path(source.relative).lexically_relative(utf8Path(std::string(kSourceRoot)));
        const auto localName = pathText(local);
        expectedFiles.emplace(localName, &source);
        addExpectedDirectories(local, expectedDirectories);
    }
    std::set<std::string> expectedFileNames;
    for (const auto& [name, source] : expectedFiles) {
        (void)source;
        expectedFileNames.insert(name);
    }

    std::set<std::string> actualFiles;
    std::set<std::string> actualDirectories;
    fs::recursive_directory_iterator iterator(sourceRoot, fs::directory_options::skip_permission_denied);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
        const auto path = iterator->path();
        if (isReparse(path))
            fail("Internal source snapshot contains a reparse point: " + utf8(path.wstring()));
        const auto relative = snapshotRelative(path, sourceRoot);
        if (iterator->is_directory())
            actualDirectories.insert(relative);
        else if (iterator->is_regular_file())
            actualFiles.insert(relative);
        else
            fail("Internal source snapshot contains an unsupported entry: " + utf8(path.wstring()));
    }
    if (actualFiles != expectedFileNames || actualDirectories != expectedDirectories)
        return false;

    for (const auto& [relative, source] : expectedFiles) {
        const auto path = sourceRoot / utf8Path(relative);
        const auto size = fs::file_size(path);
        if (size != source->bytes.size())
            return false;
        const auto bytes = assetBytes(path, source->bytes.size());
        if (bytes.size() != source->bytes.size() ||
            !std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const uint8_t*>(source->bytes.data())))
            return false;
    }
    return true;
}

fs::path writeSnapshot(const ProjectBuildRequest& request, const SourceSnapshot& snapshot, std::string_view key) {
    const auto root = fs::absolute(request.projectRoot).lexically_normal();
    if (key.size() < kBuildDirectoryKeyLength)
        fail("Build content key is unexpectedly short");
    // Keep the full content key in success.json/key.txt while using a short
    // derived directory name.  CMake creates several nested TryCompile paths
    // and Windows path budgets are otherwise easy to exceed for valid project
    // roots containing Unicode and spaces.
    const auto directoryKey = std::string(key.substr(0, kBuildDirectoryKeyLength));
    const auto keyRelative = std::string(".proto/builds/") + directoryKey;
    const auto buildRoot = projectPath(root, keyRelative, true);
    if (fs::exists(buildRoot) && isReparse(buildRoot))
        fail("Internal build directory is a reparse point");
    ensureDirectoryTree(buildRoot);
    const auto keyMarker = buildRoot / "key.txt";
    if (fs::exists(keyMarker)) {
        if (isReparse(keyMarker))
            fail("Build key marker is a reparse point");
        const auto marker = assetBytes(keyMarker, 256);
        const std::string markerText(marker.begin(), marker.end());
        if (trim(markerText) != key)
            fail("Build directory key prefix collision");
    } else {
        writeBytes(keyMarker, std::string(key) + "\n");
    }
    const auto sourceRoot = buildRoot / "src";
    if (!snapshotMatches(sourceRoot, snapshot)) {
        if (fs::exists(sourceRoot) && isReparse(sourceRoot))
            fail("Internal source snapshot is a reparse point");
        std::error_code error;
        if (fs::exists(sourceRoot))
            build_detail::removeTree(sourceRoot, error);
        if (error)
            fail("Cannot replace internal source snapshot");
        ensureDirectoryTree(sourceRoot);
        for (const auto& source : snapshot.files) {
            const auto output =
                sourceRoot / utf8Path(source.relative).lexically_relative(utf8Path(std::string(kSourceRoot)));
            ensureDirectoryTree(output.parent_path());
            writeBytes(output, source.bytes);
        }
    }
    return buildRoot;
}

std::string cmakeQuote(std::string_view value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"')
            result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

std::string generatedCMake(const SdkMetadata& sdk, const SourceSnapshot& snapshot) {
    std::ostringstream cmake;
    cmake << "cmake_minimum_required(VERSION 3.28)\n"
          << "set(ENV{CXXFLAGS} \"\")\n"
          << "set(ENV{CPPFLAGS} \"\")\n"
          << "set(ENV{LDFLAGS} \"\")\n"
          << "set(ENV{CXX} \"\")\n"
          << "set(ENV{CC} \"\")\n"
          << "set(ENV{CPATH} \"\")\n"
          << "set(ENV{CPLUS_INCLUDE_PATH} \"\")\n"
          << "set(ENV{LIBRARY_PATH} \"\")\n"
          << "set(ENV{COMPILER_PATH} \"\")\n"
          << "set(ENV{GCC_EXEC_PREFIX} \"\")\n"
          << "set(CMAKE_CXX_FLAGS " << cmakeQuote(sdk.cxxFlags) << " CACHE STRING \"\" FORCE)\n"
          << "set(CMAKE_CXX_FLAGS_DEBUG " << cmakeQuote(sdk.cxxFlagsDebug) << " CACHE STRING \"\" FORCE)\n"
          << "set(CMAKE_CXX_FLAGS_RELEASE " << cmakeQuote(sdk.cxxFlagsRelease) << " CACHE STRING \"\" FORCE)\n"
          << "set(CMAKE_EXE_LINKER_FLAGS " << cmakeQuote(sdk.exeLinkerFlags) << " CACHE STRING \"\" FORCE)\n"
          << "project(ProtoPlayerProject LANGUAGES CXX)\n"
          << "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/bin\")\n"
          << "find_package(ProtoSDK CONFIG REQUIRED)\n"
          << "proto_add_player(ProtoPlayer\n  SOURCES\n";
    for (const auto& source : snapshot.files) {
        const auto relativeToSnapshot =
            utf8Path(source.relative).lexically_relative(utf8Path(std::string(kSourceRoot)));
        cmake << "    " << cmakeQuote("src/" + pathText(relativeToSnapshot)) << "\n";
    }
    cmake << ")\n";
    return cmake.str();
}

void writeCMake(const fs::path& buildRoot, const SdkMetadata& sdk, const SourceSnapshot& snapshot) {
    const auto path = buildRoot / "CMakeLists.txt";
    const auto expected = generatedCMake(sdk, snapshot);
    if (fs::exists(path)) {
        if (isReparse(path))
            fail("Generated CMake file is a reparse point");
        if (!fs::is_regular_file(path))
            fail("Generated CMake path is not a file");
        if (fs::file_size(path) == expected.size()) {
            const auto bytes = assetBytes(path, expected.size());
            if (bytes.size() == expected.size() &&
                std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const uint8_t*>(expected.data())))
                return;
        }
    }
    writeBytes(path, expected);
}

std::vector<BuildDiagnostic> diagnosticsFromOutput(std::string_view stage, std::string output,
                                                   const fs::path& buildRoot, const fs::path& projectRoot) {
    const auto snapshotRoot = (buildRoot / "src").wstring();
    const auto snapshotGeneric = (buildRoot / "src").generic_wstring();
    const auto snapshotCmakeRoot = cmakePath(buildRoot / "src").wstring();
    const auto snapshotCmakeGeneric = cmakePath(buildRoot / "src").generic_wstring();
    const auto originalRoot = (projectRoot / "Code").wstring();
    const auto originalGeneric = (projectRoot / "Code").generic_wstring();
    std::vector<BuildDiagnostic> diagnostics;
    const std::regex gccLocation(R"(^(.+):([0-9]+):([0-9]+):)");
    const std::regex msvcLocation(R"(^(.+)\(([0-9]+),([0-9]+)\):)");
    size_t start{};
    while (start <= output.size()) {
        const auto end = output.find('\n', start);
        std::string line = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) {
            if (end == std::string::npos)
                break;
            start = end + 1;
            continue;
        }
        auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
            if (from.empty())
                return;
            size_t position{};
            while ((position = text.find(from, position)) != std::string::npos) {
                text.replace(position, from.size(), to);
                position += to.size();
            }
        };
        replaceAll(line, utf8(snapshotRoot), utf8(originalRoot));
        replaceAll(line, utf8(snapshotGeneric), utf8(originalGeneric));
        replaceAll(line, utf8(snapshotCmakeRoot), utf8(originalRoot));
        replaceAll(line, utf8(snapshotCmakeGeneric), utf8(originalGeneric));
        BuildDiagnostic diagnostic;
        diagnostic.level = (line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos)
                               ? BuildDiagnosticLevel::Warning
                               : BuildDiagnosticLevel::Error;
        diagnostic.stage = std::string(stage);
        std::smatch location;
        if (std::regex_search(line, location, gccLocation) || std::regex_search(line, location, msvcLocation)) {
            diagnostic.sourcePath = location[1].str();
            try {
                diagnostic.line = static_cast<unsigned>(std::stoul(location[2].str()));
                diagnostic.column = static_cast<unsigned>(std::stoul(location[3].str()));
            } catch (const std::exception&) {
                diagnostic.line.reset();
                diagnostic.column.reset();
            }
            // Some Ninja generators print a source path relative to the
            // binary tree (../src/Foo.cpp) instead of the absolute snapshot
            // path.  Resolve that bounded marker back to authored Code so the
            // editor can open the diagnostic reliably.
            if (diagnostic.sourcePath) {
                auto source = *diagnostic.sourcePath;
                std::replace(source.begin(), source.end(), '\\', '/');
                const auto marker = source.find("src/");
                if (marker != std::string::npos) {
                    try {
                        const auto relative = source.substr(marker + 4);
                        diagnostic.sourcePath = utf8(projectPath(projectRoot, "Code/" + relative).wstring());
                    } catch (const std::exception&) {
                        // Keep the compiler's original path when it does not
                        // identify one of the snapshotted Code files.
                    }
                }
            }
        }
        diagnostic.message = std::move(line);
        diagnostics.push_back(std::move(diagnostic));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return diagnostics;
}

void appendDiagnostics(std::vector<BuildDiagnostic>& target, std::vector<BuildDiagnostic> source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

void runCMake(const ProjectBuildRequest& request, const SdkMetadata& sdk, const SourceSnapshot& snapshot,
              const fs::path& buildRoot, std::atomic_bool& cancel, const BuildProgressCallback& progress,
              std::vector<BuildDiagnostic>& diagnostics) {
    // Keep the generated source and CMake binary tree separate.  The SDK
    // package's proto_add_player places the executable in binary/bin.
    const auto binaryRoot = buildRoot / "binary";
    ensureDirectoryTree(binaryRoot);
    ProcessSpec configure;
    configure.executable = sdk.cmake;
    configure.workingDirectory = cmakePath(buildRoot);
    configure.stdoutLimit = request.processOutputLimit;
    configure.stderrLimit = request.processOutputLimit;
    configure.clearToolchainEnvironment = true;
    configure.toolchainDirectory = sdk.compiler.parent_path();
    configure.arguments = {"-S",
                           cmakePathText(buildRoot),
                           "-B",
                           cmakePathText(binaryRoot),
                           "-G",
                           "Ninja",
                           "-DCMAKE_BUILD_TYPE=" + request.configuration,
                           "-DCMAKE_CXX_COMPILER=" + cmakePathText(sdk.compiler),
                           "-DCMAKE_MAKE_PROGRAM=" + cmakePathText(sdk.ninja),
                           "-DCMAKE_PREFIX_PATH=" + cmakePathText(sdk.root),
                           "-DProtoSDK_DIR=" + cmakePathText(sdk.root / "lib" / "cmake" / "ProtoSDK")};
    report(progress, "configure", "running CMake configure");
    const auto configured = runProcess(configure, cancel, request.configureTimeout, "configure", progress);
    appendDiagnostics(diagnostics, diagnosticsFromOutput("configure", runOutput(configured), buildRoot,
                                                         fs::absolute(request.projectRoot).lexically_normal()));
    if (configured.exitCode != 0)
        throw ProjectBuildError("CMake configure failed", diagnostics);

    checkCancelled(cancel);
    ProcessSpec build;
    build.executable = sdk.cmake;
    build.workingDirectory = cmakePath(buildRoot);
    build.stdoutLimit = request.processOutputLimit;
    build.stderrLimit = request.processOutputLimit;
    build.clearToolchainEnvironment = true;
    build.toolchainDirectory = sdk.compiler.parent_path();
    build.arguments = {"--build",  cmakePathText(binaryRoot), "--parallel", std::to_string(request.parallelism),
                       "--config", request.configuration};
    report(progress, "build", "running CMake build");
    const auto built = runProcess(build, cancel, request.buildTimeout, "build", progress);
    appendDiagnostics(diagnostics, diagnosticsFromOutput("build", runOutput(built), buildRoot,
                                                         fs::absolute(request.projectRoot).lexically_normal()));
    if (built.exitCode != 0)
        throw ProjectBuildError("CMake build failed", diagnostics);
    (void)snapshot;
}

bool sameSnapshot(const SourceSnapshot& before, const SourceSnapshot& after) {
    if (before.files.size() != after.files.size() || before.bytes != after.bytes)
        return false;
    for (size_t i = 0; i != before.files.size(); ++i) {
        if (before.files[i].relative != after.files[i].relative || before.files[i].hash != after.files[i].hash ||
            before.files[i].bytes.size() != after.files[i].bytes.size())
            return false;
    }
    return true;
}

void writeSuccessMetadata(const fs::path& buildRoot, std::string_view key, std::string_view executableHash,
                          std::string_view sdkBuildId) {
    const auto data = std::string("{\n  \"format\":\"proto.build-success\",\n  \"formatVersion\":1,\n  \"key\":") +
                      jsonString(key) + ",\n  \"sdkBuildId\":" + jsonString(sdkBuildId) +
                      ",\n  \"executableSha256\":" + jsonString(executableHash) + "\n}\n";
    writeBytes(buildRoot / "success.json", data);
}

} // namespace

ProjectBuildError::ProjectBuildError(std::string message, std::vector<BuildDiagnostic> diagnostics)
    : std::runtime_error(std::move(message)), diagnostics_(std::move(diagnostics)) {}

ProjectBuildResult buildProject(const ProjectBuildRequest& request, std::atomic_bool& cancel,
                                BuildProgressCallback progress) {
    std::vector<BuildDiagnostic> diagnostics;
    try {
        checkCancelled(cancel);
        if (!request.parallelism || request.parallelism > 64)
            fail("Build parallelism is outside the supported bound");
        if (!request.maxSourceFileBytes || request.maxSourceFileBytes > request.maxSourceBytes)
            fail("Invalid source size limits");
        const auto sdk = readSdk(request);
        if (!request.expectedSdkBuildId.empty() && sdk.sdkBuildId != request.expectedSdkBuildId)
            throw ProjectBuildError("SDK build identity does not match this editor");
        auto effectiveRequest = request;
        if (effectiveRequest.configuration.empty())
            effectiveRequest.configuration = sdk.configuration;
        if (effectiveRequest.configuration != "Debug" && effectiveRequest.configuration != "Release")
            fail("Build configuration must be Debug or Release");
        const auto snapshot = snapshotSources(effectiveRequest, cancel);
        std::string compilerTarget;
        const auto compilerVersion = compilerIdentity(sdk, cancel, progress, compilerTarget);
        const auto key = makeKey(sdk, effectiveRequest, snapshot, compilerVersion, compilerTarget);
        report(progress, "snapshot", "publishing internal Code snapshot");
        const auto buildRoot = writeSnapshot(effectiveRequest, snapshot, key);
        writeCMake(buildRoot, sdk, snapshot);
        runCMake(effectiveRequest, sdk, snapshot, buildRoot, cancel, progress, diagnostics);
        checkCancelled(cancel);

        const auto executable = buildRoot / "binary" / "bin" / "ProtoPlayer.exe";
        if (isReparse(executable) || !fs::is_regular_file(executable))
            throw ProjectBuildError("CMake build did not produce ProtoPlayer.exe", diagnostics);
        const auto executableBytes = assetBytes(executable, 512 * 1024 * 1024);
        const auto executableHash = sha256(std::span<const uint8_t>(executableBytes.data(), executableBytes.size()));

        report(progress, "describe", "discovering behavior schema");
        ProcessSpec describe;
        describe.executable = executable;
        describe.workingDirectory = executable.parent_path();
        describe.arguments = {"--describe-behaviors"};
        describe.stdoutLimit = std::min(effectiveRequest.processOutputLimit, kDescribeLimit);
        describe.stderrLimit = std::min(effectiveRequest.processOutputLimit, kDescribeLimit);
        const auto described = runProcess(describe, cancel, effectiveRequest.describeTimeout, "describe", progress);
        appendDiagnostics(diagnostics, diagnosticsFromOutput("describe", described.stderrBytes, buildRoot,
                                                             fs::absolute(request.projectRoot).lexically_normal()));
        if (described.exitCode != 0)
            throw ProjectBuildError("Behavior description failed", diagnostics);
        if (described.stdoutTruncated || described.stdoutBytes.size() > kDescribeLimit)
            throw ProjectBuildError("Behavior description exceeded its output limit", diagnostics);
        const auto schema = decodeBehaviorSchema(described.stdoutBytes, sdk.sdkBuildId);

        const auto after = snapshotSources(effectiveRequest, cancel);
        if (!sameSnapshot(snapshot, after))
            throw ProjectBuildError("Project Code changed while it was being built", diagnostics);
        writeSuccessMetadata(buildRoot, key, executableHash, sdk.sdkBuildId);
        report(progress, "complete", "project build and behavior discovery succeeded");
        return {executable, key, schema, described.stdoutBytes, std::move(diagnostics)};
    } catch (const ProjectBuildError&) {
        throw;
    } catch (const std::exception& error) {
        diagnostics.push_back({BuildDiagnosticLevel::Error, "build", error.what()});
        throw ProjectBuildError(error.what(), std::move(diagnostics));
    }
}

} // namespace proto
