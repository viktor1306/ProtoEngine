#include "project/ProjectPackage.hpp"
#include "core/Diagnostics.hpp"
#include <Proto/Build.hpp>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    using namespace proto;
    try {
        std::filesystem::path project, sdk, output;
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view option(argv[i]);
            if (option == L"--help") {
                std::cout << "ProtoPackage --project <folder> [--sdk <SDK folder>] [--output <folder>]\n";
                return 0;
            }
            if (++i >= argc)
                throw std::runtime_error("Missing export argument value");
            if (option == L"--project")
                project = argv[i];
            else if (option == L"--sdk")
                sdk = argv[i];
            else if (option == L"--output")
                output = argv[i];
            else
                throw std::runtime_error("Unknown export option");
        }
        if (project.empty())
            throw std::runtime_error("--project is required");
        auto session = ProjectSession::open(project);
        if (sdk.empty())
            sdk = executableDirectory().parent_path() / "sdk";
        if (output.empty())
            output = session->root() / "Build" / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(session->buildSettings().name.data()), session->buildSettings().name.size()));
        ProjectPackageRequest request;
        request.session = std::move(session);
        request.sdkRoot = std::move(sdk);
        request.outputDirectory = std::move(output);
        request.expectedSdkBuildId = proto::sdk::buildId;
        request.configuration = proto::sdk::configuration;
        std::atomic_bool cancel{};
        const auto result = buildProjectPackage(request, cancel, [](const BuildProgress& event) {
            std::cout << '[' << event.stage << "] " << event.message << std::endl;
        });
        std::cout << "{\"passed\":true,\"executable\":" << jsonString(utf8(result.executable.wstring()))
                  << ",\"files\":" << result.runtimeStats.files << ",\"bytes\":" << result.runtimeStats.bytes << "}\n";
        return 0;
    } catch (const ProjectPackageError& error) {
        std::cerr << error.what() << '\n';
        for (const auto& diagnostic : error.diagnostics())
            std::cerr << diagnostic.stage << ": " << diagnostic.message << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
