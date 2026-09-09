#include "project/BuildFileTree.hpp"
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    if (argc != 3) return 2;
    const auto root = fs::absolute(argv[1]).lexically_normal().make_preferred();
    const auto sentinel = fs::absolute(argv[2]).lexically_normal().make_preferred();
    const fs::path extended(proto::build_detail::extendedFilePath(root));
    const auto file = extended / std::wstring(150, L'a') / std::wstring(160, L'b') / "probe.bin";
    fs::create_directories(file.parent_path());
    { std::ofstream output(file); output << "long-path fixture"; if (!output) return 3; }
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES || file.native().size() <= 260) return 4;
    std::error_code error;
    const auto removed = proto::build_detail::removeTree(root, error);
    if (error || GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES || removed != 5) {
        std::cerr << "Cleanup failed: " << error.message() << ", removed=" << removed << '\n';
        return 5;
    }
    std::ifstream input(sentinel / "keep.txt");
    std::string text;
    std::getline(input, text);
    if (text != "outside-sentinel") return 6;
    fs::create_directory(root);
    const auto lockedFile = root / "locked.txt";
    HANDLE locked = CreateFileW(lockedFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 7;
    const auto started = GetTickCount64();
    proto::build_detail::removeTree(root, error);
    const auto elapsed = GetTickCount64() - started;
    CloseHandle(locked);
    if (!error || elapsed > 1000) return 8;
    proto::build_detail::removeTree(root, error);
    if (error || fs::exists(root)) return 9;
    std::cout << "PASS: " << file.native().size() << "-character file removed; " << removed
              << " entries removed; junction target and sibling sentinel unchanged; locked-file failure returned in "
              << elapsed << " ms and retry succeeded.\n";
    return 0;
}
