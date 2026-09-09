#pragma once
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include <stdexcept>

namespace proto::testHelpers {
// NTFS directory junctions require no symlink privilege. Test fixtures use
// private empty directories, and remove only the junction with RemoveDirectoryW.
inline void makeJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    const auto absolute = std::filesystem::absolute(target).lexically_normal().wstring();
    const auto substitute = L"\\??\\" + absolute;
    if (!std::filesystem::create_directory(link))
        throw std::runtime_error("Junction fixture path exists");
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Junction fixture open failed");
    struct Header {
        DWORD tag;
        WORD length, reserved, subOffset, subLength, printOffset, printLength;
    };
    static_assert(sizeof(Header) == 16);
    const auto pathBytes = (substitute.size() + absolute.size() + 2) * sizeof(wchar_t);
    std::vector<std::byte> bytes(sizeof(Header) + pathBytes);
    Header header{IO_REPARSE_TAG_MOUNT_POINT,        WORD(8 + pathBytes),      0, 0, WORD(substitute.size() * 2),
                  WORD((substitute.size() + 1) * 2), WORD(absolute.size() * 2)};
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), substitute.c_str(), (substitute.size() + 1) * 2);
    std::memcpy(bytes.data() + sizeof(header) + header.printOffset, absolute.c_str(), (absolute.size() + 1) * 2);
    DWORD returned{};
    const BOOL success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(), DWORD(bytes.size()), nullptr, 0,
                                         &returned, nullptr);
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!success) {
        RemoveDirectoryW(link.c_str());
        throw std::runtime_error("Junction fixture failed: " + std::to_string(error));
    }
}
} // namespace proto::testHelpers
