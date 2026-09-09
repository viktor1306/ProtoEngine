#include "assets/AssetIO.hpp"
#include "core/Id.hpp"
#include "project/ProjectBuild.hpp"
#include "project/BuildFileTree.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace proto;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view behaviorId = "50000000-0000-4000-8000-000000000001";

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output), "fixture write failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(output), "fixture write failed");
}

void removeTree(const fs::path& path, std::error_code& error) {
    build_detail::removeTree(path, error);
}

std::string read(const fs::path& path) {
    const auto bytes = assetBytes(path, 2 * 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string source(bool throwing = false, bool hanging = false, bool compileError = false) {
    std::string result = R"cpp(#include <Proto/Behavior.hpp>
#include "Math.tcc"
#include <stdexcept>
#include <thread>
#include <chrono>
#ifdef M5_ENV_SHOULD_NOT_LEAK
#error ambient CXXFLAGS leaked into the packaged build
#endif
using namespace proto;
using namespace proto::sdk;
const auto spinId = BehaviorTypeId::parse("50000000-0000-4000-8000-000000000001");
static_assert(m5MathValue == 7);
class Spin final : public Behavior {};
void RegisterProjectBehaviors(BehaviorRegistry& registry) {
)cpp";
    if (compileError)
        result += "    this is an intentional compile error;\n";
    if (throwing)
        result += "    throw std::runtime_error(\"describe failure\");\n";
    if (hanging)
        result += "    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(10));\n";
    result += R"cpp(    registry.Add<Spin>({spinId, "Spin", {
        {"speed", PropertyType::Float, 30.0, -360.0, 360.0, "Speed"}
    }});
}
)cpp";
    return result;
}

ProjectBuildRequest request(const fs::path& project, const fs::path& sdk) {
    ProjectBuildRequest value;
    value.projectRoot = project;
    value.sdkRoot = sdk;
    value.configuration = "Debug";
    value.configureTimeout = std::chrono::minutes(2);
    value.buildTimeout = std::chrono::minutes(5);
    value.describeTimeout = std::chrono::seconds(5);
    return value;
}

class EnvironmentGuard final {
  public:
    EnvironmentGuard(const wchar_t* name, const wchar_t* value) : name_(name) {
        wchar_t buffer[4096]{};
        const auto length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        hadValue_ = length != 0 && length < std::size(buffer);
        if (hadValue_)
            oldValue_.assign(buffer, length);
        check(SetEnvironmentVariableW(name, value), "failed to set test environment");
    }
    ~EnvironmentGuard() { SetEnvironmentVariableW(name_.c_str(), hadValue_ ? oldValue_.c_str() : nullptr); }

  private:
    std::wstring name_;
    std::wstring oldValue_;
    bool hadValue_{};
};

std::string sdkConfiguration(const fs::path& sdkRoot) {
    const auto document = read(sdkRoot / "sdk.json");
    const auto key = document.find("\"configuration\"");
    check(key != std::string::npos, "SDK configuration field missing");
    auto cursor = document.find(':', key + 1);
    check(cursor != std::string::npos, "SDK configuration field malformed");
    while (++cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) {
    }
    check(cursor < document.size() && document[cursor] == '"', "SDK configuration must be a string");
    const auto begin = ++cursor;
    const auto end = document.find('"', begin);
    check(end != std::string::npos, "SDK configuration string is unterminated");
    return document.substr(begin, end - begin);
}

void replaceApiVersion(const fs::path& sdkRoot) {
    auto document = read(sdkRoot / "sdk.json");
    const auto key = document.find("\"behaviorApiVersion\"");
    check(key != std::string::npos, "SDK API field missing in test copy");
    auto cursor = document.find(':', key + 1);
    check(cursor != std::string::npos, "SDK API field malformed in test copy");
    ++cursor;
    while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor])))
        ++cursor;
    const auto end = document.find_first_not_of("0123456789", cursor);
    check(end != cursor, "SDK API field is not an integer in test copy");
    document.replace(cursor, end - cursor, "999");
    write(sdkRoot / "sdk.json", document);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "usage: M5BuildTests <output-root> <sdk-root>\n";
        return 2;
    }
    const auto output = fs::absolute(argv[1]).lexically_normal();
    const auto sdk = fs::absolute(argv[2]).lexically_normal();
    const auto project = output / utf8Path("m5-build-Україна spaces-" + Uuid::create().string().substr(0, 8));
    const auto wrongSdk = output / utf8Path("m5-sdk relocated Україна spaces-" + Uuid::create().string());
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
            if (const auto* build = dynamic_cast<const ProjectBuildError*>(&error))
                for (const auto& diagnostic : build->diagnostics())
                    std::cerr << diagnostic.stage << ": " << diagnostic.message << '\n';
        }
    };

    try {
        fs::create_directories(project / "Code");
        write(project / "Code/Math.tcc", "inline constexpr int m5MathValue = 7;\n");
        write(project / "Code/Behaviors.cpp", source());
        auto buildRequest = request(project, sdk);
        buildRequest.configuration = sdkConfiguration(sdk);
        std::atomic_bool cancel{false};

        ProjectBuildResult first;
        test("builds a real SDK consumer with no compiler on parent PATH and discovers Spin", [&] {
            EnvironmentGuard ambientFlags(L"CXXFLAGS", L"-DM5_ENV_SHOULD_NOT_LEAK -D_GLIBCXX_DEBUG");
            wchar_t systemDirectory[MAX_PATH]{};
            const auto systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            check(systemLength > 0 && systemLength < MAX_PATH, "Windows system directory lookup failed");
            EnvironmentGuard restrictedPath(L"PATH", systemDirectory);
            first = buildProject(buildRequest, cancel);
            wchar_t parentPath[MAX_PATH]{};
            check(GetEnvironmentVariableW(L"PATH", parentPath, MAX_PATH) == systemLength &&
                      std::wstring(parentPath) == systemDirectory,
                  "Project build changed the parent process PATH");
            check(fs::is_regular_file(first.executable), "Player executable missing");
            check(first.schema.sdkBuildId.size() > 8, "schema SDK identity missing");
            check(first.schema.find(BehaviorTypeId::parse(behaviorId)) != nullptr, "Spin schema missing");
            check(!first.key.empty() &&
                      fs::is_regular_file(first.executable.parent_path().parent_path().parent_path() / "success.json"),
                  "successful build metadata missing");
        });

        test("unchanged source reuses the compiled tree but refreshes schema discovery", [&] {
            const auto before = fs::last_write_time(first.executable);
            const auto cached = buildProject(buildRequest, cancel);
            check(cached.key == first.key, "unchanged source produced a different content key");
            check(fs::last_write_time(cached.executable) == before, "unchanged build relinked ProtoPlayer.exe");
            check(cached.schema.find(BehaviorTypeId::parse(behaviorId)) != nullptr,
                  "fresh unchanged-build schema discovery missed Spin");
        });

        test("the installed SDK package remains relocatable", [&] {
            fs::copy(sdk, wrongSdk, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            auto relocated = request(project, wrongSdk);
            relocated.configuration = sdkConfiguration(wrongSdk);
            const auto result = buildProject(relocated, cancel);
            check(result.schema.sdkBuildId == first.schema.sdkBuildId, "relocated SDK identity changed");
            check(result.schema.find(BehaviorTypeId::parse(behaviorId)) != nullptr,
                  "relocated SDK consumer missed Spin");
            std::error_code ignored;
            removeTree(wrongSdk, ignored);
            check(!ignored, "relocated SDK fixture cleanup failed");
        });

        test("compile diagnostics map to authored Code and never return a stale executable", [&] {
            write(project / "Code/Behaviors.cpp", source(false, false, true));
            try {
                (void)buildProject(buildRequest, cancel);
                throw std::runtime_error("compile error unexpectedly succeeded");
            } catch (const ProjectBuildError& error) {
                bool mapped{};
                for (const auto& diagnostic : error.diagnostics()) {
                    if (diagnostic.sourcePath && diagnostic.sourcePath->find("Code") != std::string::npos &&
                        diagnostic.line && *diagnostic.line > 0) {
                        mapped = true;
                        break;
                    }
                }
                check(mapped, "compile diagnostic did not preserve authored file and line");
            }
            check(fs::is_regular_file(first.executable), "previous successful executable was damaged");
        });

        test("cancellation leaves the project recoverable", [&] {
            write(project / "Code/Behaviors.cpp", source());
            cancel.store(true);
            try {
                (void)buildProject(buildRequest, cancel);
                throw std::runtime_error("cancelled build unexpectedly succeeded");
            } catch (const ProjectBuildError& error) {
                check(std::string(error.what()).find("cancel") != std::string::npos,
                      "cancellation failed at an unexpected stage");
            }
            cancel.store(false);
            const auto recovered = buildProject(buildRequest, cancel);
            check(recovered.schema.find(BehaviorTypeId::parse(behaviorId)) != nullptr, "recovery build missing Spin");
        });

        test("describe failure and timeout are rejected", [&] {
            write(project / "Code/Behaviors.cpp", source(true));
            try {
                (void)buildProject(buildRequest, cancel);
                throw std::runtime_error("throwing describe unexpectedly succeeded");
            } catch (const ProjectBuildError& error) {
                if (std::string(error.what()).find("Behavior description") == std::string::npos)
                    for (const auto& diagnostic : error.diagnostics())
                        std::cerr << diagnostic.stage << ": " << diagnostic.message << '\n';
                check(std::string(error.what()).find("Behavior description") != std::string::npos,
                      (std::string("throwing Player was rejected before describe: ") + error.what()).c_str());
            }
            write(project / "Code/Behaviors.cpp", source(false, true));
            auto timeout = buildRequest;
            timeout.describeTimeout = std::chrono::milliseconds(750);
            try {
                (void)buildProject(timeout, cancel);
                throw std::runtime_error("hung describe unexpectedly succeeded");
            } catch (const ProjectBuildError& error) {
                check(std::string(error.what()).find("timed out") != std::string::npos,
                      (std::string("hung Player was rejected before describe timeout: ") + error.what()).c_str());
            }
        });

        test("editor build identity mismatch is rejected before compilation", [&] {
            auto mismatch = buildRequest;
            mismatch.expectedSdkBuildId = "a-different-editor-build";
            bool rejected{};
            try {
                (void)buildProject(mismatch, cancel);
            } catch (const ProjectBuildError& error) {
                rejected = std::string(error.what()).find("identity") != std::string::npos;
            }
            check(rejected, "incompatible editor SDK was accepted");
        });
        test("wrong SDK API metadata is rejected before consumer build", [&] {
            fs::copy(sdk, wrongSdk, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            replaceApiVersion(wrongSdk);
            auto wrong = request(project, wrongSdk);
            try {
                (void)buildProject(wrong, cancel);
                throw std::runtime_error("wrong SDK API unexpectedly succeeded");
            } catch (const ProjectBuildError& error) {
                check(std::string(error.what()).find("Unsupported SDK") != std::string::npos,
                      "wrong SDK API was rejected for an unexpected reason");
            }
            std::error_code ignored;
            removeTree(wrongSdk, ignored);
            check(!ignored, "wrong SDK fixture cleanup failed");
        });

        write(project / "Code/Behaviors.cpp", source());
        std::error_code ignored;
        removeTree(project, ignored);
        check(!ignored && !fs::exists(project), "project fixture cleanup failed");
        removeTree(wrongSdk, ignored);
        check(!ignored && !fs::exists(wrongSdk), "SDK fixture cleanup failed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        removeTree(project, ignored);
        removeTree(wrongSdk, ignored);
        return 1;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
