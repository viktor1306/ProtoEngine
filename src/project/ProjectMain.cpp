#include "project/ProjectSession.hpp"
#include "core/Diagnostics.hpp"
#include <iostream>

// The packaged starter uses the same fresh-ID, no-overwrite transaction as
// File > New Project in the Editor. No copied manifest or generated build tree.
int wmain(int argc, wchar_t** argv) {
    using namespace proto;
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
            std::cout << "ProtoProject --create <new folder> --name <project name>\n";
            return 0;
        }
        std::filesystem::path destination;
        std::string name;
        bool hasDestination{}, hasName{};
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view option(argv[i]);
            if (option != L"--create" && option != L"--name")
                throw std::runtime_error("Unknown project creation option");
            if (++i >= argc)
                throw std::runtime_error("Missing project creation argument value");
            if (option == L"--create") {
                if (hasDestination)
                    throw std::runtime_error("Duplicate --create option");
                hasDestination = true;
                destination = argv[i];
            } else {
                if (hasName)
                    throw std::runtime_error("Duplicate --name option");
                hasName = true;
                name = utf8(argv[i]);
            }
        }
        if (!hasDestination || destination.empty() || !hasName || name.empty())
            throw std::runtime_error("ProtoProject requires --create <new folder> --name <project name>");
        const auto project = ProjectSession::create(destination, name);
        std::cout << "{\"passed\":true,\"project_root\":" << jsonString(utf8(project->root().wstring()))
                  << ",\"project_id\":" << jsonString(project->info().id.string())
                  << ",\"scene_id\":" << jsonString(project->info().startupScene.string()) << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
