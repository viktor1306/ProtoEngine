#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace proto::build_detail {

inline std::wstring extendedFilePath(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
    if (absolute.starts_with(L"\\\\?\\"))
        return absolute;
    if (absolute.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    return L"\\\\?\\" + absolute;
}

// Only call for an owned build/fixture tree whose root has already been checked.
// This toolchain's CRT directory iterator can enumerate the working directory
// instead of a path beyond MAX_PATH, even with an extended prefix. Use native
// wide Win32 enumeration and stop immediately on an error; never follow links.
inline uintmax_t removeTree(const std::filesystem::path& root, std::error_code& error) {
    struct Pending {
        std::wstring path;
        bool childrenRemoved{};
    };
    struct FindHandle {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~FindHandle() {
            if (value != INVALID_HANDLE_VALUE)
                FindClose(value);
        }
    };
    error.clear();
    std::vector<Pending> pending{{extendedFilePath(root), false}};
    uintmax_t removed{};
    const auto failed = [&](DWORD code) {
        error = std::error_code(static_cast<int>(code), std::system_category());
        return static_cast<uintmax_t>(-1);
    };
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        const auto attributes = GetFileAttributesW(current.path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                continue;
            return failed(code);
        }
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!directory || reparse || current.childrenRemoved) {
            const auto deleted = directory ? RemoveDirectoryW(current.path.c_str()) : DeleteFileW(current.path.c_str());
            if (!deleted)
                return failed(GetLastError());
            ++removed;
            continue;
        }

        WIN32_FIND_DATAW data{};
        const auto search = current.path + L"\\*";
        FindHandle find{FindFirstFileW(search.c_str(), &data)};
        if (find.value == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND)
                return failed(code);
            pending.push_back({std::move(current.path), true});
            continue;
        }
        pending.push_back({current.path, true});
        do {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L"..")
                pending.push_back({current.path + L"\\" + std::wstring(name), false});
        } while (FindNextFileW(find.value, &data));
        const auto code = GetLastError();
        if (code != ERROR_NO_MORE_FILES)
            return failed(code);
    }
    return removed;
}

} // namespace proto::build_detail
